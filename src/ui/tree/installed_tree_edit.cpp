// installed_tree_edit.cpp
//
// The full tree-editing subsystem: shared pure-function foundation,
// per-category ops -- rename, create, move -- and per-effect
// ops -- edit, delete, move. All six state machines' structs and
// statics are file-local (wrapped in anonymous namespaces); addon.cpp
// reaches this file only through the accessor API in installed_tree_edit.h
// -- see that header for what's exposed and why. Extracted from addon.cpp
// -- a mechanical move, no behavior change
// beyond what was needed to cross the file boundary (raw static reads in
// addon.cpp became accessor calls).
#include "ui/tree/installed_tree_edit.h"
#include "core/tree/installed_tree_overlay.h" // JoinPath
#include "core/tree/installed_tree_store.h"   // FindInstalledJsonMutable, InvalidateInstalledTree, SaveInstalledSinFile
#include "addon/ui_colors.h"              // kDuplicateColor
#include "imgui.h"
#include <sstream>
#include <cstdio>

namespace {
// Shared across all six edit state machines (see GetEditResultMessage's
// doc comment in installed_tree_edit.h for why this lives here rather
// than with any one of them). Moved from addon.cpp when it became clear
// category rename/create/move couldn't each keep their own copy of what's
// meant to be one addon-wide "last result" message.
std::string s_editResultMessage;
}

void SetEditResultMessage(const std::string& message)
{
    s_editResultMessage = message;
}

void ClearEditResultMessage()
{
    s_editResultMessage.clear();
}

const std::string& GetEditResultMessage()
{
    return s_editResultMessage;
}

// Splits `text` into trimmed, non-empty lines. Used to turn the guids text
// box back into a guid list.
std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end   = line.find_last_not_of(" \t\r\n");
        if (start == std::string::npos)
            continue; // blank line
        out.push_back(line.substr(start, end - start + 1));
    }
    return out;
}

// True if `prefix` is `path`'s first N elements (N = prefix.size()),
// including the case where they're equal. Used to tell whether an
// in-progress edit lives at or underneath a category that's about to be
// collapsed -- see the "cancel on collapse" checks in RenderCategoryTree.
bool PathHasPrefix(const std::vector<int>& path, const std::vector<int>& prefix)
{
    if (prefix.size() > path.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (path[i] != prefix[i])
            return false;
    return true;
}

// Read-only category lookup by index path, starting from `root` (a sin
// file's top-level json, which has its own "categories" array exactly
// like any other category node). Each element of `path` is that level's
// position within its parent's "categories" array at the moment the path
// was captured during a tree walk (see `pathSoFar` in RenderCategoryTree)
// -- category identity is index-based, mirroring how effect edit/drag
// identity already works (see EditState::originalIndex's comment), so
// same-named sibling categories can never collide here the way they used
// to when this matched by name. Returns nullptr if any segment is out of
// range for the array at that point -- callers treat that as "the tree
// changed since editing started, don't guess," same as the old
// missing-name case.
nlohmann::ordered_json* FindCategoryByPath(nlohmann::ordered_json& root, const std::vector<int>& path)
{
    nlohmann::ordered_json* cursor = &root;
    for (int idx : path)
    {
        if (!cursor->contains("categories") || !(*cursor)["categories"].is_array())
            return nullptr;

        auto& cats = (*cursor)["categories"];
        if (idx < 0 || static_cast<size_t>(idx) >= cats.size())
            return nullptr;

        cursor = &cats[idx];
    }
    return cursor;
}

// Same walk as FindCategoryByPath, but collects each step's "name" field
// instead of returning a pointer, joined the same way JoinPath formats a
// typed destination ("Combat / Downstate") -- used only for display
// (result/error messages), never for identity. Meant to be called right
// after a FindCategoryByPath lookup on the same path has already
// succeeded; if the tree were to change in between, this just resolves as
// much as it still can rather than asserting.
std::string JoinCategoryPathNames(const nlohmann::ordered_json& root, const std::vector<int>& path)
{
    std::vector<std::string> names;
    const nlohmann::ordered_json* cursor = &root;
    for (int idx : path)
    {
        if (!cursor->contains("categories") || !(*cursor)["categories"].is_array())
            break;
        const auto& cats = (*cursor)["categories"];
        if (idx < 0 || static_cast<size_t>(idx) >= cats.size())
            break;
        cursor = &cats[idx];
        names.push_back(cursor->value("name", std::string("(unnamed category)")));
    }
    return JoinPath(names);
}

// ---------------------------------------------------------------------------
// Category rename, create category, and move category. Moved
// out of addon.cpp mechanically -- struct/statics unchanged, function
// bodies unchanged -- except where the tree (RenderCategoryTree /
// RenderInstalledEffects, still in addon.cpp) used to read
// this file's statics directly. Those reads are now the small set of
// query/accessor functions declared in installed_tree_edit.h instead
// (IsCategoryBeingRenamed, IsCategoryRenameUnderPath, GetCategoryDragSinName/
// Path, QueueCategoryMove, etc.) -- see each one's call site in addon.cpp
// for exactly what it replaced.
// ---------------------------------------------------------------------------

namespace {

// Category rename -- a deliberately much smaller sibling of the effect
// editor (below). Only the category's own "name" field is
// editable; moving a category (i.e. changing its parent) isn't offered
// here since that's a bigger, riskier operation (it would silently take
// every effect and subcategory underneath it along for the ride) and
// nothing's asked for that yet. Shares the same "only one edit in flight
// addon-wide" rule as effect editing -- so an effect edit and a category
// rename can never be open at the same time either.
struct CategoryEditState
{
    bool                      active = false;
    std::string               sinName;
    std::vector<int>          path; // this category's own identity, root -> ... -> this category, inclusive
    char                       nameBuf[256] = {};
};
static CategoryEditState s_categoryEdit;

struct CategoryRenameJob
{
    std::string               sinName;
    std::vector<int>          path; // root -> ... -> this category, inclusive (same convention as CategoryEditState::path)
    std::string               newName;
};
static bool              s_hasPendingCategoryRename = false;
static CategoryRenameJob s_pendingCategoryRename;

// "Add category" prompt state. Rendered inside the parent category's
// TreeNode (same idea as RenderCategoryEditor for rename), so -- unlike a
// delete confirmation -- this DOES get cancelled on collapse, same
// reasoning as CategoryEditState above.
struct CreateCategoryState
{
    bool                      active = false;
    std::string               sinName;
    std::vector<int>          parentPath; // where the new category goes; empty means this sin file's top level
    char                       nameBuf[256] = {};
};
static CreateCategoryState s_createCategory;

struct CreateCategoryJob
{
    std::string               sinName;
    std::vector<int>          parentPath;
    std::string                newName;
};
static bool               s_hasPendingCreateCategory = false;
static CreateCategoryJob  s_pendingCreateCategory;

// Payload for dragging a category itself, as opposed to an effect -- same
// "ImGui payload is just a fixed-size type marker, the real data lives in
// a static struct kept current every frame BeginDragDropSource runs"
// reasoning the effect drag payload uses (below).
struct CategoryDragPayload
{
    std::string       sinName;
    std::vector<int>  path; // this category's own identity, root -> ... -> this category, inclusive -- path.back() is this category's own index within its parent's "categories" array, path.begin()..end()-1 is that parent's own path
};
static CategoryDragPayload s_categoryDragPayload;
static const int           kCategoryDragMarker = 1; // payload bytes are a placeholder; see CategoryDragPayload's comment

// Reorder-only (see this section's header comment in installed_tree_edit.h):
// a category's destination is always its own *current* parent's
// "categories" array, so unlike an effect move there's no separate
// destinationPath -- only where within that unchanged list it ends up.
struct CategoryMoveJob
{
    std::string       sinName;
    std::vector<int>  originalPath; // this category's own identity at drag time -- see CategoryDragPayload::path
    int               destinationIndex = -1; // -1 means "append" (dropped on the shared parent's own row -- that parent's own TreeNode for a nested category, or the sin file's root row for a top-level one); otherwise the sibling index, within that same parent's "categories" array *as captured at drop time, before the source erase*, that this category should end up immediately above (dropped on that sibling's own row) -- see ApplyPendingCategoryMove for the same erase-shift adjustment an effect move's destinationIndex needed
};
static bool            s_hasPendingCategoryMove = false;
static CategoryMoveJob s_pendingCategoryMove;

} // namespace

// Populates the category-rename buffer and marks it active. `path` is
// this category's own path (root -> ... -> this category, inclusive).
void BeginCategoryEdit(const std::string& sinName, const std::vector<int>& path, const std::string& currentName)
{
    s_categoryEdit.active  = true;
    s_categoryEdit.sinName = sinName;
    s_categoryEdit.path    = path;
    std::snprintf(s_categoryEdit.nameBuf, sizeof(s_categoryEdit.nameBuf), "%s", currentName.c_str());
    ClearEditResultMessage();
}

void CancelCategoryEdit()
{
    s_categoryEdit.active = false;
    ClearEditResultMessage();
}

// Draws the rename widget for whichever category is currently being
// renamed. Only called from inside that one category's TreeNode. As with
// the effect editor, Save only records `s_pendingCategoryRename` -- the
// actual json mutation happens in ApplyPendingCategoryRename, after the
// whole tree has finished rendering for this frame.
void RenderCategoryEditor()
{
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("Renaming this category.");
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name##category", s_categoryEdit.nameBuf, sizeof(s_categoryEdit.nameBuf));

    if (ImGui::Button("Save##category"))
    {
        std::string trimmed = s_categoryEdit.nameBuf;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        size_t end   = trimmed.find_last_not_of(" \t\r\n");
        trimmed = (start == std::string::npos) ? std::string() : trimmed.substr(start, end - start + 1);

        if (trimmed.empty())
        {
            SetEditResultMessage("Rename not saved: category name can't be empty.");
        }
        else
        {
            CategoryRenameJob job;
            job.sinName = s_categoryEdit.sinName;
            job.path    = s_categoryEdit.path;
            job.newName = trimmed;

            s_pendingCategoryRename    = std::move(job);
            s_hasPendingCategoryRename = true;
            s_categoryEdit.active      = false; // this node's path may change/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##category"))
        CancelCategoryEdit();
}

// Applies a previously-recorded category rename to the in-memory json and
// writes it to disk. Re-finds the category by its recorded path right
// before mutating -- same reasoning as ApplyPendingEdit. Safe to
// call unconditionally every frame; no-ops if nothing is pending.
void ApplyPendingCategoryRename()
{
    if (!s_hasPendingCategoryRename)
        return;
    s_hasPendingCategoryRename = false;

    const CategoryRenameJob& job = s_pendingCategoryRename;

    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        SetEditResultMessage("Rename failed: " + job.sinName + " is no longer loaded.");
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    nlohmann::ordered_json* category = FindCategoryByPath(root, job.path);
    if (!category)
    {
        SetEditResultMessage("Rename failed: this category is no longer there.");
        return;
    }

    (*category)["name"] = job.newName;

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        SetEditResultMessage("Rename applied in memory but failed to write to disk (" + writeError +
                              "). Reloading from disk so nothing shown is out of sync with what's actually saved.");
        InvalidateInstalledTree();
        return;
    }

    SetEditResultMessage("Renamed category to \"" + job.newName + "\".");
    InvalidateInstalledTree(); // force a clean reload from disk next expand
}

bool IsCategoryBeingRenamed(const std::string& sinName, const std::vector<int>& path)
{
    return s_categoryEdit.active && s_categoryEdit.sinName == sinName && s_categoryEdit.path == path;
}

bool IsCategoryRenameUnderPath(const std::string& sinName, const std::vector<int>& path)
{
    return s_categoryEdit.active && s_categoryEdit.sinName == sinName && PathHasPrefix(s_categoryEdit.path, path);
}

bool IsCategoryRenameActive()
{
    return s_categoryEdit.active;
}

// Populates the create-category state and marks it active. `parentPath` is
// where the new category will go -- empty means this sin file's top level.
void BeginCreateCategory(const std::string& sinName, const std::vector<int>& parentPath)
{
    s_createCategory.active      = true;
    s_createCategory.sinName     = sinName;
    s_createCategory.parentPath  = parentPath;
    s_createCategory.nameBuf[0]  = '\0';
    ClearEditResultMessage();
}

void CancelCreateCategory()
{
    s_createCategory = CreateCategoryState();
}

// Draws the inline "new subcategory" prompt for whichever category is
// currently the target parent. Only records `s_pendingCreateCategory` --
// the actual creation happens in ApplyPendingCreateCategory, after the
// whole tree has finished rendering for this frame.
void RenderCreateCategoryEditor()
{
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("New subcategory.");
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name##newcategory", s_createCategory.nameBuf, sizeof(s_createCategory.nameBuf));

    if (ImGui::Button("Create##newcategory"))
    {
        std::string trimmed = s_createCategory.nameBuf;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        size_t end   = trimmed.find_last_not_of(" \t\r\n");
        trimmed = (start == std::string::npos) ? std::string() : trimmed.substr(start, end - start + 1);

        if (trimmed.empty())
        {
            SetEditResultMessage("Not created: category name can't be empty.");
        }
        else
        {
            CreateCategoryJob job;
            job.sinName    = s_createCategory.sinName;
            job.parentPath = s_createCategory.parentPath;
            job.newName    = trimmed;

            s_pendingCreateCategory    = std::move(job);
            s_hasPendingCreateCategory = true;
            s_createCategory.active    = false; // this node's path may change/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##newcategory"))
        CancelCreateCategory();
}

// Applies a previously-recorded category creation to the in-memory json
// and writes it to disk. Re-finds the parent by path right before
// mutating -- same reasoning as every other Apply* function here.
// Deliberately errors out rather than silently reusing an existing
// same-named sibling: the user asked to create a specific new category,
// so finding one already there is worth surfacing, not hiding. Safe to
// call unconditionally every frame; no-ops if nothing is pending.
void ApplyPendingCreateCategory()
{
    if (!s_hasPendingCreateCategory)
        return;
    s_hasPendingCreateCategory = false;

    const CreateCategoryJob& job = s_pendingCreateCategory;

    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        SetEditResultMessage("Create failed: " + job.sinName + " is no longer loaded.");
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    nlohmann::ordered_json* parent = job.parentPath.empty() ? &root : FindCategoryByPath(root, job.parentPath);
    if (!parent)
    {
        SetEditResultMessage("Create failed: the parent category is no longer there.");
        return;
    }

    if (!parent->contains("categories") || !(*parent)["categories"].is_array())
        (*parent)["categories"] = nlohmann::ordered_json::array();

    for (const auto& sub : (*parent)["categories"])
    {
        if (sub.contains("name") && sub["name"] == job.newName)
        {
            SetEditResultMessage("Create failed: \"" + job.newName + "\" already exists here.");
            return;
        }
    }

    nlohmann::ordered_json fresh;
    fresh["name"] = job.newName;
    (*parent)["categories"].push_back(std::move(fresh));

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        SetEditResultMessage("Create applied in memory but failed to write to disk (" + writeError +
                              "). Reloading from disk so nothing shown is out of sync with what's actually saved.");
        InvalidateInstalledTree();
        return;
    }

    SetEditResultMessage("Created category \"" + job.newName + "\".");
    InvalidateInstalledTree(); // force a clean reload from disk next expand
}

bool IsCreatingCategoryAt(const std::string& sinName, const std::vector<int>& parentPath)
{
    return s_createCategory.active && s_createCategory.sinName == sinName && s_createCategory.parentPath == parentPath;
}

bool IsCategoryCreateUnderPath(const std::string& sinName, const std::vector<int>& path)
{
    return s_createCategory.active && s_createCategory.sinName == sinName && PathHasPrefix(s_createCategory.parentPath, path);
}

bool IsCreateCategoryActive()
{
    return s_createCategory.active;
}

// Records that the category at `path` (in `sinName`) is the one currently
// being dragged, and hands ImGui the fixed-size marker payload -- call
// from inside ImGui::BeginDragDropSource(), same as before this moved.
void BeginCategoryDrag(const std::string& sinName, const std::vector<int>& path)
{
    s_categoryDragPayload.sinName = sinName;
    s_categoryDragPayload.path    = path;
    ImGui::SetDragDropPayload("VFXD_CATEGORY", &kCategoryDragMarker, sizeof(kCategoryDragMarker));
}

const std::string& GetCategoryDragSinName()
{
    return s_categoryDragPayload.sinName;
}

const std::vector<int>& GetCategoryDragPath()
{
    return s_categoryDragPayload.path;
}

void QueueCategoryMove(const std::string& sinName, const std::vector<int>& originalPath, int destinationIndex)
{
    CategoryMoveJob job;
    job.sinName          = sinName;
    job.originalPath     = originalPath;
    job.destinationIndex = destinationIndex;

    s_pendingCategoryMove    = std::move(job);
    s_hasPendingCategoryMove = true;
}

// Applies a previously-recorded drag-and-drop reorder (see QueueCategoryMove)
// to the in-memory json and writes it to disk. Reorder-only, by design (see
// this file's category-move header comment) -- the category never leaves
// its own current parent's "categories" array, so unlike an effect move
// there's only ever one array in play here, not a separate source and
// destination. Re-finds that array, and the category's position in it, by
// path/index right before mutating -- same reasoning as every other
// Apply* function here. Safe to call unconditionally every frame; no-ops
// if nothing is pending.
void ApplyPendingCategoryMove()
{
    if (!s_hasPendingCategoryMove)
        return;
    s_hasPendingCategoryMove = false;

    const CategoryMoveJob& job = s_pendingCategoryMove;

    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        SetEditResultMessage("Reorder failed: " + job.sinName + " is no longer loaded.");
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    if (job.originalPath.empty())
    {
        SetEditResultMessage("Reorder failed: nothing to reorder.");
        return;
    }
    std::vector<int> parentPath(job.originalPath.begin(), job.originalPath.end() - 1);
    int              originalIndex = job.originalPath.back();
    nlohmann::ordered_json* parent = parentPath.empty() ? &root : FindCategoryByPath(root, parentPath);
    if (!parent || !parent->contains("categories") || !(*parent)["categories"].is_array())
    {
        SetEditResultMessage("Reorder failed: the category's parent is no longer there.");
        return;
    }

    auto& siblings = (*parent)["categories"];
    // Indexed lookup, not a name search -- same reasoning as
    // ApplyPendingEdit/ApplyPendingDelete's category branch: two
    // sibling categories can share a name, and a name search would
    // silently grab whichever one comes first.
    if (originalIndex < 0 || static_cast<size_t>(originalIndex) >= siblings.size())
    {
        SetEditResultMessage("Reorder failed: the category is no longer there.");
        return;
    }
    auto        it   = siblings.begin() + originalIndex;
    std::string name = it->value("name", std::string());

    nlohmann::ordered_json categoryCopy = std::move(*it);
    siblings.erase(it);

    // destinationIndex == -1 means "append" (dropped on the shared
    // parent's own row); otherwise "insert immediately before this
    // index," captured against `siblings` *before* the erase just above
    // ran, so (since source and destination are always this same array
    // here) an index that came from later than originalIndex needs the
    // same -1 shift an effect move's same-category case needed, for the
    // same reason: everything after originalIndex just moved down by one.
    if (job.destinationIndex < 0)
    {
        siblings.push_back(std::move(categoryCopy));
    }
    else
    {
        int insertIndex = job.destinationIndex;
        if (originalIndex < insertIndex)
            insertIndex -= 1;

        // Defensive clamp -- see the equivalent effect-move comment
        // for why this is worth having even though the no-op guards
        // at drop time shouldn't let a bad index reach here.
        if (insertIndex < 0)
            insertIndex = 0;
        if (static_cast<size_t>(insertIndex) > siblings.size())
            insertIndex = static_cast<int>(siblings.size());

        siblings.insert(siblings.begin() + insertIndex, std::move(categoryCopy));
    }

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        SetEditResultMessage("Reordered \"" + name + "\" in memory but failed to write to disk (" + writeError +
                              "). Reloading from disk so nothing shown is out of sync with what's actually saved.");
        InvalidateInstalledTree(); // force a clean reload; don't trust the in-memory copy after a failed write
        return;
    }

    SetEditResultMessage("Reordered \"" + name + "\".");
    InvalidateInstalledTree(); // force a clean reload from disk next expand, same as after an applied update
}

// ---------------------------------------------------------------------------
// Per-effect ops: EditState/EditSaveJob, DeleteConfirmState/
// DeleteJob, and EffectDragPayload/EffectMoveJob, plus every
// Begin*/Cancel*/Render*/Apply* function that goes with them. Moved out of
// addon.cpp mechanically -- see installed_tree_edit.h for the accessor API
// that replaces addon.cpp's old direct reads of these statics.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Effect editor -- lets a user pick one effect in the installed tree and
// makes that one node's fields editable. Scoped deliberately narrow to this
// addon's own concerns -- name, description, guids, and category placement.
// Behaviors (Hide/Show/SetDuration) are NEVER made editable here; those stay
// owned by VfxDenoiser's own UI, always rendered read-only via RenderBehavior
// regardless of edit state.
//
// Only one edit can be in flight at a time, addon-wide -- mirrors the
// existing single-in-flight-request philosophy in github_update.cpp, and
// avoids the ambiguity of two half-finished edits landing in whatever order
// imgui happens to render them.
// ---------------------------------------------------------------------------

struct EditState
{
    bool                      active = false;
    std::string               sinName;
    std::vector<int>          originalPath; // category identity: root -> immediate parent, each element that level's index within its parent's "categories" array (see FindCategoryByPath)
    std::string               originalName; // effect's name at the moment editing started, used for display/messages and as a sanity check on save
    int                        originalIndex = -1; // this effect's position within originalPath's "effects" array at BeginEdit time -- the actual identity key, since two sibling effects can share a name

    char nameBuf[256]         = {};
    char descBuf[1024]        = {};
    char guidsBuf[2048]       = {}; // one guid per line
};
static EditState s_edit;

// Set by the Save button; consumed once, after the whole tree has finished
// rendering for this frame, so the effect array is never mutated mid-walk.
struct EditSaveJob
{
    std::string               sinName;
    std::vector<int>          originalPath; // see EditState::originalPath
    std::string               originalName;
    int                        originalIndex = -1; // position within originalPath's "effects" array -- see EditState::originalIndex
    std::string                newName;
    std::string                newDescription;
    std::vector<std::string>   newGuids;
};
static bool        s_hasPendingSave = false;
static EditSaveJob s_pendingSave;

void BeginEdit(const std::string& sinName, const std::vector<int>& path,
               int index, const nlohmann::ordered_json& effect)
{
    s_edit.active        = true;
    s_edit.sinName       = sinName;
    s_edit.originalPath  = path;
    s_edit.originalName  = effect.value("name", std::string());
    s_edit.originalIndex = index;

    std::snprintf(s_edit.nameBuf, sizeof(s_edit.nameBuf), "%s", s_edit.originalName.c_str());

    std::string desc = effect.value("description", std::string());
    std::snprintf(s_edit.descBuf, sizeof(s_edit.descBuf), "%s", desc.c_str());

    std::string guidsJoined;
    if (effect.contains("guids") && effect["guids"].is_array())
    {
        for (const auto& g : effect["guids"])
        {
            if (!g.is_string())
                continue;
            if (!guidsJoined.empty())
                guidsJoined += "\n";
            guidsJoined += g.get<std::string>();
        }
    }
    std::snprintf(s_edit.guidsBuf, sizeof(s_edit.guidsBuf), "%s", guidsJoined.c_str());

    s_editResultMessage.clear();
}

void CancelEdit()
{
    s_edit.active = false;
    s_editResultMessage.clear();
}

bool IsEffectBeingEdited(const std::string& sinName, const std::vector<int>& path, int index)
{
    return s_edit.active && s_edit.sinName == sinName &&
           s_edit.originalPath == path && s_edit.originalIndex == index;
}

bool IsEffectEditUnderPath(const std::string& sinName, const std::vector<int>& path)
{
    return s_edit.active && s_edit.sinName == sinName && PathHasPrefix(s_edit.originalPath, path);
}

bool IsEffectEditActive()
{
    return s_edit.active;
}

// Draws the editable widgets for whichever effect is currently being
// edited. Only called from inside that one effect's TreeNode. Saving here
// never touches the json directly -- it just records `s_pendingSave` and
// sets `s_hasPendingSave`, so the actual mutation happens once, safely,
// after the whole tree has finished walking for this frame (see
// ApplyPendingEdit).
void RenderEffectEditor()
{
    // Fixed width for all edit fields -- without this, ImGui::InputText
    // stretches to fill the whole options-panel width, which reads as
    // way too wide for short values like a name or a guid.
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("Editing this addon's own fields. Hide/Show/duration stay owned by VfxDenoiser.");

    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name", s_edit.nameBuf, sizeof(s_edit.nameBuf));
    ImGui::InputTextMultiline("Description", s_edit.descBuf, sizeof(s_edit.descBuf), ImVec2(kFieldWidth, 60));
    ImGui::TextDisabled("GUIDs (one per line):");
    ImGui::InputTextMultiline("##guids", s_edit.guidsBuf, sizeof(s_edit.guidsBuf), ImVec2(kFieldWidth, 80));

    if (ImGui::Button("Save"))
    {
        std::string trimmedName = s_edit.nameBuf;
        size_t start = trimmedName.find_first_not_of(" \t\r\n");
        size_t end   = trimmedName.find_last_not_of(" \t\r\n");
        trimmedName  = (start == std::string::npos) ? std::string() : trimmedName.substr(start, end - start + 1);

        if (trimmedName.empty())
        {
            s_editResultMessage = "Edit not saved: name can't be empty.";
        }
        else
        {
            EditSaveJob job;
            job.sinName         = s_edit.sinName;
            job.originalPath    = s_edit.originalPath;
            job.originalName    = s_edit.originalName;
            job.originalIndex   = s_edit.originalIndex;
            job.newName         = trimmedName;
            job.newDescription  = s_edit.descBuf;
            job.newGuids        = SplitLines(s_edit.guidsBuf);

            s_pendingSave    = std::move(job);
            s_hasPendingSave = true;
            s_edit.active    = false; // the node this refers to may move/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        CancelEdit();
}

// Applies a previously-recorded edit (see RenderEffectEditor) to the
// in-memory json and writes it to disk. Deliberately re-finds the source
// category and effect by path/name right before mutating, rather than
// carrying a pointer from render time -- same reasoning as
// ApplyMergePlan's re-derived index in merge.cpp: nothing else is supposed
// to mutate the store's copy between "Save was clicked" and "this runs"
// (both happen within the same frame), but re-deriving costs nothing and
// removes any doubt.
void ApplyPendingEdit()
{
    if (!s_hasPendingSave)
        return;
    s_hasPendingSave = false;

    const EditSaveJob& job = s_pendingSave;

    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        s_editResultMessage = "Edit failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.originalPath);
    if (!srcCategory || !srcCategory->contains("effects") || !(*srcCategory)["effects"].is_array())
    {
        s_editResultMessage = "Edit failed: the original category is no longer there.";
        return;
    }

    auto& effectsArr = (*srcCategory)["effects"];
    // Indexed lookup, not a name search: two sibling effects can share a
    // name, and a name search would silently grab whichever
    // one happens to come first, editing the wrong effect. originalIndex
    // is this effect's position at BeginEdit time, which can't have
    // shifted since -- nothing else is allowed to mutate this array while
    // an edit is in flight (see this function's own header comment). The
    // name check is just a defensive sanity check, not the identity.
    if (job.originalIndex < 0 || static_cast<size_t>(job.originalIndex) >= effectsArr.size())
    {
        s_editResultMessage = "Edit failed: effect \"" + job.originalName + "\" is no longer there.";
        return;
    }
    auto it = effectsArr.begin() + job.originalIndex;
    if (!it->contains("name") || (*it)["name"] != job.originalName)
    {
        s_editResultMessage = "Edit failed: effect \"" + job.originalName + "\" is no longer there.";
        return;
    }

    // Apply the edited fields. Note "guids" is always written, even if
    // empty -- an edit can legitimately clear all guids (though see
    // merge.h: a guid-less effect then can't be tracked across a future
    // update and will simply be skipped, same as any other guid-less
    // effect). Category placement is never touched here -- that's
    // drag-and-drop's job exclusively now (see BeginEdit's comment) -- so
    // this always mutates the existing array element in place, same as
    // category rename does for "name." No erase/re-insert, so no chance
    // of reordering the effect within its category.
    nlohmann::ordered_json guidsArr = nlohmann::ordered_json::array();
    for (const auto& g : job.newGuids)
        guidsArr.push_back(g);

    (*it)["name"] = job.newName;
    if (job.newDescription.empty())
        it->erase("description");
    else
        (*it)["description"] = job.newDescription;
    (*it)["guids"] = std::move(guidsArr);

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Edit applied in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        InvalidateInstalledTree(); // force a clean reload; don't trust the in-memory copy after a failed write
        return;
    }

    s_editResultMessage  = "Saved changes to \"" + job.newName + "\".";
    InvalidateInstalledTree(); // force a clean reload from disk next expand, same as after an applied update
}

// ---------------------------------------------------------------------------
// Delete (effects always; categories only when empty). Same "record state
// while rendering, apply once after the whole tree has finished this frame"
// shape as the effect editor above.
// ---------------------------------------------------------------------------

// Confirmation state for a pending delete. Rendered inline right next to
// the item's own row (see the "-" SmallButton in RenderCategoryTree),
// which is always visible whether or not that row's TreeNode is open --
// so unlike EditState, this deliberately does NOT get cancelled just
// because the owning node is collapsed; nothing about it was ever hidden
// by collapsing in the first place.
struct DeleteConfirmState
{
    bool                      active     = false;
    bool                      isCategory = false;
    std::string               sinName;
    std::vector<int>          path;        // effect: containing category's identity; category: this category's OWN identity, inclusive (see EditState::originalPath)
    int                       index       = -1; // effect index within path's "effects" array -- see EditState::originalIndex; unused for a category delete
    std::string                displayName; // just for the confirmation text/messages
};
static DeleteConfirmState s_deleteConfirm;

struct DeleteJob
{
    bool                      isCategory = false;
    std::string               sinName;
    std::vector<int>          path;
    int                       index = -1;
    std::string                name; // sanity check at apply time -- see ApplyPendingDelete
};
static bool      s_hasPendingDelete = false;
static DeleteJob s_pendingDelete;

// Populates the delete-confirmation state and marks it active. `path` is
// the effect's containing category path (effect delete) or the category's
// own path, inclusive (category delete); `index` is the effect's position
// within that path's "effects" array, unused for a category delete.
void BeginDeleteConfirm(const std::string& sinName, const std::vector<int>& path, int index,
                         bool isCategory, const std::string& displayName)
{
    s_deleteConfirm.active      = true;
    s_deleteConfirm.isCategory  = isCategory;
    s_deleteConfirm.sinName     = sinName;
    s_deleteConfirm.path        = path;
    s_deleteConfirm.index       = index;
    s_deleteConfirm.displayName = displayName;
    s_editResultMessage.clear();
}

void CancelDeleteConfirm()
{
    s_deleteConfirm = DeleteConfirmState();
}

bool IsDeletingThisCategory(const std::string& sinName, const std::vector<int>& path)
{
    return s_deleteConfirm.active && s_deleteConfirm.isCategory &&
           s_deleteConfirm.sinName == sinName && s_deleteConfirm.path == path;
}

bool IsDeletingThisEffect(const std::string& sinName, const std::vector<int>& path, int index)
{
    return s_deleteConfirm.active && !s_deleteConfirm.isCategory &&
           s_deleteConfirm.sinName == sinName &&
           s_deleteConfirm.path == path && s_deleteConfirm.index == index;
}

bool IsDeleteConfirmActive()
{
    return s_deleteConfirm.active;
}

bool IsDeleteConfirmUnderPath(const std::string& sinName, const std::vector<int>& path)
{
    return s_deleteConfirm.active && s_deleteConfirm.sinName == sinName &&
           PathHasPrefix(s_deleteConfirm.path, path);
}

// Draws the inline "Delete X? [Delete] [Cancel]" prompt for whichever
// effect/category is currently pending delete. As with every other editor
// here, this only records `s_pendingDelete` -- the actual removal happens
// in ApplyPendingDelete, after the whole tree has finished rendering for
// this frame.
void RenderDeleteConfirm()
{
    ImGui::TextColored(kDuplicateColor, "Delete \"%s\"? A .bak is kept, but there's no in-app undo for this.",
                        s_deleteConfirm.displayName.c_str());
    if (ImGui::SmallButton("Delete##confirm"))
    {
        DeleteJob job;
        job.isCategory = s_deleteConfirm.isCategory;
        job.sinName    = s_deleteConfirm.sinName;
        job.path       = s_deleteConfirm.path;
        job.index      = s_deleteConfirm.index;
        job.name       = s_deleteConfirm.displayName;

        s_pendingDelete        = std::move(job);
        s_hasPendingDelete     = true;
        s_deleteConfirm.active = false; // this node's path may change/disappear on the next reload
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel##delete"))
        CancelDeleteConfirm();
}

// Applies a previously-recorded delete to the in-memory json and writes it
// to disk. Re-finds the target right before mutating -- same reasoning as
// every other Apply* function above -- and, for a category, re-checks
// it's still empty right here too, in case anything changed between the
// confirm prompt and now (nothing can in practice, since only one
// edit/delete/create/rename is ever in flight addon-wide, but this is the
// authoritative check either way, not the confirm prompt).
void ApplyPendingDelete()
{
    if (!s_hasPendingDelete)
        return;
    s_hasPendingDelete = false;

    const DeleteJob& job = s_pendingDelete;

    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        s_editResultMessage = "Delete failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    if (job.isCategory)
    {
        if (job.path.empty())
        {
            s_editResultMessage = "Delete failed: nothing to delete.";
            return;
        }
        std::vector<int> parentPath(job.path.begin(), job.path.end() - 1);
        int myIndex = job.path.back();
        nlohmann::ordered_json* parent = parentPath.empty() ? &root : FindCategoryByPath(root, parentPath);
        if (!parent || !parent->contains("categories") || !(*parent)["categories"].is_array())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }

        auto& siblings = (*parent)["categories"];
        // Indexed lookup, not a name search -- same reasoning as the
        // effect-delete branch below: two sibling categories can share a
        // name, and a name search would silently grab whichever one comes
        // first. myIndex is this category's position at BeginDeleteConfirm
        // time, which can't have shifted since (only one edit/delete/
        // create/rename is ever in flight addon-wide). The name check is
        // just a defensive sanity check, not the identity.
        if (myIndex < 0 || static_cast<size_t>(myIndex) >= siblings.size())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        auto it = siblings.begin() + myIndex;
        if (!it->contains("name") || (*it)["name"] != job.name)
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        bool hasEffects = it->contains("effects") && !(*it)["effects"].empty();
        bool hasSubcats = it->contains("categories") && !(*it)["categories"].empty();
        if (hasEffects || hasSubcats)
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" isn't empty anymore.";
            return;
        }
        siblings.erase(it);
    }
    else
    {
        nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.path);
        if (!srcCategory || !srcCategory->contains("effects") || !(*srcCategory)["effects"].is_array())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        auto& effectsArr = (*srcCategory)["effects"];
        // Indexed lookup, not a name search -- same reasoning as
        // ApplyPendingEdit/ApplyPendingMove above.
        if (job.index < 0 || static_cast<size_t>(job.index) >= effectsArr.size())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        auto it = effectsArr.begin() + job.index;
        if (!it->contains("name") || (*it)["name"] != job.name)
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        effectsArr.erase(it);
    }

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Delete applied in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        InvalidateInstalledTree();
        return;
    }

    s_editResultMessage   = "Deleted \"" + job.name + "\".";
    InvalidateInstalledTree(); // force a clean reload from disk next expand
}

// ---------------------------------------------------------------------------
// Effect move (drag-and-drop). See EffectMoveJob (installed_tree_edit.h)
// for the destinationIndex convention shared with the category-move job.
// ---------------------------------------------------------------------------

static EffectDragPayload s_dragPayload;
static bool              s_hasPendingMove = false;
static EffectMoveJob     s_pendingMove;

void BeginEffectDrag(const std::string& sinName, const std::vector<int>& path,
                      const std::string& effectName, int index)
{
    s_dragPayload.sinName       = sinName;
    s_dragPayload.originalPath  = path;
    s_dragPayload.effectName    = effectName;
    s_dragPayload.originalIndex = index;
}

const EffectDragPayload& GetEffectDragPayload()
{
    return s_dragPayload;
}

void QueueEffectMove(EffectMoveJob job)
{
    s_pendingMove    = std::move(job);
    s_hasPendingMove = true;
}

// Applies a previously-recorded drag-and-drop move (see EffectMoveJob) to
// the in-memory json and writes it to disk. Re-finds the source category
// and the moved effect right before mutating -- same reasoning as
// ApplyPendingEdit/ApplyPendingCategoryRename above. Unlike ApplyPendingEdit,
// this moves the matched effect object itself (erase from the source
// array, insert into the destination array) rather than mutating fields
// in place -- see EffectMoveJob's destinationIndex comment for how those
// two operations are reconciled when originalPath == destinationPath.
void ApplyPendingMove()
{
    if (!s_hasPendingMove)
        return;
    s_hasPendingMove = false;

    const EffectMoveJob& job = s_pendingMove;

    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        s_editResultMessage = "Move failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.originalPath);
    if (!srcCategory || !srcCategory->contains("effects") || !(*srcCategory)["effects"].is_array())
    {
        s_editResultMessage = "Move failed: the original category is no longer there.";
        return;
    }

    nlohmann::ordered_json effectCopy;
    bool found = false;
    auto& effectsArr = (*srcCategory)["effects"];
    // Indexed lookup -- see ApplyPendingEdit's comment for why a name
    // search isn't safe when sibling effects can share a name.
    if (job.originalIndex >= 0 && static_cast<size_t>(job.originalIndex) < effectsArr.size())
    {
        auto it = effectsArr.begin() + job.originalIndex;
        if (it->contains("name") && (*it)["name"] == job.effectName)
        {
            effectCopy = *it;
            effectsArr.erase(it);
            found = true;
        }
    }
    if (!found)
    {
        s_editResultMessage = "Move failed: effect \"" + job.effectName + "\" is no longer there.";
        return;
    }

    // destinationPath is the drop target category's own index-based
    // identity (captured from the tree at drop time, see
    // RenderCategoryTree's drag-drop target handler) -- it's a real,
    // already-existing category, never typed text, so this resolves it
    // the same way srcCategory was resolved above rather than creating
    // anything new.
    nlohmann::ordered_json* destCategory = FindCategoryByPath(root, job.destinationPath);
    if (!destCategory)
    {
        s_editResultMessage = "Move failed: the destination category is no longer there.";
        return;
    }
    if (!destCategory->contains("effects") || !(*destCategory)["effects"].is_array())
        (*destCategory)["effects"] = nlohmann::ordered_json::array();
    auto& destEffectsArr = (*destCategory)["effects"];

    // destinationIndex == -1 means "append" (dropped on destinationPath's
    // own row, see RenderCategoryTree's category-row drop target) -- the
    // same behavior this always had before destinationIndex existed.
    // Otherwise it's "insert immediately before this index" (dropped on a
    // specific effect's row, see the effect-row drop target), where that
    // index was captured against destEffectsArr *before* the erase above
    // ran. If the source and destination are the same category, that
    // erase already shifted every index after originalIndex down by one,
    // so a destination index that came from later in that same array
    // needs the same adjustment here or the effect lands one slot too far
    // right. A destination index from a different category, or from
    // earlier in the same array, was never touched by that erase and
    // needs no adjustment.
    if (job.destinationIndex < 0)
    {
        destEffectsArr.push_back(std::move(effectCopy));
    }
    else
    {
        int insertIndex = job.destinationIndex;
        if (job.originalPath == job.destinationPath && job.originalIndex < insertIndex)
            insertIndex -= 1;

        // Defensive clamp -- shouldn't trigger given the no-op guards at
        // drop time and everything happening within one frame, but a
        // stale index silently landing outside the array is worse than a
        // slightly-off placement.
        if (insertIndex < 0)
            insertIndex = 0;
        if (static_cast<size_t>(insertIndex) > destEffectsArr.size())
            insertIndex = static_cast<int>(destEffectsArr.size());

        destEffectsArr.insert(destEffectsArr.begin() + insertIndex, std::move(effectCopy));
    }

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Moved \"" + job.effectName + "\" in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        InvalidateInstalledTree(); // force a clean reload; don't trust the in-memory copy after a failed write
        return;
    }

    std::string destLabel = job.destinationPath.empty() ? std::string("(top level)") : JoinCategoryPathNames(root, job.destinationPath);
    if (job.originalPath == job.destinationPath)
        s_editResultMessage = "Reordered \"" + job.effectName + "\" within \"" + destLabel + "\".";
    else
        s_editResultMessage = "Moved \"" + job.effectName + "\" to \"" + destLabel + "\".";
    InvalidateInstalledTree(); // force a clean reload from disk next expand, same as after an applied update
}

// ---------------------------------------------------------------------------
// AnyEditInFlight -- true if any one of the six state machines above is
// currently active, addon-wide. Used to grey out/disable starting a
// different edit/rename/delete/create/move while another is already open.
// This needs state from every one of the per-category and per-effect
// state machines, which is why it lives here rather than with just one
// of them.
// ---------------------------------------------------------------------------
bool AnyEditInFlight()
{
    return IsEffectEditActive() || IsCategoryRenameActive() ||
           IsDeleteConfirmActive() || IsCreateCategoryActive();
}
