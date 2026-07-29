//##############################################################################
// installed_tree_view.cpp
//------------------------------------------------------------------------------
// RenderInstalledEffects(dir)   draws the whole Installed Effects section
//------------------------------------------------------------------------------
// Extracted from addon.cpp: RenderCategoryTree (the recursive tree renderer),
// RenderInstalledEffects (the section wrapper), and their small leaf
// renderers (GuidList/GuidDiff/JsonValue/Behavior/ConflictSources) are all
// file-local -- RenderInstalledEffects is the only symbol this module
// exposes. Reaches the editing/store/overlay/search modules only through
// their own accessor headers, never through another module's statics.
//
// s_overlayCache holds one built (duplicate-guid- and pending-diff-tagged)
// copy of each sin file's tree, rebuilt only when its generation or diff
// status changes -- rebuilding it every frame instead was the direct cause
// of a reported scrolling stall on a large tree with an overlay open.
//
// The tree search box drives one lowercased query (s_treeSearchQueryLower)
// that every match/filter helper below compares against.
// s_treeSearchQueryChanged is true for exactly one frame per query change
// and gates every forced-open/forced-closed TreeNode call in this file --
// doing that work every frame instead (not just on change) was a second,
// separate cause of the same kind of stall.
//
// Drag-and-drop is reorder-only: an effect or category can move among its
// current siblings, never to a different parent or sin file. Nothing that
// only exists in a pending-update overlay (__vfxd_new/__vfxd_rework/
// __vfxd_virtual) offers drag, edit, or delete -- there's no stable real
// on-disk position/identity for it yet. Any edit/rename/delete/create state
// scoped under a node that stops being drawn this frame (hidden by search,
// or collapsed) is cancelled immediately rather than left running
// invisibly.
//
// The one exception is a single-GUID drag: a GUID's own bullet row, in
// the plain read-only view only, is a drag source (see the
// GuidListDragContext-driven branch of RenderGuidList below) -- not
// offered from inside the effect editor, which is back to a single
// "one GUID per line" textbox (see RenderEffectEditor in
// installed_tree_edit.cpp). That's a genuine cross-effect content move,
// any effect row in the same sin file is a valid target (except the one
// currently open for editing), and it writes straight to disk on drop --
// see QueueGuidMerge.
//------------------------------------------------------------------------------

#include "github_update.h"
#include "imgui.h"
#include "installed_tree_edit.h"
#include "installed_tree_overlay.h"
#include "installed_tree_search.h"
#include "installed_tree_store.h"
#include "installed_tree_view.h"
#include "ui_colors.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

//********************************************************************************
// GuidListDragContext
//--------------------------------------------------------------------------------
// sinName/path/index    the owning effect's identity (see GuidDragPayload)
// effectName             owning effect's display name, for messages
//--------------------------------------------------------------------------------
// Passed to RenderGuidList to make its rows draggable straight from the
// read-only tree (no need to open the effect's editor first) -- nullptr
// (the default) keeps the plain BulletText rendering RenderGuidDiff's
// added/removed/unchanged buckets use, where dragging wouldn't make
// sense (an "Added" bucket's GUIDs aren't actually on this effect yet).
//--------------------------------------------------------------------------------
struct GuidListDragContext
{
    std::string       sinName;
    std::vector<int>  path;
    int               index = -1;
    std::string       effectName;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderGuidList
//------------------------------------------------------------------------------
// `color` is optional -- nullptr for the default text color (a plain guids
// list), or a color to tint every bullet (e.g. kReworkColor, to set a
// reworked effect's post-update GUIDs apart from its current ones).
// `dragContext` is optional -- see GuidListDragContext. When set, each row
// is a "VFXD_GUID" drag source (see QueueGuidMerge) instead of a plain
// bullet; gated on !AnyEditInFlight() so it can't start a new drag while
// some other edit is already open elsewhere.
//------------------------------------------------------------------------------
void RenderGuidList(const char* label, const std::vector<std::string>& guids, const ImVec4* color = nullptr,
                     const GuidListDragContext* dragContext = nullptr)
{
    if (guids.empty())
    {
        ImGui::TextDisabled("%s: (none)", label);
        return;
    }

    ImGui::TextDisabled("%s:", label);
    ImGui::Indent();
    if (color)
        ImGui::PushStyleColor(ImGuiCol_Text, *color);

    if (!dragContext)
    {
        for (const auto& g : guids)
            ImGui::BulletText("%s", g.c_str());
    }
    else
    {
        bool dragAllowed = !AnyEditInFlight();
        for (size_t i = 0; i < guids.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BulletText("%s", guids[i].c_str());

            //_ BulletText isn't an interactive widget -- it never registers an
            // ImGui ID, so BeginDragDropSource needs ImGuiDragDropFlags_SourceAllowNullID
            // to accept it as a source. Without this flag, ImGui hits an assert
            // (IM_ASSERT(0) in BeginDragDropSource) any time this runs while the
            // mouse button is down and the last item has no ID -- which includes
            // the very first frame a node opens, since clicking the tree arrow
            // toggles it open on mouse-DOWN, not mouse-up.
            if (dragAllowed && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                BeginGuidDrag(dragContext->sinName, dragContext->path, dragContext->index,
                              dragContext->effectName, guids[i]);
                ImGui::SetDragDropPayload("VFXD_GUID", &kGuidDragMarker, sizeof(kGuidDragMarker));
                ImGui::Text("Move GUID \"%s\"", guids[i].c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    }

    if (color)
        ImGui::PopStyleColor();
    ImGui::Unindent();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderGuidDiff
//------------------------------------------------------------------------------
// Shows only what a reworked effect's guid list would actually change to,
// rather than the full current and post-update lists side by side -- a 1c
// merge can fold in dozens of untouched guids (see BuildMergedRework in
// merge.cpp), so printing both in full mostly repeats the same lines.
// Guids are an unordered identity set (see GuidDiff in merge.cpp), so this
// is a set difference, not a positional diff; unchanged guids still get
// listed, only added/removed ones get their own highlighted section.
//------------------------------------------------------------------------------
void RenderGuidDiff(const std::vector<std::string>& oldGuids, const std::vector<std::string>& newGuids)
{
    std::unordered_set<std::string> oldSet(oldGuids.begin(), oldGuids.end());
    std::unordered_set<std::string> newSet(newGuids.begin(), newGuids.end());

    std::vector<std::string> added, removed, unchanged;
    for (const auto& g : newGuids)
        (oldSet.count(g) ? unchanged : added).push_back(g);
    for (const auto& g : oldGuids)
        if (!newSet.count(g))
            removed.push_back(g);

    if (added.empty() && removed.empty())
    {
        RenderGuidList("GUIDs", unchanged);   //. nothing changed -- plain list
        return;
    }

    if (!unchanged.empty())
        RenderGuidList("GUIDs", unchanged);

    if (!added.empty())
        RenderGuidList(added.size() == 1 ? "Added GUID" : "Added GUIDs", added, &kNewColor);

    if (!removed.empty())
        RenderGuidList(removed.size() == 1 ? "Removed GUID" : "Removed GUIDs", removed, &kDuplicateColor);
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderJsonValue
//------------------------------------------------------------------------------
// Prints one key/value pair outside the confirmed effect/category schema
// (name/description/guids/behaviors) -- a forward-compat fallback, e.g. for
// a field a future VfxDenoiser version adds, rendered generically by JSON
// type so it shows up as *something* rather than silently vanishing.
//------------------------------------------------------------------------------
void RenderJsonValue(const std::string& key, const nlohmann::ordered_json& value)
{
    switch (value.type())
    {
        case nlohmann::ordered_json::value_t::string:
            ImGui::BulletText("%s: %s", key.c_str(), value.get<std::string>().c_str());
            break;
        case nlohmann::ordered_json::value_t::boolean:
            ImGui::BulletText("%s: %s", key.c_str(), value.get<bool>() ? "true" : "false");
            break;
        case nlohmann::ordered_json::value_t::number_integer:
        case nlohmann::ordered_json::value_t::number_unsigned:
            ImGui::BulletText("%s: %lld", key.c_str(), static_cast<long long>(value.get<int64_t>()));
            break;
        case nlohmann::ordered_json::value_t::number_float:
            ImGui::BulletText("%s: %g", key.c_str(), value.get<double>());
            break;
        case nlohmann::ordered_json::value_t::null:
            ImGui::BulletText("%s: (null)", key.c_str());
            break;
        case nlohmann::ordered_json::value_t::array:
        case nlohmann::ordered_json::value_t::object:
        default:
            //_ Unknown shape -- dump compactly rather than guess a
            // schema-specific rendering for a nested object/array.
            ImGui::Bullet();
            ImGui::TextWrapped("%s: %s", key.c_str(), value.dump().c_str());
            break;
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderBehavior
//------------------------------------------------------------------------------
// Renders one entry of an effect's "behaviors" array. Confirmed real shape
// (sample Collection.json): type Hide/Show/SetDuration, caster Self/
// Others/All, plus a "duration" (ms, per VfxDenoiser's README) only when
// type is SetDuration.
//------------------------------------------------------------------------------
void RenderBehavior(const nlohmann::ordered_json& behavior)
{
    std::string type   = behavior.value("type", std::string("?"));
    std::string caster = behavior.value("caster", std::string("?"));

    if (type == "SetDuration" && behavior.contains("duration") && behavior["duration"].is_number())
    {
        ImGui::BulletText("Set duration: %gms for %s", behavior["duration"].get<double>(), caster.c_str());
    }
    else
    {
        ImGui::BulletText("%s for %s", type.c_str(), caster.c_str());
    }

    //_ Anything beyond type/caster/duration is unexpected -- surface it
    // rather than silently dropping it.
    for (const auto& [key, value] : behavior.items())
    {
        if (key == "type" || key == "caster" || key == "duration")
            continue;
        ImGui::Indent();
        RenderJsonValue(key, value);
        ImGui::Unindent();
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderConflictSources
//------------------------------------------------------------------------------
// Renders the discarded/disagreeing settings recorded on a merge conflict
// (see MergePlanMergeCandidate in merge.h and "__vfxd_conflict_sources" in
// installed_tree_overlay.cpp) -- one block per other matched candidate,
// naming which effect it came from and its own behaviors. This is what
// "review before applying" is asking the user to look at, shown right
// where the warning already is.
//------------------------------------------------------------------------------
void RenderConflictSources(const nlohmann::ordered_json& effect)
{
    if (!effect.contains("__vfxd_conflict_sources") || !effect["__vfxd_conflict_sources"].is_array())
        return;

    for (const auto& src : effect["__vfxd_conflict_sources"])
    {
        std::string name     = src.value("name", std::string("(unnamed effect)"));
        std::string category = src.value("category", std::string());

        if (category.empty())
            ImGui::TextColored(kDuplicateColor, "From \"%s\":", name.c_str());
        else
            ImGui::TextColored(kDuplicateColor, "From \"%s\" (in %s):", name.c_str(), category.c_str());

        ImGui::Indent();
        if (src.contains("behaviors") && src["behaviors"].is_array() && !src["behaviors"].empty())
        {
            for (const auto& behavior : src["behaviors"])
                RenderBehavior(behavior);
        }
        else
        {
            ImGui::TextDisabled("(no behaviors configured)");
        }
        ImGui::Unindent();
    }
}

//******************************************************************************
// OverlayCacheEntry
//------------------------------------------------------------------------------
// generation   tree generation this copy was built from
// diffStatus   diff status this copy was built from
// file         the built (dupe/diff-tagged) copy of the installed tree
//------------------------------------------------------------------------------
// Per-sin cache entry backing s_overlayCache -- see the file header for why
// this cache exists. Invalidated on GetInstalledTreeGeneration() changing
// (file reloaded/edited) or the sin's own EDiffStatus changing (a diff
// produces one MergePlan per Ready transition; a reload always passes
// through NotLoaded/Loading first, which this also catches).
//------------------------------------------------------------------------------
struct OverlayCacheEntry
{
    int         generation = -1;
    EDiffStatus diffStatus = EDiffStatus::NotLoaded;
    nlohmann::ordered_json file;
};
//_ Per-sin cache of built overlay trees, keyed by sin name.
std::unordered_map<std::string, OverlayCacheEntry> s_overlayCache;

//_ Raw ImGui input buffer for the installed-tree search box.
char        s_treeSearchBuf[256] = {};
//_ Lowercased from s_treeSearchBuf once per frame; every match/filter
// helper below compares against this, not the raw buffer.
std::string s_treeSearchQueryLower;

//_ Search starts only once this many characters are typed -- a shorter
// query matches too much to be useful and costs a needless per-keystroke
// tree walk.
constexpr size_t kMinTreeSearchLength = 3;

//_ True for exactly one frame, when s_treeSearchQueryLower just changed
// (see file header for why this gates forced-open/closed calls).
bool s_treeSearchQueryChanged = false;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderCategoryTree
//------------------------------------------------------------------------------
// Recursively renders one category node -- a TreeNode per category, with
// effects and subcategories nested underneath. `category`/`effects`/
// `categories` mirror the exact JSON shape merge.cpp walks.
//
// `pathSoFar` is pushed/popped in place so anything rendered inside knows
// its own category's index path -- root to immediate parent, each element
// that level's index in its parent's "categories" array -- which is how
// right-click-to-edit identifies and later re-finds a category or effect
// (see EditState::originalIndex's comment). `myIndex` is this category's
// own position in its parent's array, supplied by the caller. `sinName`
// scopes edit state per installed file. Caller must PushID a stable
// per-sibling key before calling, so same-named siblings don't collide in
// imgui's ID stack.
//
// `forceShow` is true once an ancestor already matched the search box
// directly, showing this whole subtree unfiltered from there down (like a
// folder search that also shows everything inside a matched folder). Only
// ever set by this function itself, on the recursive call for its own
// subcategories.
//
// Force-open checks below run only on the frame the search query changed,
// never gated on whether a search is currently active -- running them
// every frame instead (a full subtree walk each time) previously stalled
// scrolling on a large tree; skipping them once search ends would leave
// nodes force-open forever (see file header).
//------------------------------------------------------------------------------
void RenderCategoryTree(const std::string& sinName, const nlohmann::ordered_json& category,
                         std::vector<int>& pathSoFar, int myIndex, bool forceShow = false)
{
    std::string name = category.value("name", std::string("(unnamed category)"));
    pathSoFar.push_back(myIndex);

    bool searchActive = !s_treeSearchQueryLower.empty();

    //_ An unmatched subtree isn't drawn at all (not even collapsed). Cancel
    // any edit scoped under here first -- nothing should keep running
    // invisibly (see file header).
    if (searchActive && !forceShow && !CategorySubtreeMatchesSearch(category, s_treeSearchQueryLower))
    {
        if (IsCategoryRenameUnderPath(sinName, pathSoFar))
            CancelCategoryEdit();
        if (IsEffectEditUnderPath(sinName, pathSoFar))
            CancelEdit();
        if (IsCategoryCreateUnderPath(sinName, pathSoFar))
            CancelCreateCategory();
        if (IsDeleteConfirmUnderPath(sinName, pathSoFar))
            CancelDeleteConfirm();

        //_ Not visited this frame -- on a query change, reset any
        // force-opened state an earlier query left here (see
        // SilentlyCloseSubtree's own comment).
        if (s_treeSearchQueryChanged)
            SilentlyCloseSubtree(category);

        pathSoFar.pop_back();
        return;
    }

    //_ Whether THIS category matched directly, vs. only containing a match
    // below -- decides whether its children get filtered individually or
    // shown in full.
    bool categoryMatchesDirectly = !searchActive || forceShow ||
        ContainsCI(name, s_treeSearchQueryLower) ||
        ContainsCI(category.value("description", std::string()), s_treeSearchQueryLower);

    //_ Forced open only if collapsing would hide a match not already
    // visible on the row itself -- own description, or a descendant match
    // (see function header for the query-change gating).
    if (s_treeSearchQueryChanged)
    {
        bool categoryNeedsForceOpen = searchActive &&
            (CategoryDescriptionMatches(category, s_treeSearchQueryLower) ||
             CategoryHasDescendantMatch(category, s_treeSearchQueryLower));
        ImGui::SetNextItemOpen(categoryNeedsForceOpen, ImGuiCond_Always);
    }

    bool isRenamingThis = IsCategoryBeingRenamed(sinName, pathSoFar);
    bool isDeletingThisCategory = IsDeletingThisCategory(sinName, pathSoFar);
    bool isCreatingHere = IsCreatingCategoryAt(sinName, pathSoFar);

    bool categoryHasDupe     = category.value("__vfxd_hasdupe", false);
    bool categoryHasConflict = category.value("__vfxd_hasconflict", false);
    bool categoryHasRework   = category.value("__vfxd_hasrework", false);
    bool categoryHasNew      = category.value("__vfxd_hasnew", false);
    bool categoryVirtual     = category.value("__vfxd_virtual", false);

    //_ Priority: dupe/conflict (red) needs a second look before trusting
    // which effect is which; failing that, rework (orange) outranks new
    // (green) as the thing worth double-checking.
    const ImVec4* categoryTint = nullptr;
    if (categoryHasDupe || categoryHasConflict)
        categoryTint = &kDuplicateColor;
    else if (categoryHasRework)
        categoryTint = &kReworkColor;
    else if (categoryHasNew)
        categoryTint = &kNewColor;

    if (categoryTint)
        ImGui::PushStyleColor(ImGuiCol_Text, *categoryTint);
    bool categoryOpen = ImGui::TreeNode(name.c_str());
    if (categoryTint)
        ImGui::PopStyleColor();

    //_ Closed on the query-change frame -> descendants won't be visited to
    // reset their own force-open state, so do it here instead (same
    // reasoning as the search-skip branch above).
    if (!categoryOpen && s_treeSearchQueryChanged)
    {
        //_ TreeNode only pushes its ID scope when open; enter it manually
        // here to match the IDs the real render pass would use.
        ImGui::PushID(name.c_str());
        SilentlyCloseChildren(category);
        ImGui::PopID();
    }

    //_ Drop target for an effect dragged from elsewhere in this sin file --
    // attaches to the row itself, so it accepts a drop whether open or
    // collapsed. Not offered on a "__vfxd_virtual" overlay category, same
    // reason as Rename: nothing real on disk yet to move into.
    if (!categoryVirtual && ImGui::BeginDragDropTarget())
    {
        if (ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
        {
            //_ Real payload lives in GetEffectDragPayload(), not the drop
            // bytes (see EffectDragPayload). Guard the sin match since only
            // same-sin moves are offered; skip if already last in this
            // category to avoid a pointless no-op rewrite+.bak.
            const EffectDragPayload& dragPayload = GetEffectDragPayload();
            bool sameCategory = dragPayload.originalPath == pathSoFar;
            bool alreadyLast  = sameCategory && category.contains("effects") && category["effects"].is_array() &&
                                dragPayload.originalIndex == static_cast<int>(category["effects"].size()) - 1;
            if (dragPayload.sinName == sinName && !alreadyLast)
            {
                EffectMoveJob job;
                job.sinName          = dragPayload.sinName;
                job.originalPath     = dragPayload.originalPath;
                job.effectName       = dragPayload.effectName;
                job.originalIndex    = dragPayload.originalIndex;
                job.destinationPath  = pathSoFar;
                job.destinationIndex = -1; // append -- see EffectMoveJob's comment

                QueueEffectMove(std::move(job));
            }
        }

        if (ImGui::AcceptDragDropPayload("VFXD_CATEGORY"))
        {
            //_ Reorder-only (see file header): dropped here means "append
            // to my children" if I'm the dragged category's parent, or
            // "insert above me" if we share a parent; a different parent
            // entirely would be reparenting, so it's silently ignored.
            if (GetCategoryDragSinName() == sinName && !GetCategoryDragPath().empty())
            {
                const std::vector<int>& dragPath = GetCategoryDragPath();
                std::vector<int> draggedParentPath(dragPath.begin(), dragPath.end() - 1);
                int              draggedIndex = dragPath.back();

                std::vector<int> myParentPath = pathSoFar;
                myParentPath.pop_back();

                if (pathSoFar == draggedParentPath)
                {
                    bool alreadyLast = category.contains("categories") && category["categories"].is_array() &&
                                       draggedIndex == static_cast<int>(category["categories"].size()) - 1;   //. append case, same no-op check as effect target
                    if (!alreadyLast)
                        QueueCategoryMove(GetCategoryDragSinName(), dragPath, -1);
                }
                else if (myParentPath == draggedParentPath)
                {
                    //_ Insert above this category; skip dropping on itself
                    // or on the sibling right after it (post-erase-shift,
                    // that would land it right back where it started).
                    bool noOp = (draggedIndex == myIndex) || (draggedIndex == myIndex - 1);
                    if (!noOp)
                        QueueCategoryMove(GetCategoryDragSinName(), dragPath, myIndex);
                }
                //_ else: different parent = reparenting, not offered
            }
        }

        ImGui::EndDragDropTarget();
    }

    //_ Reorder among current siblings only (see file header). Gated on
    // AnyEditInFlight so a drag can't start mid-edit elsewhere.
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginDragDropSource())
    {
        BeginCategoryDrag(sinName, pathSoFar);
        ImGui::Text("Move \"%s\"", name.c_str());
        ImGui::EndDragDropSource();
    }

    //_ Only offered when no other edit is in flight anywhere, and never on
    // a "__vfxd_virtual" overlay category (see BuildDiffOverlayTree).
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginPopupContextItem("category_ctx"))
    {
        if (ImGui::MenuItem("Rename"))
            BeginCategoryEdit(sinName, pathSoFar, name);
        ImGui::EndPopup();
    }

    //_ Rendered after the context menu above so they don't steal "last
    // item" from the TreeNode. Delete stays disabled while this category
    // has any content -- never silently deletes it. "+" only shows once
    // open, since it adds something *inside* what's being looked at.
    if (!categoryVirtual)
    {
        bool categoryEmpty = (!category.contains("effects") || category["effects"].empty()) &&
                              (!category.contains("categories") || category["categories"].empty());
        bool deleteDisabled = !categoryEmpty || (AnyEditInFlight() && !isDeletingThisCategory);

        ImGui::SameLine();
        if (deleteDisabled)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        bool deleteClicked = ImGui::SmallButton("-##delcat");
        if (deleteDisabled)
            ImGui::PopStyleVar();
        if (deleteClicked && !deleteDisabled)
            BeginDeleteConfirm(sinName, pathSoFar, -1, /*isCategory=*/true, name);

        if (categoryOpen)
        {
            bool createDisabled = AnyEditInFlight() && !isCreatingHere;
            ImGui::SameLine();
            if (createDisabled)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            bool createClicked = ImGui::SmallButton("+##addcat");
            if (createDisabled)
                ImGui::PopStyleVar();
            if (createClicked && !createDisabled)
                BeginCreateCategory(sinName, pathSoFar);
        }
    }

    //_ Sits right below this row, not nested inside the TreeNode's
    // collapsible content -- visible whether the node is open or collapsed.
    if (isDeletingThisCategory)
        RenderDeleteConfirm();

    if (categoryOpen)
    {
        if (isRenamingThis)
            RenderCategoryEditor();
        if (isCreatingHere)
            RenderCreateCategoryEditor();

        if (category.contains("description") && category["description"].is_string())
        {
            std::string desc = category["description"].get<std::string>();
            if (!desc.empty())
                ImGui::TextWrapped("%s", desc.c_str());
        }

        if (category.contains("effects") && category["effects"].is_array())
        {
            int i = 0;
            for (const auto& effect : category["effects"])
            {
                const int effIndex = i++;

                //_ Hidden by search -- not matched by category or effect.
                // Cancel any edit in flight (see file header) since it
                // won't be drawn at all this frame.
                if (searchActive && !categoryMatchesDirectly && !EffectMatchesSearch(effect, s_treeSearchQueryLower))
                {
                    bool isEditingThisHidden = IsEffectBeingEdited(sinName, pathSoFar, effIndex);
                    if (isEditingThisHidden)
                        CancelEdit();

                    bool isDeletingThisHidden = IsDeletingThisEffect(sinName, pathSoFar, effIndex);
                    if (isDeletingThisHidden)
                        CancelDeleteConfirm();

                    //_ Not visited this frame -- on a query change, reset
                    // this effect's own stored open state too.
                    if (s_treeSearchQueryChanged)
                    {
                        ImGui::PushID(effIndex);
                        ImGui::GetStateStorage()->SetInt(ImGui::GetID("effect"), 0);
                        ImGui::PopID();
                    }

                    continue;
                }

                ImGui::PushID(effIndex);

                std::string effName = effect.value("name", std::string("(unnamed effect)"));
                //_ Identity is (sinName, path, index) -- NOT name. Siblings
                // can share a name (VfxDenoiser doesn't require uniqueness);
                // matching by name would make every same-named sibling
                // think it was the one being edited.
                bool isEditingThis = IsEffectBeingEdited(sinName, pathSoFar, effIndex);
                bool isDeletingThisEffect = IsDeletingThisEffect(sinName, pathSoFar, effIndex);

                bool effIsDupe     = effect.value("__vfxd_dupe_guid", false);
                bool effIsNew      = effect.value("__vfxd_new", false);
                bool effIsRework   = effect.value("__vfxd_rework", false);
                bool effIsConflict = effect.value("__vfxd_conflict", false);

                //_ Hollowed out by a GUID drag-merge, or by deleting the
                // last GUID by hand -- see "Delete Empty" below. Alpha-dimmed
                // rather than text-colored, so it layers with the tints below.
                bool effIsEmptyGuids = !effect.contains("guids") || !effect["guids"].is_array() || effect["guids"].empty();
                if (effIsEmptyGuids)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);

                //_ A settings conflict is just as much a "review before
                // applying" situation as a duplicate guid -- same red tint,
                // same top priority.
                if (effIsDupe || effIsConflict)
                    ImGui::PushStyleColor(ImGuiCol_Text, kDuplicateColor);
                else if (effIsNew)
                    ImGui::PushStyleColor(ImGuiCol_Text, kNewColor);
                else if (effIsRework)
                    ImGui::PushStyleColor(ImGuiCol_Text, kReworkColor);

                //_ Forced open only if it matched through hidden content
                // (description/GUID) -- a name match is already visible on
                // the row. Query-change gating as above (see file header).
                bool effectNeedsForceOpen = searchActive && EffectHiddenContentMatches(effect, s_treeSearchQueryLower);
                if (s_treeSearchQueryChanged)
                    ImGui::SetNextItemOpen(effectNeedsForceOpen, ImGuiCond_Always);
                bool nodeOpen = ImGui::TreeNode("effect", "%s%s%s", effName.c_str(),
                                                isEditingThis ? " (editing)" : "",
                                                effIsEmptyGuids ? " (empty)" : "");

                if (effIsDupe || effIsNew || effIsRework || effIsConflict)
                    ImGui::PopStyleColor();
                if (effIsEmptyGuids)
                    ImGui::PopStyleVar();

                //_ Places a dragged effect immediately above this row --
                // complements the category-row target above so together
                // they reach every position. Not offered on an overlay-only
                // effect (__vfxd_new/__vfxd_rework): no stable real on-disk
                // position while only previewed.
                if (!effIsNew && !effIsRework && ImGui::BeginDragDropTarget())
                {
                    if (ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
                    {
                        //_ Same same-sin guard as the category target, plus
                        // two no-op cases: dropped on itself, or on the
                        // effect right after it (post-erase-shift, that
                        // would land it back where it started).
                        const EffectDragPayload& dragPayload = GetEffectDragPayload();
                        bool sameCategory = dragPayload.originalPath == pathSoFar;
                        bool noOp = sameCategory && (dragPayload.originalIndex == effIndex ||
                                                      dragPayload.originalIndex == effIndex - 1);
                        if (dragPayload.sinName == sinName && !noOp)
                        {
                            EffectMoveJob job;
                            job.sinName          = dragPayload.sinName;
                            job.originalPath     = dragPayload.originalPath;
                            job.effectName       = dragPayload.effectName;
                            job.originalIndex    = dragPayload.originalIndex;
                            job.destinationPath  = pathSoFar;
                            job.destinationIndex = effIndex;

                            QueueEffectMove(std::move(job));
                        }
                    }

                    //_ Every effect row is a valid target except the one
                    // currently being edited (see QueueGuidMerge) and itself.
                    if (!isEditingThis && ImGui::AcceptDragDropPayload("VFXD_GUID"))
                    {
                        const GuidDragPayload& guidPayload = GetGuidDragPayload();
                        bool sameEffect = guidPayload.originalPath == pathSoFar && guidPayload.originalIndex == effIndex;
                        if (guidPayload.sinName == sinName && !sameEffect)
                        {
                            GuidMergeJob job;
                            job.sinName               = guidPayload.sinName;
                            job.originalPath          = guidPayload.originalPath;
                            job.originalIndex         = guidPayload.originalIndex;
                            job.effectName            = guidPayload.effectName;
                            job.guid                  = guidPayload.guid;
                            job.destinationPath       = pathSoFar;
                            job.destinationIndex      = effIndex;
                            job.destinationEffectName = effName;

                            QueueGuidMerge(std::move(job));
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                //_ Not offered on an overlay-only effect, same reasoning as
                // the drop target above. Gated on AnyEditInFlight so a drag
                // can't start mid-edit elsewhere.
                if (!effIsNew && !effIsRework && !AnyEditInFlight() && ImGui::BeginDragDropSource())
                {
                    BeginEffectDrag(sinName, pathSoFar, effName, effIndex);
                    ImGui::SetDragDropPayload("VFXD_EFFECT", &kEffectDragMarker, sizeof(kEffectDragMarker));
                    ImGui::Text("Move \"%s\"", effName.c_str());
                    ImGui::EndDragDropSource();
                }

                //_ Only offered when no edit is in flight anywhere, and
                // never on an overlay-only effect -- nothing at
                // pathSoFar/effIndex in the real file is guaranteed to be
                // this same effect until the update is applied.
                if (!effIsNew && !effIsRework && !AnyEditInFlight() && ImGui::BeginPopupContextItem("effect_ctx"))
                {
                    if (ImGui::MenuItem("Edit"))
                        BeginEdit(sinName, pathSoFar, effIndex, effect);
                    ImGui::EndPopup();
                }

                //_ Never grayed out for emptiness (unlike a category) --
                // only while some other edit/delete/create/rename is in
                // flight elsewhere. Not offered on an overlay-only effect.
                if (!effIsNew && !effIsRework)
                {
                    bool deleteDisabled = AnyEditInFlight() && !isDeletingThisEffect;
                    ImGui::SameLine();
                    if (deleteDisabled)
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                    bool deleteClicked = ImGui::SmallButton("-##deleff");
                    if (deleteDisabled)
                        ImGui::PopStyleVar();
                    if (deleteClicked && !deleteDisabled)
                        BeginDeleteConfirm(sinName, pathSoFar, effIndex, /*isCategory=*/false, effName);
                }

                //_ sits below the row, visible whether nodeOpen or not
                if (isDeletingThisEffect)
                    RenderDeleteConfirm();

                if (nodeOpen)
                {
                    if (isEditingThis)
                    {
                        RenderEffectEditor();
                    }
                    else
                    {
                        if (effIsDupe)
                            ImGui::TextColored(kDuplicateColor,
                                "One or more of this effect's GUIDs is also used by another installed effect. "
                                "This shouldn't normally happen and updates are blocked for this file until it's resolved.");
                        else if (effIsNew)
                            ImGui::TextColored(kNewColor, "New from a pending update -- not yet applied.");
                        else if (effIsRework)
                        {
                            int mergedCount = effect.value("__vfxd_merged_count", 0);
                            bool renamed    = effect.contains("__vfxd_old_name");
                            bool movedCat   = effect.contains("__vfxd_old_category");

                            if (mergedCount > 0)
                            {
                                std::string msg = "This effect and " + std::to_string(mergedCount) +
                                                   (mergedCount == 1 ? " other effect" : " other effects") +
                                                   " would be merged into this one by a pending update.";
                                ImGui::TextColored(effIsConflict ? kDuplicateColor : kReworkColor, "%s", msg.c_str());
                                if (effIsConflict)
                                    ImGui::TextColored(kDuplicateColor,
                                        "The merged effect(s) had different settings -- shown below, review before applying.");
                            }
                            else if (effIsConflict)
                            {
                                //_ A GUID this effect absorbed used to
                                // belong to another effect surviving under
                                // its own separate update -- still worth a
                                // second look even though nothing's lost.
                                ImGui::TextColored(kDuplicateColor,
                                    "This effect absorbed a GUID from another effect with different settings -- shown below, review before applying.");
                            }
                            else if (renamed || movedCat)
                            {
                                ImGui::TextColored(kReworkColor,
                                    "This effect's GUIDs, name, and/or category would be updated by a pending update.");
                            }
                            else
                            {
                                ImGui::TextColored(kReworkColor,
                                    "GUIDs would be refreshed by a pending update -- name/category/settings stay as they are.");
                            }

                            if (renamed)
                                ImGui::TextColored(kReworkColor, "Renamed from \"%s\".",
                                    effect.value("__vfxd_old_name", std::string()).c_str());
                            if (movedCat)
                                ImGui::TextColored(kReworkColor, "Moved from \"%s\".",
                                    effect.value("__vfxd_old_category", std::string()).c_str());

                            //_ what the conflict warning above asks to review
                            if (effIsConflict)
                                RenderConflictSources(effect);
                        }

                        if (effect.contains("description") && effect["description"].is_string())
                        {
                            std::string desc = effect["description"].get<std::string>();
                            if (!desc.empty())
                                ImGui::TextWrapped("%s", desc.c_str());
                        }

                        std::vector<std::string> guids;
                        if (effect.contains("guids") && effect["guids"].is_array())
                            for (const auto& g : effect["guids"])
                                if (g.is_string())
                                    guids.push_back(g.get<std::string>());

                        if (effIsRework)
                        {
                            //_ Current (default color) and pending-update
                            // (kReworkColor) stacked, so both are visible
                            // without needing to apply first.
                            std::vector<std::string> newGuids;
                            if (effect.contains("__vfxd_new_guids") && effect["__vfxd_new_guids"].is_array())
                                for (const auto& g : effect["__vfxd_new_guids"])
                                    if (g.is_string())
                                        newGuids.push_back(g.get<std::string>());

                            RenderGuidDiff(guids, newGuids);
                        }
                        else
                        {
                            //_ effIsNew still needs checking here -- an overlay-only
                            // effect has no stable on-disk position (same reasoning
                            // as the row's own guards above), so its GUIDs can't drag.
                            if (effIsNew)
                            {
                                RenderGuidList("guids", guids);
                            }
                            else
                            {
                                GuidListDragContext dragContext;
                                dragContext.sinName    = sinName;
                                dragContext.path       = pathSoFar;
                                dragContext.index      = effIndex;
                                dragContext.effectName = effName;
                                RenderGuidList("guids", guids, nullptr, &dragContext);
                            }
                        }

                        if (effect.contains("behaviors") && effect["behaviors"].is_array())
                        {
                            ImGui::TextDisabled("Behaviors (owned by VfxDenoiser):");
                            for (const auto& behavior : effect["behaviors"])
                                RenderBehavior(behavior);
                        }

                        //_ Anything beyond the confirmed schema is
                        // unexpected -- surface it rather than drop it.
                        for (const auto& [key, value] : effect.items())
                        {
                            if (key == "name" || key == "description" || key == "guids" || key == "behaviors"
                                || key == "__vfxd_new" || key == "__vfxd_rework" || key == "__vfxd_new_guids"
                                || key == "__vfxd_hasnew" || key == "__vfxd_hasrework"
                                || key == "__vfxd_dupe_guid" || key == "__vfxd_hasdupe"
                                || key == "__vfxd_old_name" || key == "__vfxd_old_category"
                                || key == "__vfxd_merged_count" || key == "__vfxd_conflict"
                                || key == "__vfxd_conflict_sources")
                                continue;
                            RenderJsonValue(key, value);
                        }
                    }

                    ImGui::TreePop();
                }
                else if (isEditingThis)
                {
                    //_ collapsing mid-edit cancels it, same as the category case
                    CancelEdit();
                }

                ImGui::PopID();
            }
        }

        if (category.contains("categories") && category["categories"].is_array())
        {
            int i = 0;
            for (const auto& sub : category["categories"])
            {
                ImGui::PushID(i);
                RenderCategoryTree(sinName, sub, pathSoFar, i, categoryMatchesDirectly);
                ImGui::PopID();
                ++i;
            }
        }

        ImGui::TreePop();
    }
    else
    {
        //_ Collapsed -- nothing inside (rename UI, create prompt, effect
        // editor) is drawn this frame, so cancel rather than let it run
        // invisibly. DeleteConfirmState is deliberately excluded -- its
        // row sits outside the collapsible content (see its own comment).
        if (IsCategoryRenameUnderPath(sinName, pathSoFar))
            CancelCategoryEdit();
        if (IsEffectEditUnderPath(sinName, pathSoFar))
            CancelEdit();
        if (IsCategoryCreateUnderPath(sinName, pathSoFar))
            CancelCreateCategory();
    }

    pathSoFar.pop_back();
}

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderInstalledEffects
//------------------------------------------------------------------------------
// Draws the "Installed Effects" section: one top-level TreeNode per
// installed sin file, each expanding into that file's real category tree
// via RenderCategoryTree. Read-only browsing by default; right-clicking an
// effect offers "Edit". Independent of whether a GitHub update is
// available.
//------------------------------------------------------------------------------
void RenderInstalledEffects(const std::string& denoiserAddonDir)
{
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    if (ImGui::Button("Refresh##installed_tree"))
        LoadInstalledEffectsTree(denoiserAddonDir);

    ImGui::SameLine();
    //_ Greyed out while another edit's in flight, same as the per-effect
    // "-" delete button -- not gated on there being anything empty yet,
    // that's re-checked when the confirm itself renders (see below).
    {
        bool deleteEmptyDisabled = AnyEditInFlight() && !IsDeleteEmptyConfirmActive();
        if (deleteEmptyDisabled)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        bool deleteEmptyClicked = ImGui::Button("Delete Empty##installed_tree");
        if (deleteEmptyDisabled)
            ImGui::PopStyleVar();
        if (deleteEmptyClicked && !deleteEmptyDisabled)
            BeginDeleteEmptyConfirm();
    }
    if (IsDeleteEmptyConfirmActive())
        RenderDeleteEmptyConfirm();

    ImGui::TextDisabled("Drag an effect onto a category to move it to the end of that category,\n"
                         "or onto another effect to place it just above that one.\n"
                         "Categories can be dragged only to reorder them in the same parent category.\n"
                         "GUIDs can be dragged onto other effects.\n"
                         "Right-click unfolded effects or categories to edit them.");

    //_ Recomputes the lowercased query RenderCategoryTree's matching
    // helpers compare against; the actual filtering happens down there.
    ImGui::InputTextWithHint("##installed_tree_search", "Search name / category / description / GUID...",
                              s_treeSearchBuf, sizeof(s_treeSearchBuf));
    if (s_treeSearchBuf[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##installed_tree_search"))
            s_treeSearchBuf[0] = '\0';
    }
    std::string typedLower = s_treeSearchBuf;
    std::transform(typedLower.begin(), typedLower.end(), typedLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (!typedLower.empty() && typedLower.size() < kMinTreeSearchLength)
        ImGui::TextDisabled("Keep typing... (search starts at %zu characters)", kMinTreeSearchLength);

    //. below the minimum, treat the query as empty (no filtering/expansion)
    std::string newQueryLower = (typedLower.size() >= kMinTreeSearchLength) ? typedLower : std::string();

    //. see s_treeSearchQueryChanged's own comment for why this matters
    s_treeSearchQueryChanged = (newQueryLower != s_treeSearchQueryLower);
    s_treeSearchQueryLower   = std::move(newQueryLower);

    if (!GetEditResultMessage().empty())
        ImGui::TextWrapped("%s", GetEditResultMessage().c_str());

    if (GetInstalledSins().empty())
    {
        ImGui::TextDisabled("No Visual Sins effect files found in VfxDenoiser's folder.");
        return;
    }

    //_ A Ready diff plan overlays pending-update coloring onto this same
    // tree (BuildDiffOverlayTree) rather than a separate list. Sins with
    // no plan yet just render the plain on-disk tree.
    std::vector<SinDiffInfo> diffs = GetSinDiffInfo();
    bool anyOverlayShown  = false;
    bool anyConflictShown = false;

    for (const auto& sin : GetInstalledSins())
    {
        ImGui::PushID(sin.sinName.c_str());

        const nlohmann::ordered_json* installedFile = FindInstalledJson(sin.sinName);
        if (!installedFile)
        {
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s: couldn't read %s", sin.sinName.c_str(), sin.fileName.c_str());
            ImGui::PopID();
            continue;
        }

        const SinDiffInfo* diff = nullptr;
        for (const auto& d : diffs)
            if (d.sinName == sin.sinName)
                diff = &d;

        bool hasOverlay = diff && diff->status == EDiffStatus::Ready && !diff->plan.IsEmpty();

        const auto& duplicateGuidsBySin = GetDuplicateGuidsBySin();
        auto        dupIt    = duplicateGuidsBySin.find(sin.sinName);
        bool        hasDupes = dupIt != duplicateGuidsBySin.end() && !dupIt->second.empty();

        //_ Duplicate-guid tagging first (a property of the file itself),
        // then the pending-update diff on the same copy -- both can
        // coexist on one node; RenderCategoryTree picks red over
        // orange/green. Only ever a copy -- see OverlayCacheEntry.
        const nlohmann::ordered_json* fileToRender = installedFile;

        if (hasDupes || hasOverlay)
        {
            EDiffStatus statusForCache = diff ? diff->status : EDiffStatus::NotLoaded;
            OverlayCacheEntry& cached = s_overlayCache[sin.sinName];
            bool stale = cached.generation != GetInstalledTreeGeneration() || cached.diffStatus != statusForCache;

            if (stale)
            {
                nlohmann::ordered_json built = *installedFile;
                if (hasDupes)
                    built = BuildDuplicateOverlayTree(built, dupIt->second);
                if (hasOverlay)
                    built = BuildDiffOverlayTree(built, diff->plan);

                cached.generation = GetInstalledTreeGeneration();
                cached.diffStatus = statusForCache;
                cached.file       = std::move(built);
            }

            fileToRender = &cached.file;
            if (hasOverlay)
            {
                anyOverlayShown = true;
                for (const auto& rw : diff->plan.reworks)
                    if (rw.behaviorsConflict) { anyConflictShown = true; break; }
            }
        }

        if (hasDupes)
        {
            ImGui::TextColored(kDuplicateColor,
                "%s: duplicate GUID(s) detected in this file -- updates are blocked until this is resolved (see red entries below).",
                sin.sinName.c_str());
        }

        //_ Whether this file has any match at all -- lets the root row
        // force itself open, and lets an empty result say so further down.
        bool searchActive   = !s_treeSearchQueryLower.empty();
        bool anyMatchInFile = false;
        if (searchActive && fileToRender->contains("categories") && (*fileToRender)["categories"].is_array())
            for (const auto& cat : (*fileToRender)["categories"])
                if (CategorySubtreeMatchesSearch(cat, s_treeSearchQueryLower))
                {
                    anyMatchInFile = true;
                    break;
                }

        //. same query-change force-open/shut gating as RenderCategoryTree
        if (s_treeSearchQueryChanged)
            ImGui::SetNextItemOpen(anyMatchInFile, ImGuiCond_Always);

        bool rootOpen = ImGui::TreeNode("root", "%s (%s)", sin.sinName.c_str(), sin.fileName.c_str());

        //_ Same fix as RenderCategoryTree's collapsed branch: closed on a
        // query-change frame, so reset descendants' state here instead.
        // "root" is TreeNode's str_id, pushed to match its real ID scope.
        if (!rootOpen && s_treeSearchQueryChanged)
        {
            ImGui::PushID("root");
            if (fileToRender->contains("categories") && (*fileToRender)["categories"].is_array())
            {
                int i = 0;
                for (const auto& cat : (*fileToRender)["categories"])
                {
                    ImGui::PushID(i);
                    SilentlyCloseSubtree(cat);
                    ImGui::PopID();
                    ++i;
                }
            }
            ImGui::PopID();
        }

        if (rootOpen)
        {
            std::vector<int> path;   //. this sin file's top level -- empty path

            //_ Reorder-only (see file header): this root row is the
            // "shared parent's own row" a top-level category doesn't
            // otherwise have. Only offered within the same sin file.
            if (ImGui::BeginDragDropTarget())
            {
                if (ImGui::AcceptDragDropPayload("VFXD_CATEGORY"))
                {
                    if (GetCategoryDragSinName() == sin.sinName && GetCategoryDragPath().size() == 1)
                    {
                        const std::vector<int>& dragPath = GetCategoryDragPath();
                        int  draggedIndex = dragPath.back();
                        bool alreadyLast  = fileToRender->contains("categories") && (*fileToRender)["categories"].is_array() &&
                                            draggedIndex == static_cast<int>((*fileToRender)["categories"].size()) - 1;
                        if (!alreadyLast)
                            QueueCategoryMove(GetCategoryDragSinName(), dragPath, -1);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool isCreatingAtTopLevel = IsCreatingCategoryAt(sin.sinName, path);

            bool createDisabled = AnyEditInFlight() && !isCreatingAtTopLevel;
            ImGui::SameLine();
            if (createDisabled)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            bool createClicked = ImGui::SmallButton("+##addcat_top");
            if (createDisabled)
                ImGui::PopStyleVar();
            if (createClicked && !createDisabled)
                BeginCreateCategory(sin.sinName, path);

            if (isCreatingAtTopLevel)
                RenderCreateCategoryEditor();

            const nlohmann::ordered_json& file = *fileToRender;
            if (file.contains("categories") && file["categories"].is_array())
            {
                if (searchActive && !anyMatchInFile)
                {
                    ImGui::TextDisabled("(no matches in this file)");
                }
                else
                {
                    int i = 0;
                    for (const auto& cat : file["categories"])
                    {
                        ImGui::PushID(i);
                        RenderCategoryTree(sin.sinName, cat, path, i);
                        ImGui::PopID();
                        ++i;
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("(no categories in this file)");
            }
            ImGui::TreePop();
        }
        else if (IsCreatingCategoryAt(sin.sinName, std::vector<int>()))
        {
            //. collapsing hides the "+" button and prompt, so cancel it
            CancelCreateCategory();
        }

        ImGui::PopID();
    }

    if (anyOverlayShown)
    {
        ImGui::Spacing();
        ImGui::TextColored(kNewColor,    "* New effect from a pending update.");
        ImGui::TextColored(kReworkColor, "* GUIDs would be refreshed or merged or the name or category has changed.");
        if (anyConflictShown)
            ImGui::TextColored(kDuplicateColor, "* Merged effects had different settings -- review before applying");
    }
    if (!GetDuplicateGuidsBySin().empty())
    {
        bool anyDupes = false;
        for (const auto& [name, guids] : GetDuplicateGuidsBySin())
            if (!guids.empty()) { anyDupes = true; break; }
        if (anyDupes)
        {
            ImGui::Spacing();
            ImGui::TextColored(kDuplicateColor, "* Duplicate GUID shared with another installed effect -- resolve before updating");
        }
    }

    //_ Deferred to here, after every category/effect array has finished
    // iterating, so nothing is mutated mid-walk. All six called
    // unconditionally.
    ApplyPendingEdit();
    ApplyPendingCategoryRename();
    ApplyPendingMove();
    ApplyPendingCategoryMove();
    ApplyPendingDelete();
    ApplyPendingCreateCategory();
    ApplyPendingGuidMerge();
}