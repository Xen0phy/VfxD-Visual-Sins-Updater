//################################################################################
// installed_tree_edit.cpp
//--------------------------------------------------------------------------------
// See installed_tree_edit.h for the module contract. Owns: every one of
// the six state machines' structs and statics, all file-local (wrapped
// in anonymous namespaces) -- addon.cpp reaches this file only through
// the header's accessor API. Extracted from addon.cpp as a mechanical
// move, no behavior change beyond what crossing the file boundary
// needed (raw static reads in addon.cpp became accessor calls).
//--------------------------------------------------------------------------------

#include "effect_db.h"              //. EffectDb_SetName, EffectDb_GetEffect, EffectDb_IsKnownGuid
#include "imgui.h"
#include "installed_tree_edit.h"
#include "installed_tree_overlay.h" //. JoinPath
#include "installed_tree_store.h"   //. FindInstalledJsonMutable, InvalidateInstalledTree, SaveInstalledSinFile
#include "ui_colors.h"              //. kDuplicateColor

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace {
//_ Shared by all six state machines -- see SetEditResultMessage's doc
// comment in installed_tree_edit.h for why this isn't scoped to just one.
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
            continue; //. blank line
        out.push_back(line.substr(start, end - start + 1));
    }
    return out;
}

bool PathHasPrefix(const std::vector<int>& path, const std::vector<int>& prefix)
{
    if (prefix.size() > path.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (path[i] != prefix[i])
            return false;
    return true;
}

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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindEffectByPath
//--------------------------------------------------------------------------------
// Resolves an effect by (containing category's path, index within that
// category's "effects" array, expected name) -- the identity/sanity check
// every ApplyPending* below repeats: same reasoning as FindCategoryByPath,
// one level deeper. Returns nullptr on any failure (category gone,
// "effects" missing/not-an-array, index out of range, or the name at that
// index no longer matches -- something else moved into the slot). On
// failure, if errorOut is non-null, fills it with a ready-to-display
// "<label> is no longer there." message; callers prepend their own
// "Edit failed: "/"Move failed: "/etc. Not exported -- only used within
// this file.
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindEffectByPath(nlohmann::ordered_json& root, const std::vector<int>& path,
                                          int index, const std::string& expectedName,
                                          const std::string& label, std::string* errorOut)
{
    nlohmann::ordered_json* category = FindCategoryByPath(root, path);
    if (!category || !category->contains("effects") || !(*category)["effects"].is_array())
    {
        if (errorOut)
            *errorOut = "\"" + label + "\" is no longer there.";
        return nullptr;
    }
    auto& effects = (*category)["effects"];
    if (index < 0 || static_cast<size_t>(index) >= effects.size())
    {
        if (errorOut)
            *errorOut = "\"" + label + "\" is no longer there.";
        return nullptr;
    }
    auto it = effects.begin() + index;
    if (!it->contains("name") || (*it)["name"] != expectedName)
    {
        if (errorOut)
            *errorOut = "\"" + label + "\" is no longer there.";
        return nullptr;
    }
    return &(*it);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// TrySaveOrReport
//--------------------------------------------------------------------------------
// Shared tail of every ApplyPending* below: write sinName to disk, and on
// failure set the standard "applied in memory but failed to write"
// message (folding in the write error) plus InvalidateInstalledTree().
// `what` is the caller's own past-tense description of what was applied
// ("Edit applied", "Reordered \"Foo\"", "GUID moved", ...), so the
// message reads naturally. Returns true on success, in which case the
// caller still sets its own success message and still calls
// InvalidateInstalledTree() itself -- wording differs too much per-job
// to share that half.
//--------------------------------------------------------------------------------
bool TrySaveOrReport(const std::string& sinName, const std::string& what)
{
    std::string writeError;
    if (SaveInstalledSinFile(sinName, writeError))
        return true;

    s_editResultMessage = what + " in memory but failed to write to disk (" + writeError +
                           "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
    InvalidateInstalledTree();
    return false;
}

namespace {

//********************************************************************************
// CategoryEditState
//--------------------------------------------------------------------------------
// active     whether a rename is currently open
// sinName    which sin file the renamed category belongs to
// path       this category's own identity, root -> ... -> this
//            category, inclusive
// nameBuf/descBuf   edit buffers, same "description" key as effects
//--------------------------------------------------------------------------------
// Shares the same "only one edit in flight addon-wide" rule as effect
// editing (see AnyEditInFlight) -- so an effect edit and a category
// rename can never be open at the same time either.
//--------------------------------------------------------------------------------
struct CategoryEditState
{
    bool              active = false;
    std::string       sinName;
    std::vector<int>  path;
    char              nameBuf[256]  = {};
    char              descBuf[1024] = {};
};
static CategoryEditState s_categoryEdit;

//********************************************************************************
// CategoryRenameJob
//--------------------------------------------------------------------------------
// sinName        which sin file to rename in
// path           root -> ... -> this category, inclusive (same convention
//                as CategoryEditState::path)
// newName        the trimmed replacement name
// newDescription the replacement description -- same "erase if empty"
//                handling as EditSaveJob::newDescription
//--------------------------------------------------------------------------------
struct CategoryRenameJob
{
    std::string       sinName;
    std::vector<int>  path;
    std::string       newName;
    std::string       newDescription;
};
static bool              s_hasPendingCategoryRename = false;
static CategoryRenameJob s_pendingCategoryRename;

//********************************************************************************
// CreateCategoryState
//--------------------------------------------------------------------------------
// active        whether an "add category" prompt is currently open
// sinName       which sin file the new category belongs to
// parentPath    where the new category goes; empty means this sin
//               file's top level
// nameBuf       edit buffer for the in-progress name
//--------------------------------------------------------------------------------
struct CreateCategoryState
{
    bool              active = false;
    std::string       sinName;
    std::vector<int>  parentPath;
    char              nameBuf[256] = {};
};
static CreateCategoryState s_createCategory;

//********************************************************************************
// CreateCategoryJob
//--------------------------------------------------------------------------------
// sinName       which sin file to create in
// parentPath    where the new category goes
// newName       the trimmed new category's name
//--------------------------------------------------------------------------------
struct CreateCategoryJob
{
    std::string       sinName;
    std::vector<int>  parentPath;
    std::string       newName;
};
static bool              s_hasPendingCreateCategory = false;
static CreateCategoryJob s_pendingCreateCategory;

//********************************************************************************
// CategoryDragPayload
//--------------------------------------------------------------------------------
// sinName    which sin file the dragged category belongs to
// path       this category's own identity, root -> ... -> this
//            category, inclusive -- path.back() is its index within its
//            parent's "categories" array, path.begin()..end()-1 is that
//            parent's own path
//--------------------------------------------------------------------------------
// Payload for dragging a category itself, as opposed to an effect -- same
// "ImGui's payload is just a fixed-size marker, the real data lives in a
// static struct kept current every frame BeginDragDropSource runs"
// reasoning EffectDragPayload uses (installed_tree_edit.h).
//--------------------------------------------------------------------------------
struct CategoryDragPayload
{
    std::string       sinName;
    std::vector<int>  path;
};
static CategoryDragPayload s_categoryDragPayload;
static const int           kCategoryDragMarker = 1; //. placeholder payload bytes

//********************************************************************************
// CategoryMoveJob
//--------------------------------------------------------------------------------
// sinName             which sin file to reorder in
// originalPath        this category's own identity at drag time -- see
//                      CategoryDragPayload::path
// destinationIndex     -1 for "append" (dropped on the shared parent's
//                      own row), else the sibling index -- within that
//                      same parent's "categories" array as captured at
//                      drop time, before the source erase -- to land
//                      immediately above; see ApplyPendingCategoryMove
//--------------------------------------------------------------------------------
// Reorder-only (see installed_tree_edit.h): a category's destination is
// always its own current parent's "categories" array, so unlike an
// effect move there's no separate destinationPath.
//--------------------------------------------------------------------------------
struct CategoryMoveJob
{
    std::string       sinName;
    std::vector<int>  originalPath;
    int               destinationIndex = -1;
};
static bool            s_hasPendingCategoryMove = false;
static CategoryMoveJob s_pendingCategoryMove;

} //. namespace

void BeginCategoryEdit(const std::string& sinName, const std::vector<int>& path,
                       const std::string& currentName, const std::string& currentDescription)
{
    s_categoryEdit.active  = true;
    s_categoryEdit.sinName = sinName;
    s_categoryEdit.path    = path;
    std::snprintf(s_categoryEdit.nameBuf, sizeof(s_categoryEdit.nameBuf), "%s", currentName.c_str());
    std::snprintf(s_categoryEdit.descBuf, sizeof(s_categoryEdit.descBuf), "%s", currentDescription.c_str());
    ClearEditResultMessage();
}

void CancelCategoryEdit()
{
    s_categoryEdit.active = false;
    ClearEditResultMessage();
}

void RenderCategoryEditor()
{
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("Editing this category.");
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name##category", s_categoryEdit.nameBuf, sizeof(s_categoryEdit.nameBuf));
    ImGui::InputTextMultiline("Description##category", s_categoryEdit.descBuf, sizeof(s_categoryEdit.descBuf),
                               ImVec2(kFieldWidth, 60));

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
            job.sinName        = s_categoryEdit.sinName;
            job.path           = s_categoryEdit.path;
            job.newName        = trimmed;
            job.newDescription = s_categoryEdit.descBuf;

            s_pendingCategoryRename    = std::move(job);
            s_hasPendingCategoryRename = true;
            s_categoryEdit.active      = false; //. path may vanish on reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##category"))
        CancelCategoryEdit();
}

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
    if (job.newDescription.empty())
        category->erase("description");
    else
        (*category)["description"] = job.newDescription;

    if (!TrySaveOrReport(job.sinName, "Rename applied"))
        return;

    SetEditResultMessage("Renamed category to \"" + job.newName + "\".");
    InvalidateInstalledTree(); //. force reload on next expand
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
            s_createCategory.active    = false; //. path may vanish on reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##newcategory"))
        CancelCreateCategory();
}

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

    //_ Errors out rather than silently reusing an existing same-named
    // sibling -- the user asked for a new category, so finding one
    // already there is worth surfacing, not hiding.
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
    InvalidateInstalledTree(); //. force reload on next expand
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
    //_ Indexed lookup, not a name search -- sibling categories can
    // share a name (same reasoning as the effect-side Apply functions
    // further down).
    if (originalIndex < 0 || static_cast<size_t>(originalIndex) >= siblings.size())
    {
        SetEditResultMessage("Reorder failed: the category is no longer there.");
        return;
    }
    auto        it   = siblings.begin() + originalIndex;
    std::string name = it->value("name", std::string());

    nlohmann::ordered_json categoryCopy = std::move(*it);
    siblings.erase(it);

    //_ destinationIndex is captured against `siblings` before the erase
    // above runs; erasing shifts everything after originalIndex down by
    // one, so a destination index from later in the same array needs -1.
    if (job.destinationIndex < 0)
    {
        siblings.push_back(std::move(categoryCopy));
    }
    else
    {
        int insertIndex = job.destinationIndex;
        if (originalIndex < insertIndex)
            insertIndex -= 1;

        //_ Defensive clamp -- shouldn't trigger given the drop-time
        // no-op guards, but a stale index landing outside the array is
        // worse than a slightly-off placement.
        if (insertIndex < 0)
            insertIndex = 0;
        if (static_cast<size_t>(insertIndex) > siblings.size())
            insertIndex = static_cast<int>(siblings.size());

        siblings.insert(siblings.begin() + insertIndex, std::move(categoryCopy));
    }

    if (!TrySaveOrReport(job.sinName, "Reordered \"" + name + "\""))
        return;

    SetEditResultMessage("Reordered \"" + name + "\".");
    InvalidateInstalledTree(); //. force reload on next expand
}

//********************************************************************************
// EditState
//--------------------------------------------------------------------------------
// active           whether an effect edit is currently open
// sinName          which sin file the edited effect belongs to
// originalPath     containing category's identity: root -> immediate
//                  parent, each element that level's index within its
//                  parent's "categories" array (see FindCategoryByPath)
// originalName     effect's name at BeginEdit time -- for display and
//                  as a sanity check on save
// originalIndex    this effect's position within originalPath's
//                  "effects" array at BeginEdit time -- the actual
//                  identity key, since sibling effects can share a name
// nameBuf/descBuf/guidsBuf   edit buffers (guidsBuf: one guid per line)
//--------------------------------------------------------------------------------
struct EditState
{
    bool              active = false;
    std::string       sinName;
    std::vector<int>  originalPath;
    std::string       originalName;
    int               originalIndex = -1;

    char nameBuf[256]   = {};
    char descBuf[1024]  = {};
    char guidsBuf[2048] = {};
};
static EditState s_edit;

//********************************************************************************
// EditSaveJob
//--------------------------------------------------------------------------------
// sinName/originalPath/originalName/originalIndex   see EditState's own
//                                                    fields above
// newName/newDescription/newGuids                   the edited values
//--------------------------------------------------------------------------------
// Set by the Save button; consumed once, after the whole tree has
// finished rendering for this frame, so the effect array is never
// mutated mid-walk.
//--------------------------------------------------------------------------------
struct EditSaveJob
{
    std::string              sinName;
    std::vector<int>         originalPath;
    std::string              originalName;
    int                      originalIndex = -1;
    std::string              newName;
    std::string              newDescription;
    std::vector<std::string> newGuids;
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

void RenderEffectEditor()
{
    //_ Fixed width for all edit fields -- without this, InputText
    // stretches to fill the whole options-panel width, way too wide
    // for a short value like a name or a guid.
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
            s_edit.active    = false; //. the node this refers to may move/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        CancelEdit();
}

void ApplyPendingEdit()
{
    if (!s_hasPendingSave)
        return;
    s_hasPendingSave = false;

    const EditSaveJob& job = s_pendingSave;

    //_ Re-finds the source category/effect by path rather than carrying
    // a pointer from render time -- same reasoning as ApplyMergePlan's
    // re-derived index in merge.cpp; costs nothing, removes any doubt.
    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        s_editResultMessage = "Edit failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    //_ Indexed-by-path-and-index lookup, not a name search -- sibling
    // effects can share a name. originalIndex can't have shifted since
    // BeginEdit time (no other mutation of this array is allowed mid-edit).
    std::string findError;
    nlohmann::ordered_json* effect = FindEffectByPath(root, job.originalPath, job.originalIndex,
                                                        job.originalName, job.originalName, &findError);
    if (!effect)
    {
        s_editResultMessage = "Edit failed: " + findError;
        return;
    }

    //_ "guids" is always written, even empty -- clearing all guids is
    // valid (just won't be tracked across a future update, see merge.h).
    // Category placement isn't touched here; that's drag-and-drop's job.
    nlohmann::ordered_json guidsArr = nlohmann::ordered_json::array();
    for (const auto& g : job.newGuids)
        guidsArr.push_back(g);

    (*effect)["name"] = job.newName;
    if (job.newDescription.empty())
        effect->erase("description");
    else
        (*effect)["description"] = job.newDescription;
    (*effect)["guids"] = std::move(guidsArr);

    if (!TrySaveOrReport(job.sinName, "Edit applied"))
        return;

    //_ A rename here is meant to write both places when a guid is known to
    // both -- see effect_db.h's EffectDb_SetName doc comment. This is the
    // JSON half's caller, so it's also the natural place to drive the
    // database half: any guid on this effect the database already knows
    // about gets its name synced too. A guid the database has never seen
    // is left alone (EffectDb_SetName would just no-op on it anyway) --
    // this doesn't create new database rows, only updates existing ones.
    for (const auto& guid : job.newGuids)
        if (EffectDb_IsKnownGuid(guid))
            EffectDb_SetName(guid, job.newName);

    s_editResultMessage = "Saved changes to \"" + job.newName + "\".";
    InvalidateInstalledTree(); //. force reload on next expand
}

//********************************************************************************
// DbRenameState
//--------------------------------------------------------------------------------
// active         whether a db-only rename is currently open
// guid_b64       the guid being renamed -- its own identity, since a
//                db-only node has no stable JSON (sinName, path, index)
// originalName   for display
// nameBuf        edit buffer
//--------------------------------------------------------------------------------
// Sibling of EditState, but scoped to guids with no real JSON entry (a
// "__vfxd_db_only" node -- see RenderCategoryTree's effIsDbOnly branch).
// Only ever writes EffectDb_SetName; never touches any sin file, since
// there's no JSON entry here to write. A guid that already has a JSON
// entry uses BeginEdit/RenderEffectEditor/ApplyPendingEdit instead, which
// syncs the database name itself -- see that function's own comment.
//--------------------------------------------------------------------------------
struct DbRenameState
{
    bool        active = false;
    std::string guid_b64;
    std::string originalName;

    char nameBuf[256] = {};
};
static DbRenameState s_dbRename;

//********************************************************************************
// DbRenameSaveJob
//--------------------------------------------------------------------------------
// guid_b64/newName   see DbRenameState's own fields above
//--------------------------------------------------------------------------------
// Set by the Save button; consumed once, after the whole tree has finished
// rendering for this frame, same deferred-apply shape as every other job
// here even though EffectDb_SetName doesn't touch anything this frame's
// tree walk is iterating over -- keeping the shape uniform is worth more
// than the (nonexistent) mid-walk hazard it would avoid.
//--------------------------------------------------------------------------------
struct DbRenameSaveJob
{
    std::string guid_b64;
    std::string newName;
};
static bool          s_hasPendingDbRename = false;
static DbRenameSaveJob s_pendingDbRename;

void BeginDbRename(const std::string& guid_b64, const std::string& currentName)
{
    s_dbRename.active       = true;
    s_dbRename.guid_b64     = guid_b64;
    s_dbRename.originalName = currentName;

    std::snprintf(s_dbRename.nameBuf, sizeof(s_dbRename.nameBuf), "%s", currentName.c_str());

    s_editResultMessage.clear();
}

void CancelDbRename()
{
    s_dbRename.active = false;
    s_editResultMessage.clear();
}

bool IsDbGuidBeingRenamed(const std::string& guid_b64)
{
    return s_dbRename.active && s_dbRename.guid_b64 == guid_b64;
}

bool IsDbRenameActive()
{
    return s_dbRename.active;
}

void RenderDbRenameEditor()
{
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("Renaming this guid in the \"for science\" database only -- it has no JSON entry.");

    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name", s_dbRename.nameBuf, sizeof(s_dbRename.nameBuf));

    if (ImGui::Button("Save"))
    {
        std::string trimmedName = s_dbRename.nameBuf;
        size_t start = trimmedName.find_first_not_of(" \t\r\n");
        size_t end   = trimmedName.find_last_not_of(" \t\r\n");
        trimmedName  = (start == std::string::npos) ? std::string() : trimmedName.substr(start, end - start + 1);

        if (trimmedName.empty())
        {
            s_editResultMessage = "Rename not saved: name can't be empty.";
        }
        else
        {
            DbRenameSaveJob job;
            job.guid_b64 = s_dbRename.guid_b64;
            job.newName  = trimmedName;

            s_pendingDbRename    = std::move(job);
            s_hasPendingDbRename = true;
            s_dbRename.active    = false; //. the node this refers to may move/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        CancelDbRename();
}

void ApplyPendingDbRename()
{
    if (!s_hasPendingDbRename)
        return;
    s_hasPendingDbRename = false;

    const DbRenameSaveJob& job = s_pendingDbRename;

    if (!EffectDb_SetName(job.guid_b64, job.newName))
    {
        s_editResultMessage = "Rename failed: this guid is no longer known to the effect database.";
        return;
    }

    s_editResultMessage = "Renamed to \"" + job.newName + "\" in the effect database.";

    //_ EffectDb_SetName already bumped EffectDb_GetGeneration() on its
    // own, but the overlay cache deliberately does NOT react to that by
    // itself -- see OverlayCacheEntry's comment on why effect-db
    // generation changes alone never force a rebuild (that's what kept
    // live background capture from stuttering the tree). This is
    // different: a deliberate, user-clicked, one-off action, not
    // high-frequency capture -- the same category of trigger "pressing
    // Refresh" already is, per that same design. InvalidateInstalledTree()
    // forces the next frame's rebuild to pick up the new name rather than
    // leaving the old one on screen until some unrelated reload happens to
    // occur.
    InvalidateInstalledTree();
}

//********************************************************************************
// PromoteToJsonJob
//--------------------------------------------------------------------------------
// guid_b64   the db-only guid being promoted
//--------------------------------------------------------------------------------
// Deliberately just the guid -- name/category_path are re-looked-up fresh
// from the database at Apply time (see ApplyPendingPromote) rather than
// carried from click time, same "re-find, don't trust a stale snapshot"
// reasoning as every other ApplyPending* here.
//--------------------------------------------------------------------------------
struct PromoteToJsonJob
{
    std::string guid_b64;
};
static bool           s_hasPendingPromote = false;
static PromoteToJsonJob s_pendingPromote;

void QueuePromoteToJson(const std::string& guid_b64)
{
    s_pendingPromote.guid_b64 = guid_b64;
    s_hasPendingPromote       = true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// JsonHasGuid
//--------------------------------------------------------------------------------
// True if `guid` already appears on some effect anywhere under `category`.
// Guards ApplyPendingPromote against double-adding the same guid -- e.g.
// two "Add to JSON" clicks queued in the same frame, or a guid that was
// already promoted (by hand, or by a previous promote) since the db-only
// node currently on screen was last built.
//--------------------------------------------------------------------------------
bool JsonHasGuid(const nlohmann::ordered_json& category, const std::string& guid)
{
    if (category.contains("effects") && category["effects"].is_array())
        for (const auto& eff : category["effects"])
            if (eff.contains("guids") && eff["guids"].is_array())
                for (const auto& g : eff["guids"])
                    if (g.is_string() && g.get<std::string>() == guid)
                        return true;

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            if (JsonHasGuid(sub, guid))
                return true;

    return false;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateRealCategory
//--------------------------------------------------------------------------------
// Same name-path walk as installed_tree_overlay.cpp's file-local
// FindOrCreateDiffCategory, but over a real, mutable sin file rather than a
// display-only overlay copy -- so a category this creates is a genuine new
// category on disk, not tagged "__vfxd_virtual". Not shared code with that
// function since one writes a throwaway display copy and the other writes
// what SaveInstalledSinFile is about to persist; conflating "virtual for
// display" and "real on save" into one flag-driven function seemed more
// error-prone than two short, single-purpose ones.
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindOrCreateRealCategory(nlohmann::ordered_json& root, const std::vector<std::string>& path)
{
    nlohmann::ordered_json* cursor = &root;
    for (const auto& segment : path)
    {
        if (!cursor->contains("categories") || !(*cursor)["categories"].is_array())
            (*cursor)["categories"] = nlohmann::ordered_json::array();

        nlohmann::ordered_json* next = nullptr;
        for (auto& sub : (*cursor)["categories"])
            if (sub.value("name", std::string()) == segment) { next = &sub; break; }

        if (!next)
        {
            nlohmann::ordered_json newCat;
            newCat["name"]       = segment;
            newCat["categories"] = nlohmann::ordered_json::array();
            newCat["effects"]    = nlohmann::ordered_json::array();
            (*cursor)["categories"].push_back(std::move(newCat));
            next = &(*cursor)["categories"].back();
        }
        cursor = next;
    }
    return cursor;
}

void ApplyPendingPromote()
{
    if (!s_hasPendingPromote)
        return;
    s_hasPendingPromote = false;

    const std::string guid_b64 = s_pendingPromote.guid_b64;

    EffectDbEffect dbEff;
    if (!EffectDb_GetEffect(guid_b64, dbEff))
    {
        s_editResultMessage = "Add to JSON failed: this guid is no longer known to the effect database.";
        return;
    }

    //_ Promotion always targets Greed -- see effect_db.h on why there's no
    // per-guid file choice to make here.
    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable("Greed");
    if (!rootPtr)
    {
        s_editResultMessage = "Add to JSON failed: VfxD_Greed.json is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    if (JsonHasGuid(root, guid_b64))
    {
        s_editResultMessage = "Add to JSON: already has a JSON entry in Greed.";
        InvalidateInstalledTree(); //. the on-screen db-only node is stale -- force a rebuild
        return;
    }

    //_ Same fallback bucket BuildEffectDbOverlayTree already displays an
    // unplaced guid under -- see its own kUnplacedBucket.
    static const std::vector<std::string> kUnplacedBucket = { "Unrecognized (for science)" };
    const std::vector<std::string>& path = dbEff.categoryPath.empty() ? kUnplacedBucket : dbEff.categoryPath;

    nlohmann::ordered_json* cat = FindOrCreateRealCategory(root, path);
    if (!cat->contains("effects") || !(*cat)["effects"].is_array())
        (*cat)["effects"] = nlohmann::ordered_json::array();

    //_ Falls back to the guid itself when unnamed -- same reasoning as
    // BuildEffectDbOverlayTree's own newEffect["name"] fallback.
    std::string finalName = dbEff.name.empty() ? guid_b64 : dbEff.name;

    nlohmann::ordered_json newEffect;
    newEffect["name"]  = finalName;
    newEffect["guids"] = nlohmann::ordered_json::array({ guid_b64 });
    (*cat)["effects"].push_back(std::move(newEffect));

    if (!TrySaveOrReport("Greed", "Added to JSON"))
        return;

    s_editResultMessage = "Added \"" + finalName + "\" to Greed's JSON.";
    InvalidateInstalledTree(); //. force reload on next expand
}

static DbOnlyGuidDragPayload s_dbOnlyDragPayload;

void BeginDbOnlyGuidDrag(const std::string& guid_b64, const std::string& effectName)
{
    s_dbOnlyDragPayload.guid_b64   = guid_b64;
    s_dbOnlyDragPayload.effectName = effectName;
}

const DbOnlyGuidDragPayload& GetDbOnlyGuidDragPayload()
{
    return s_dbOnlyDragPayload;
}

//********************************************************************************
// DbCategoryPlacementJob
//--------------------------------------------------------------------------------
// guid_b64        the db-only guid being placed
// categoryPath    destination category's name path -- see
//                 QueueDbCategoryPlacement's own comment
// effectName      display name, for the result message -- captured here
//                 at Queue time rather than read back from the live drag
//                 payload at Apply time, same "don't trust a stale
//                 snapshot read later" reasoning as every other job here
//--------------------------------------------------------------------------------
struct DbCategoryPlacementJob
{
    std::string              guid_b64;
    std::vector<std::string> categoryPath;
    std::string              effectName;
};
static bool                   s_hasPendingDbCategoryPlacement = false;
static DbCategoryPlacementJob s_pendingDbCategoryPlacement;

void QueueDbCategoryPlacement(const std::string& guid_b64, const std::vector<std::string>& categoryPath)
{
    s_pendingDbCategoryPlacement.guid_b64     = guid_b64;
    s_pendingDbCategoryPlacement.categoryPath = categoryPath;
    s_pendingDbCategoryPlacement.effectName   = s_dbOnlyDragPayload.effectName;
    s_hasPendingDbCategoryPlacement           = true;
}

void ApplyPendingDbCategoryPlacement()
{
    if (!s_hasPendingDbCategoryPlacement)
        return;
    s_hasPendingDbCategoryPlacement = false;

    const DbCategoryPlacementJob& job = s_pendingDbCategoryPlacement;

    if (!EffectDb_SetCategoryPath(job.guid_b64, job.categoryPath))
    {
        s_editResultMessage = "Category placement failed: this guid is no longer known to the effect database.";
        return;
    }

    s_editResultMessage = "Moved \"" + job.effectName + "\" to " + JoinPath(job.categoryPath) + ".";

    //_ Same reasoning as ApplyPendingDbRename: EffectDb_SetCategoryPath
    // already bumped EffectDb_GetGeneration() on its own, but the overlay
    // cache deliberately doesn't react to that by itself (see
    // OverlayCacheEntry's comment) -- this is a deliberate, user-dragged,
    // one-off action, so force the rebuild explicitly rather than waiting
    // for an unrelated Refresh/JSON edit.
    InvalidateInstalledTree();
}

//********************************************************************************
// DeleteConfirmState
//--------------------------------------------------------------------------------
// active         whether a delete confirmation is currently open
// isCategory     true for a category delete, false for an effect delete
// sinName        which sin file the target belongs to
// path           effect: containing category's identity; category: this
//                category's OWN identity, inclusive (see EditState's
//                originalPath field)
// index          effect index within path's "effects" array (see
//                EditState's originalIndex field); unused for a
//                category delete
// displayName    just for the confirmation text/messages
//--------------------------------------------------------------------------------
// Rendered inline right next to the item's own row (see the "-"
// SmallButton in RenderCategoryTree), which is always visible whether or
// not that row's TreeNode is open -- so unlike EditState, this
// deliberately does NOT get cancelled just because the owning node is
// collapsed; nothing about it was ever hidden by collapsing in the first
// place.
//--------------------------------------------------------------------------------
struct DeleteConfirmState
{
    bool              active     = false;
    bool              isCategory = false;
    std::string       sinName;
    std::vector<int>  path;
    int               index = -1;
    std::string       displayName;
};
static DeleteConfirmState s_deleteConfirm;

//********************************************************************************
// DeleteJob
//--------------------------------------------------------------------------------
// isCategory/sinName/path/index   see DeleteConfirmState's own fields
//                                 above
// name                            sanity check at apply time -- see
//                                 ApplyPendingDelete
//--------------------------------------------------------------------------------
struct DeleteJob
{
    bool              isCategory = false;
    std::string       sinName;
    std::vector<int>  path;
    int               index = -1;
    std::string       name;
};
static bool      s_hasPendingDelete = false;
static DeleteJob s_pendingDelete;

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
        s_deleteConfirm.active = false; //. path may vanish on reload
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel##delete"))
        CancelDeleteConfirm();
}

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
        //_ Indexed lookup, not a name search -- sibling categories can
        // share a name (same reasoning as the effect branch below); safe
        // since only one edit/delete/create/rename is in flight at once.
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
        //_ Re-checked here, not just at the confirm prompt -- this is
        // the authoritative check.
        if (hasEffects || hasSubcats)
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" isn't empty anymore.";
            return;
        }
        siblings.erase(it);
    }
    else
    {
        //_ Same indexed-by-path-and-index lookup as ApplyPendingEdit. Once
        // found, re-resolve the category for an erasable iterator --
        // guaranteed to succeed since FindEffectByPath just verified path/index/name.
        std::string findError;
        if (!FindEffectByPath(root, job.path, job.index, job.name, job.name, &findError))
        {
            s_editResultMessage = "Delete failed: " + findError;
            return;
        }
        nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.path);
        auto& effectsArr = (*srcCategory)["effects"];
        effectsArr.erase(effectsArr.begin() + job.index);
    }

    if (!TrySaveOrReport(job.sinName, "Delete applied"))
        return;

    s_editResultMessage   = "Deleted \"" + job.name + "\".";
    InvalidateInstalledTree(); //. force reload on next expand
}

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

void ApplyPendingMove()
{
    if (!s_hasPendingMove)
        return;
    s_hasPendingMove = false;

    const EffectMoveJob& job = s_pendingMove;

    //_ Re-finds the source category/effect by path rather than carrying
    // a pointer from render time -- same reasoning as ApplyPendingEdit.
    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        s_editResultMessage = "Move failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    //_ Same indexed-by-path-and-index lookup as ApplyPendingEdit. Once
    // found, re-resolve the category for an erasable iterator --
    // guaranteed to succeed since FindEffectByPath just verified path/index/name.
    std::string findError;
    if (!FindEffectByPath(root, job.originalPath, job.originalIndex, job.effectName, job.effectName, &findError))
    {
        s_editResultMessage = "Move failed: " + findError;
        return;
    }
    nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.originalPath);
    auto& effectsArr = (*srcCategory)["effects"];
    auto  srcIt       = effectsArr.begin() + job.originalIndex;
    nlohmann::ordered_json effectCopy = std::move(*srcIt);
    effectsArr.erase(srcIt);

    //_ destinationPath is a real, already-existing category (captured
    // from the tree at drop time), never typed text -- resolved the same
    // way srcCategory was, rather than creating anything new.
    nlohmann::ordered_json* destCategory = FindCategoryByPath(root, job.destinationPath);
    if (!destCategory)
    {
        s_editResultMessage = "Move failed: the destination category is no longer there.";
        return;
    }
    if (!destCategory->contains("effects") || !(*destCategory)["effects"].is_array())
        (*destCategory)["effects"] = nlohmann::ordered_json::array();
    auto& destEffectsArr = (*destCategory)["effects"];

    //_ Append/insert convention: see EffectMoveJob (installed_tree_edit.h).
    // Same-category moves need a -1 shift here since the erase above
    // already moved everything after originalIndex down by one.
    if (job.destinationIndex < 0)
    {
        destEffectsArr.push_back(std::move(effectCopy));
    }
    else
    {
        int insertIndex = job.destinationIndex;
        if (job.originalPath == job.destinationPath && job.originalIndex < insertIndex)
            insertIndex -= 1;

        //_ Defensive clamp -- see ApplyPendingCategoryMove's identical
        // case for why this is worth having.
        if (insertIndex < 0)
            insertIndex = 0;
        if (static_cast<size_t>(insertIndex) > destEffectsArr.size())
            insertIndex = static_cast<int>(destEffectsArr.size());

        destEffectsArr.insert(destEffectsArr.begin() + insertIndex, std::move(effectCopy));
    }

    if (!TrySaveOrReport(job.sinName, "Moved \"" + job.effectName + "\""))
        return;

    std::string destLabel = job.destinationPath.empty() ? std::string("(top level)") : JoinCategoryPathNames(root, job.destinationPath);
    if (job.originalPath == job.destinationPath)
        s_editResultMessage = "Reordered \"" + job.effectName + "\" within \"" + destLabel + "\".";
    else
        s_editResultMessage = "Moved \"" + job.effectName + "\" to \"" + destLabel + "\".";
    InvalidateInstalledTree(); //. force reload on next expand
}

static GuidDragPayload s_guidDragPayload;
static bool            s_hasPendingGuidMerge = false;
static GuidMergeJob    s_pendingGuidMerge;

void BeginGuidDrag(const std::string& sinName, const std::vector<int>& path,
                    int index, const std::string& effectName, const std::string& guid)
{
    s_guidDragPayload.sinName      = sinName;
    s_guidDragPayload.originalPath = path;
    s_guidDragPayload.originalIndex = index;
    s_guidDragPayload.effectName    = effectName;
    s_guidDragPayload.guid          = guid;
}

const GuidDragPayload& GetGuidDragPayload()
{
    return s_guidDragPayload;
}

void QueueGuidMerge(GuidMergeJob job)
{
    s_pendingGuidMerge    = std::move(job);
    s_hasPendingGuidMerge = true;
}

void ApplyPendingGuidMerge()
{
    if (!s_hasPendingGuidMerge)
        return;
    s_hasPendingGuidMerge = false;

    const GuidMergeJob& job = s_pendingGuidMerge;

    if (job.originalPath == job.destinationPath && job.originalIndex == job.destinationIndex)
    {
        s_editResultMessage = "Nothing to do: dropped a GUID onto its own effect.";
        return;
    }

    //_ Re-finds both effects by path/index rather than carrying pointers
    // from render time -- same reasoning as ApplyPendingMove.
    nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(job.sinName);
    if (!rootPtr)
    {
        s_editResultMessage = "GUID move failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = *rootPtr;

    //_ Both effects resolved by path/index/name -- same reasoning as
    // ApplyPendingEdit/ApplyPendingMove, just needed twice here since a
    // merge touches two independent effects instead of one.
    std::string findError;
    nlohmann::ordered_json* srcEffect = FindEffectByPath(root, job.originalPath, job.originalIndex,
                                                           job.effectName, job.effectName, &findError);
    if (!srcEffect)
    {
        s_editResultMessage = "GUID move failed: " + findError;
        return;
    }

    if (!srcEffect->contains("guids") || !(*srcEffect)["guids"].is_array())
    {
        s_editResultMessage = "GUID move failed: \"" + job.effectName + "\" no longer has that GUID.";
        return;
    }
    auto& srcGuids = (*srcEffect)["guids"];
    auto  guidIt   = std::find_if(srcGuids.begin(), srcGuids.end(),
        [&](const nlohmann::ordered_json& g) { return g.is_string() && g.get<std::string>() == job.guid; });
    if (guidIt == srcGuids.end())
    {
        s_editResultMessage = "GUID move failed: \"" + job.effectName + "\" no longer has that GUID.";
        return;
    }

    //_ destinationPath/Index are a real, already-existing effect
    // (captured from the tree at drop time), never typed text -- resolved
    // the same way srcEffect was.
    nlohmann::ordered_json* destEffect = FindEffectByPath(root, job.destinationPath, job.destinationIndex,
                                                            job.destinationEffectName, job.destinationEffectName, &findError);
    if (!destEffect)
    {
        s_editResultMessage = "GUID move failed: " + findError;
        return;
    }

    if (!destEffect->contains("guids") || !(*destEffect)["guids"].is_array())
        (*destEffect)["guids"] = nlohmann::ordered_json::array();
    auto& destGuids = (*destEffect)["guids"];

    //_ Never creates a cross-effect duplicate -- if the target already
    // has this GUID, the source copy is still removed (that's the point
    // of the drag), it just isn't re-added on top of the existing one.
    bool alreadyOnDest = std::any_of(destGuids.begin(), destGuids.end(),
        [&](const nlohmann::ordered_json& g) { return g.is_string() && g.get<std::string>() == job.guid; });

    srcGuids.erase(guidIt);
    if (!alreadyOnDest)
        destGuids.push_back(job.guid);

    if (!TrySaveOrReport(job.sinName, "GUID moved"))
        return;

    s_editResultMessage = alreadyOnDest
        ? "Moved GUID to \"" + job.destinationEffectName + "\" (it already had this GUID -- removed the duplicate from \"" +
              job.effectName + "\")."
        : "Moved GUID from \"" + job.effectName + "\" to \"" + job.destinationEffectName + "\".";
    InvalidateInstalledTree(); //. force reload on next expand
}

namespace {

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CollectEmptyGuidEffects / RemoveEmptyGuidEffects
//--------------------------------------------------------------------------------
// Recursive category walks backing CountEmptyGuidEffects and the "Delete
// Empty" sweep -- an effect counts as empty if its "guids" key is
// missing, non-array, or an empty array. Collect only counts; Remove
// actually erases (erase-remove per category, then recurses into
// subcategories) and returns how many it removed, for the result
// message.
//--------------------------------------------------------------------------------
void CollectEmptyGuidEffects(const nlohmann::ordered_json& node, int& count)
{
    if (node.contains("effects") && node["effects"].is_array())
        for (const auto& eff : node["effects"])
            if (!eff.contains("guids") || !eff["guids"].is_array() || eff["guids"].empty())
                ++count;

    if (node.contains("categories") && node["categories"].is_array())
        for (const auto& cat : node["categories"])
            CollectEmptyGuidEffects(cat, count);
}

size_t RemoveEmptyGuidEffects(nlohmann::ordered_json& node)
{
    size_t removed = 0;

    if (node.contains("effects") && node["effects"].is_array())
    {
        auto& effects = node["effects"];
        for (auto it = effects.begin(); it != effects.end();)
        {
            if (!it->contains("guids") || !(*it)["guids"].is_array() || (*it)["guids"].empty())
            {
                it = effects.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }
    }

    if (node.contains("categories") && node["categories"].is_array())
        for (auto& cat : node["categories"])
            removed += RemoveEmptyGuidEffects(cat);

    return removed;
}

bool s_deleteEmptyConfirmActive = false;

} //. namespace

int CountEmptyGuidEffects()
{
    int count = 0;
    for (const auto& [sinName, root] : GetInstalledJson())
        CollectEmptyGuidEffects(root, count);
    return count;
}

bool IsDeleteEmptyConfirmActive()
{
    return s_deleteEmptyConfirmActive;
}

void BeginDeleteEmptyConfirm()
{
    s_deleteEmptyConfirmActive = true;
    s_editResultMessage.clear();
}

void CancelDeleteEmptyConfirm()
{
    s_deleteEmptyConfirmActive = false;
}

void RenderDeleteEmptyConfirm()
{
    int count = CountEmptyGuidEffects();
    if (count == 0)
    {
        s_editResultMessage       = "No empty effects to delete.";
        s_deleteEmptyConfirmActive = false;
        return;
    }

    ImGui::TextColored(kDuplicateColor,
        "Delete %d empty effect%s (no GUIDs) across every loaded sin file? "
        "A .bak is kept per file, but there's no in-app undo for this.",
        count, count == 1 ? "" : "s");

    if (ImGui::SmallButton("Delete##confirm_empty"))
    {
        int                      totalRemoved = 0;
        std::vector<std::string> failedSins;

        //_ GetInstalledJson() just supplies the set of sin names to visit
        // -- each one is then re-fetched mutably, same "look it up fresh,
        // don't carry a pointer" reasoning as every Apply* above.
        std::vector<std::string> sinNames;
        for (const auto& [sinName, root] : GetInstalledJson())
            sinNames.push_back(sinName);

        for (const auto& sinName : sinNames)
        {
            nlohmann::ordered_json* rootPtr = FindInstalledJsonMutable(sinName);
            if (!rootPtr)
                continue;

            size_t removed = RemoveEmptyGuidEffects(*rootPtr);
            if (removed == 0)
                continue;

            std::string writeError;
            if (!SaveInstalledSinFile(sinName, writeError))
                failedSins.push_back(sinName);
            else
                totalRemoved += static_cast<int>(removed);
        }

        s_deleteEmptyConfirmActive = false;

        if (!failedSins.empty())
        {
            std::string joined;
            for (size_t i = 0; i < failedSins.size(); ++i)
            {
                if (i)
                    joined += ", ";
                joined += failedSins[i];
            }
            s_editResultMessage = "Deleted " + std::to_string(totalRemoved) +
                                   " empty effect(s), but failed to write: " + joined +
                                   ". Reloading from disk so nothing shown is out of sync with what's actually saved.";
        }
        else
        {
            s_editResultMessage = "Deleted " + std::to_string(totalRemoved) + " empty effect(s).";
        }
        InvalidateInstalledTree();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel##delete_empty"))
        CancelDeleteEmptyConfirm();
}

bool AnyEditInFlight()
{
    return IsEffectEditActive() || IsCategoryRenameActive() ||
           IsDeleteConfirmActive() || IsCreateCategoryActive() ||
           IsDeleteEmptyConfirmActive() || IsDbRenameActive();
}