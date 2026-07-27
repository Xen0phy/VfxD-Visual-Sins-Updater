// installed_tree_view.cpp
//
// RenderCategoryTree + RenderInstalledEffects + their small render leaves,
// plus the overlay cache and tree-search state that are specific to this
// section. Extracted from addon.cpp -- a mechanical move, no behavior change.
// See installed_tree_view.h for what's exposed and why.
#include "ui/tree/installed_tree_view.h"
#include "imgui.h"
#include "addon/ui_colors.h"
#include "core/tree/installed_tree_store.h"
#include "core/tree/installed_tree_overlay.h"
#include "core/tree/installed_tree_search.h"
#include "ui/tree/installed_tree_edit.h"
#include "integration/github_update.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <utility>

namespace {

// `color` is optional -- pass nullptr for the default text color (used for
// a plain guids list), or a color to tint every bullet line (used to show
// a reworked effect's post-update GUIDs in kReworkColor, distinct from its
// current GUIDs just above in the default color).
void RenderGuidList(const char* label, const std::vector<std::string>& guids, const ImVec4* color = nullptr)
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
    for (const auto& g : guids)
        ImGui::BulletText("%s", g.c_str());
    if (color)
        ImGui::PopStyleColor();
    ImGui::Unindent();
}

// For a reworked effect, shows only what a pending update would actually
// change about its guid list, rather than the full current and full
// post-update lists side by side. Those two lists can share almost
// everything (a 1c merge folds dozens of untouched guids from the
// merged-away candidates straight into the survivor's list, see
// BuildMergedRework in merge.cpp), so printing both in full mostly
// repeats the same lines twice. Order doesn't carry meaning for this
// comparison -- guids are an unordered identity set as far as every merge
// decision is concerned (see GuidDiff in merge.cpp) -- so this is a
// straight set difference, not a positional diff. Guids present on both
// sides are still listed individually, same as a plain guid list always
// has been; only the genuinely added/removed ones get their own
// highlighted section, so a single real change doesn't get lost in (or
// require reprinting) everything that didn't change.
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
        // Nothing actually changed -- just the ordinary plain list.
        RenderGuidList("GUIDs", unchanged);
        return;
    }

    if (!unchanged.empty())
        RenderGuidList("GUIDs", unchanged);

    if (!added.empty())
        RenderGuidList(added.size() == 1 ? "Added GUID" : "Added GUIDs", added, &kNewColor);

    if (!removed.empty())
        RenderGuidList(removed.size() == 1 ? "Removed GUID" : "Removed GUIDs", removed, &kDuplicateColor);
}


// Prints one key/value pair that isn't part of the confirmed effect/
// category schema below (name/description/guids/behaviors). This is only
// a forward-compat fallback now -- e.g. if a future VfxDenoiser version
// adds a new field -- rendered generically by JSON type so an unknown
// field still shows up as *something* rather than silently vanishing.
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
            // Unknown shape -- dump it compactly rather than guess a
            // schema-specific rendering for a nested object/array.
            ImGui::Bullet();
            ImGui::TextWrapped("%s: %s", key.c_str(), value.dump().c_str());
            break;
    }
}

// Renders one entry of an effect's "behaviors" array. Confirmed real
// shape (from a sample Collection.json): {"type": "Hide"|"Show"|
// "SetDuration", "caster": "Self"|"Others"|"All", plus "duration"
// (milliseconds, per VfxDenoiser's own README) only when type is
// SetDuration}.
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

    // Anything beyond type/caster/duration is unexpected -- surface it
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

// Per-sin cache for the duplicate/diff overlay trees RenderInstalledEffects
// paints onto the installed tree (see BuildDuplicateOverlayTree/
// BuildDiffOverlayTree below). Building either means deep-copying the
// entire installed file plus a recursive tag pass -- real work for a large
// file (thousands of effects isn't unusual) -- and this used to happen again on every single
// ImGui frame the tree was open, whether or not anything had actually
// changed since the last frame. That's the direct cause of a reported bug:
// with an overlay active and the tree tall enough to need a scrollbar, the
// per-frame rebuild made frame time long enough that mouse-wheel scrolling
// felt like it had stopped working outright (wheel deltas the host
// accumulates between frames get eaten by a stalled one) -- reproducible
// only with an overlay showing and only once there was enough content to
// make the rebuild expensive, which matches exactly how it was reported.
// Invalidated on GetInstalledTreeGeneration() changing (the file itself was
// reloaded/edited) or the sin's own EDiffStatus changing (a diff only ever
// produces one MergePlan per Ready transition -- StartLoadDiff replacing
// it always goes through NotLoaded/Loading again first, which this catches
// too).
struct OverlayCacheEntry
{
    int         generation = -1;
    EDiffStatus diffStatus = EDiffStatus::NotLoaded;
    nlohmann::ordered_json file;
};
std::unordered_map<std::string, OverlayCacheEntry> s_overlayCache;

// ---------------------------------------------------------------------------
// Installed-effects tree search box (RenderInstalledEffects). A single text
// box filters every installed sin file's tree at once by name, category
// name, description, or GUID substring, case-insensitively. s_treeSearchBuf
// is the raw ImGui input buffer; s_treeSearchQueryLower is recomputed from
// it once per frame at the top of RenderInstalledEffects and is what the
// matching helpers below actually compare against, so nothing else in this
// file has to lowercase repeatedly.
char        s_treeSearchBuf[256] = {};
std::string s_treeSearchQueryLower;

// Search only actually starts once at least this many characters are
// typed -- a 1-2 character query matches almost everything in a typical
// tree anyway, so there's little value in it and it's needless work (both
// the matching itself and the expansion it triggers) on every keystroke
// along the way to a more useful query.
constexpr size_t kMinTreeSearchLength = 3;

// True only on the single frame where s_treeSearchQueryLower just changed
// from what it was last frame (recomputed once, at the top of
// RenderInstalledEffects). RenderCategoryTree gates its forced-open calls
// on this rather than on "a search is active" -- forcing potentially
// hundreds of nodes open is only meant to happen once, right when the
// query changes, not on every single frame the search box merely still has
// text in it. Re-forcing it every frame was expensive enough on a large
// tree to stall the whole overlay (dropped keystrokes, unresponsive
// scrolling) while typing.
bool s_treeSearchQueryChanged = false;

// Recursively walks one category node (read-only by default): a TreeNode
// per category, effects listed as nested TreeNodes underneath. `category`
// and `effects`/`categories` are exactly the same JSON shape merge.cpp
// walks, so this stays a faithful mirror of what ResolveMergePlan sees.
//
// `pathSoFar` is threaded through and mutated in place (pushed before
// descending, popped after) so any effect rendered inside knows its own
// category's identity (root -> immediate parent, each element that
// level's index within its parent's "categories" array) -- this is what
// lets right-click-to-edit identify, and later re-find, a specific
// category or effect, the same way effect identity is index-based (see
// EditState::originalIndex's comment). `myIndex` is this category's own
// position within its parent's "categories" array, supplied by the
// caller (which is already iterating that array to get here) since this
// function has no other way to know it. `sinName` identifies which
// installed file this tree belongs to, since edit state is scoped
// per-sin as well as per-path.
//
// Caller must PushID a stable per-sibling key (index is fine here, since
// this tree is rebuilt wholesale on every reload rather than mutated in
// place) before calling, so sibling categories that happen to share a
// name don't collide in imgui's ID stack.
//
// `forceShow` is true once an ancestor category has already matched the
// tree search box directly (by its own name/description) -- from that
// point down, the whole subtree is shown unfiltered, the same way a folder
// search that matches a folder name shows everything inside it rather than
// filtering further. Callers outside this function never need to pass it;
// it's only ever set by RenderCategoryTree itself on the recursive call for
// its own subcategories.
void RenderCategoryTree(const std::string& sinName, const nlohmann::ordered_json& category,
                         std::vector<int>& pathSoFar, int myIndex, bool forceShow = false)
{
    std::string name = category.value("name", std::string("(unnamed category)"));
    pathSoFar.push_back(myIndex);

    bool searchActive = !s_treeSearchQueryLower.empty();

    // Skip this category (and everything under it) entirely when a search
    // is active and nothing in this subtree matches -- an unrelated branch
    // just isn't drawn, rather than shown collapsed and empty-looking.
    // Cancel any edit state scoped under here first, same reasoning as the
    // "collapsed" branch further down: nothing inside is being drawn this
    // frame, so nothing should be left running invisibly.
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

        // This category (and everything under it) isn't being visited at
        // all this frame -- if the query just changed, anything under here
        // that was force-opened by an earlier, different query needs its
        // stored open state reset now, or it'll reappear already expanded
        // the next time this category matches again. See
        // SilentlyCloseSubtree's own comment for why.
        if (s_treeSearchQueryChanged)
            SilentlyCloseSubtree(category);

        pathSoFar.pop_back();
        return;
    }

    // Whether THIS category matched directly (as opposed to only containing
    // a match further down) -- decides whether its own effects/subcategories
    // get filtered individually below, or shown in full because the category
    // itself is what the search was looking for.
    bool categoryMatchesDirectly = !searchActive || forceShow ||
        ContainsCI(name, s_treeSearchQueryLower) ||
        ContainsCI(category.value("description", std::string()), s_treeSearchQueryLower);

    // Only force this category open if leaving it collapsed would hide
    // something: either its own description (only shown once open), or a
    // match somewhere in its subtree (whose row only becomes visible once
    // THIS node is open). A category matching only by its own name does
    // NOT need forcing open -- that match is already visible right on its
    // (possibly collapsed) row. This is deliberately independent of
    // categoryMatchesDirectly/forceShow above: those control what's shown
    // once a node IS open, not whether it needs to be forced open at all.
    // Only computed -- and only applied -- on the frame the search query
    // just changed. CategoryHasDescendantMatch does a full recursive walk
    // of this category's subtree, so evaluating it unconditionally on
    // every frame (even though the result was only ever consumed here)
    // stacked a second full-tree walk on top of the filtering walk above,
    // for every category, every frame, the whole time search was active --
    // expensive enough on a large tree to stall the overlay (dropped
    // keystrokes, unresponsive scrolling) for as long as the search box
    // had text in it, not just on the one frame the query changed. Gating
    // the computation itself (not just the SetNextItemOpen call) is what
    // actually avoids that cost.
    //
    // Deliberately NOT gated on searchActive here (only s_treeSearchQueryChanged) --
    // this needs to run on the way OUT of a search too, i.e. the frame the
    // query drops back below kMinTreeSearchLength (or is cleared). That
    // frame has searchActive == false, and if this block skipped it, every
    // category that had been force-opened while the search was active would
    // just stay open forever -- nothing else ever tells it to close. So
    // this always fires on a query change; categoryNeedsForceOpen is simply
    // false whenever there's no active search to justify it, folding the
    // category back down the same way it was forced open. Set explicitly
    // either way (not just when true) for the same reason: a category
    // forced open for a shorter/different query (e.g. "war" matching
    // "Warhorn" here) but no longer matching a narrower one (e.g.
    // "warrior") must be forced back shut on that same frame too. Once set
    // here, ImGui's own persisted open/closed state carries it forward on
    // later unchanged frames, same as always.
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

    // A duplicate-guid problem, or a merge whose candidates' settings
    // disagreed, wins the tint (red) over everything else -- both are
    // things that need a second look before an update should even be
    // trusted to know which effect is which underneath this category.
    // Failing that, a rework anywhere underneath wins over a new effect
    // (orange over green) -- a rework is the thing worth double-checking,
    // so a category with both should still stand out.
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

    // This category's own children (effects and nested subcategories) are
    // only ever visited further down, inside "if (categoryOpen)" -- so if
    // this node just closed (whether search forced it shut, or it was
    // already closed and stays that way) on the very frame the query
    // changed, nothing will visit its descendants this frame to reset
    // whatever force-open state an earlier, different query left on them.
    // Same reasoning as the early search-skip branch above; see
    // SilentlyCloseSubtree's own comment.
    if (!categoryOpen && s_treeSearchQueryChanged)
    {
        // TreeNode only auto-pushes its own ID scope onto the stack when it
        // opens (that's what lets its children compute IDs relative to it).
        // Since it's closed here, nothing pushed that scope -- so it has to
        // be entered manually to match the IDs the real render pass would
        // use if this category were open.
        ImGui::PushID(name.c_str());
        SilentlyCloseChildren(category);
        ImGui::PopID();
    }

    // Drop target for an effect dragged from elsewhere in this same sin
    // file (see EffectDragPayload/BeginDragDropSource below) -- attaches to
    // the TreeNode row itself, so a category accepts a drop whether it's
    // open or collapsed. Not offered on a pending-update overlay category
    // ("__vfxd_virtual") for the same reason Rename isn't: it isn't in the
    // real on-disk file yet, so there's nothing to re-find and move into
    // once the drop is applied.
    if (!categoryVirtual && ImGui::BeginDragDropTarget())
    {
        if (ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
        {
            // Payload bytes are just a type marker (see EffectDragPayload's
            // comment) -- the real source info lives in
            // installed_tree_edit.cpp's drag-payload static, read here via
            // GetEffectDragPayload(), which BeginEffectDrag keeps current
            // every frame the drag is held. Since only same-sin moves are
            // offered, guard against a payload somehow tagged with a
            // different sin (shouldn't happen -- BeginDragDropSource below only starts a
            // drag using this category tree's own sinName -- but cheap to
            // check rather than assume).
            //
            // Unlike before this category also doubled as a reorder-to-
            // end target, a drop here from the *same* category is now
            // allowed -- it's how an effect reaches the last position in
            // its own list (see this file's drag-and-drop header comment)
            // -- except when the dragged effect is already last, which
            // would erase-then-reinsert it in the exact same spot: a
            // no-op that would still trigger a pointless rewrite+.bak.
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
            // A dragged category can land in one of two places relative to
            // THIS row, and nowhere else -- reorder-only, see this file's
            // drag-and-drop header comment. If this category IS the
            // dragged one's current parent, dropping here means "append to
            // the end of my own children" (mirrors the effect target just
            // above). If this category instead shares that same parent
            // (i.e. it's a current sibling of the dragged one), dropping
            // here means "insert immediately above me." Anything else --
            // a category dropped onto an unrelated one under a different
            // parent entirely -- would be reparenting, which isn't offered
            // yet, so it's silently ignored rather than guessed at.
            if (GetCategoryDragSinName() == sinName && !GetCategoryDragPath().empty())
            {
                const std::vector<int>& dragPath = GetCategoryDragPath();
                std::vector<int> draggedParentPath(dragPath.begin(), dragPath.end() - 1);
                int              draggedIndex = dragPath.back();

                std::vector<int> myParentPath = pathSoFar;
                myParentPath.pop_back();

                if (pathSoFar == draggedParentPath)
                {
                    // Append case -- but only a real move if the dragged
                    // category isn't already the last child here, same
                    // no-op reasoning as the effect target's alreadyLast.
                    bool alreadyLast = category.contains("categories") && category["categories"].is_array() &&
                                       draggedIndex == static_cast<int>(category["categories"].size()) - 1;
                    if (!alreadyLast)
                        QueueCategoryMove(GetCategoryDragSinName(), dragPath, -1);
                }
                else if (myParentPath == draggedParentPath)
                {
                    // Sibling case -- insert immediately above this
                    // category, skipping the same two no-op positions the
                    // effect-row target skips: dropped on itself
                    // (draggedIndex == myIndex), or on the sibling right
                    // after it, which -- once the erase-shift is
                    // accounted for in ApplyPendingCategoryMove -- would
                    // land the dragged category right back where it
                    // started.
                    bool noOp = (draggedIndex == myIndex) || (draggedIndex == myIndex - 1);
                    if (!noOp)
                        QueueCategoryMove(GetCategoryDragSinName(), dragPath, myIndex);
                }
                // else: a different parent entirely -- would be
                // reparenting, not offered yet, so left alone.
            }
        }

        ImGui::EndDragDropTarget();
    }

    // Drag source -- any real category (not one only existing in a
    // pending-update overlay, see "__vfxd_virtual" above) can be picked up
    // and dropped to reorder it among its own current siblings (see this
    // file's drag-and-drop header comment for why reparenting into a
    // different parent isn't offered). Gated on the same "no other edit in
    // flight" rule as the effect drag source below, so a drag can't start
    // while an edit/rename elsewhere is mid-flight.
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginDragDropSource())
    {
        BeginCategoryDrag(sinName, pathSoFar);
        ImGui::Text("Move \"%s\"", name.c_str());
        ImGui::EndDragDropSource();
    }

    // Only offer to start a rename (or an effect edit, see below) when no
    // other edit of either kind is already in flight anywhere, and not on
    // a category that only exists in a pending-update overlay -- see
    // BuildDiffOverlayTree's "__vfxd_virtual" comment.
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginPopupContextItem("category_ctx"))
    {
        if (ImGui::MenuItem("Rename"))
            BeginCategoryEdit(sinName, pathSoFar, name);
        ImGui::EndPopup();
    }

    // "-" delete / "+" add-subcategory, rendered after the context menu
    // above so they don't steal "last item" from the TreeNode (which
    // BeginDragDropTarget/BeginPopupContextItem both attach to). Not
    // offered on a pending-update overlay category, same reasoning as
    // Rename/the drop target above. Delete stays grayed out for as long
    // as this category has any subcategory or effect inside it -- never
    // silently deletes content. "+" only appears once the category is
    // unfolded, since it's about adding something *inside* what you're
    // currently looking at.
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

    // The confirm prompt sits right below this row, not nested inside the
    // TreeNode's collapsible content -- the "-" button above is visible
    // whether this node is open or collapsed, so the prompt it opens stays
    // visible either way too, unlike isRenamingThis/isCreatingHere below.
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

                // Hidden by the tree search box: this category's own name/
                // description didn't match, and neither does this effect
                // (name/description/guids). Cancel any edit in flight on it
                // first -- same "nothing invisible stays running" reasoning
                // as the subtree-skip above -- since it won't be drawn this
                // frame at all, not even collapsed.
                if (searchActive && !categoryMatchesDirectly && !EffectMatchesSearch(effect, s_treeSearchQueryLower))
                {
                    bool isEditingThisHidden = IsEffectBeingEdited(sinName, pathSoFar, effIndex);
                    if (isEditingThisHidden)
                        CancelEdit();

                    bool isDeletingThisHidden = IsDeletingThisEffect(sinName, pathSoFar, effIndex);
                    if (isDeletingThisHidden)
                        CancelDeleteConfirm();

                    // Same reasoning as the category-level resets above --
                    // this effect isn't being visited at all this frame, so
                    // if the query just changed, its own stored open state
                    // needs resetting now or it reappears already expanded
                    // once it matches some future query.
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
                // Identity is (sinName, category path, index) -- NOT name.
                // Sibling effects can legitimately share a name (VfxDenoiser
                // itself doesn't require uniqueness), and matching by name
                // here would make every same-named sibling in this category
                // think it was the one being edited: all of them would pop
                // open showing the same editor content, and closing any one
                // of them (since only the actually-open one has real state)
                // would look like it cancelled the edit that was just
                // started on a different sibling.
                bool isEditingThis = IsEffectBeingEdited(sinName, pathSoFar, effIndex);
                bool isDeletingThisEffect = IsDeletingThisEffect(sinName, pathSoFar, effIndex);

                bool effIsDupe     = effect.value("__vfxd_dupe_guid", false);
                bool effIsNew      = effect.value("__vfxd_new", false);
                bool effIsRework   = effect.value("__vfxd_rework", false);
                bool effIsConflict = effect.value("__vfxd_conflict", false);

                // A settings conflict from a merge is just as much a
                // "review this before applying" situation as a duplicate
                // guid, so it gets the same red tint and the same top
                // priority.
                if (effIsDupe || effIsConflict)
                    ImGui::PushStyleColor(ImGuiCol_Text, kDuplicateColor);
                else if (effIsNew)
                    ImGui::PushStyleColor(ImGuiCol_Text, kNewColor);
                else if (effIsRework)
                    ImGui::PushStyleColor(ImGuiCol_Text, kReworkColor);

                // An effect's own node is only forced open if it matched
                // through content that's actually hidden until then -- its
                // description or a GUID. A name match alone doesn't need
                // it: that match is already visible right on this row.
                bool effectNeedsForceOpen = searchActive && EffectHiddenContentMatches(effect, s_treeSearchQueryLower);
                // Set explicitly either way, not just when true -- see the
                // category force-open comment above for why a node that no
                // longer needs opening has to be forced shut too. Also NOT
                // gated on searchActive, same reasoning as that comment --
                // this needs to fire on the way out of a search too, so an
                // effect forced open while searching folds back down once
                // the query is cleared/shortened, rather than staying open
                // forever.
                if (s_treeSearchQueryChanged)
                    ImGui::SetNextItemOpen(effectNeedsForceOpen, ImGuiCond_Always);
                bool nodeOpen = ImGui::TreeNode("effect", "%s%s", effName.c_str(), isEditingThis ? " (editing)" : "");

                if (effIsDupe || effIsNew || effIsRework || effIsConflict)
                    ImGui::PopStyleColor();

                // Drop target for an effect dragged onto this effect's own
                // row -- places the dragged effect immediately above this
                // one (see EffectMoveJob's destinationIndex comment).
                // Complements the category-row target above: together they
                // reach every position in the list, so this one never
                // needs to distinguish "above" from "below" within the
                // row. Not offered on an effect that only exists in a
                // pending-update overlay ("__vfxd_new") or on a
                // rework/merge preview node ("__vfxd_rework") -- neither
                // has a stable real on-disk position while only previewed
                // (BuildDiffOverlayTree can relocate a rework's survivor,
                // or shift a sibling's index by deleting a merged-away
                // effect out of the same array), so `pathSoFar`/`effIndex`
                // captured here could point at the wrong real effect once
                // actually applied.
                if (!effIsNew && !effIsRework && ImGui::BeginDragDropTarget())
                {
                    if (ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
                    {
                        // Same same-sin guard as the category-row target,
                        // plus two no-op cases specific to landing on a
                        // specific effect rather than appending: dropping
                        // an effect onto itself, and dropping it onto the
                        // effect immediately after it in the same
                        // category. The first is obvious. The second is
                        // subtler -- once the dragged effect is erased
                        // from originalIndex, the effect that used to sit
                        // at originalIndex + 1 shifts down to originalIndex,
                        // so "insert before it" would put the dragged
                        // effect right back where it started (see
                        // ApplyPendingMove's index adjustment). Both are
                        // skipped here rather than left for ApplyPendingMove
                        // to silently no-op, so neither triggers a
                        // pointless rewrite+.bak.
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
                    ImGui::EndDragDropTarget();
                }

                // Drag source -- any real effect, at its real on-disk
                // position, can be picked up and dropped onto a different
                // category's TreeNode row to move it there. Not offered on
                // an effect that only exists in a pending-update overlay
                // ("__vfxd_new") or a rework/merge preview node
                // ("__vfxd_rework") -- see the drag-drop-target comment
                // just above for why neither has a position worth trusting
                // yet. Gated on the same "no other edit in flight" rule as
                // the context-menu Edit just below, so a drag can't be
                // started while an edit/rename elsewhere is mid-flight
                // (see EffectMoveJob's comment for why moves are otherwise
                // independent of that machinery).
                if (!effIsNew && !effIsRework && !AnyEditInFlight() && ImGui::BeginDragDropSource())
                {
                    BeginEffectDrag(sinName, pathSoFar, effName, effIndex);
                    ImGui::SetDragDropPayload("VFXD_EFFECT", &kEffectDragMarker, sizeof(kEffectDragMarker));
                    ImGui::Text("Move \"%s\"", effName.c_str());
                    ImGui::EndDragDropSource();
                }

                // Only offer to start a new edit when none is already in
                // flight anywhere -- see EditState's comment in
                // installed_tree_edit.cpp for why --
                // and never on an effect that only exists in a pending-
                // update overlay ("__vfxd_new") or a rework/merge preview
                // node ("__vfxd_rework"): the former isn't in the real
                // file yet, and the latter's position in this overlay
                // copy is provisional (see the drag-drop-target comment
                // above) -- either way there's nothing at `pathSoFar`/
                // `effIndex` in the REAL file guaranteed to be this same
                // effect until the update is actually applied.
                if (!effIsNew && !effIsRework && !AnyEditInFlight() && ImGui::BeginPopupContextItem("effect_ctx"))
                {
                    if (ImGui::MenuItem("Edit"))
                        BeginEdit(sinName, pathSoFar, effIndex, effect);
                    ImGui::EndPopup();
                }

                // "-" delete, rendered after the context menu above for
                // the same "don't steal last item" reason as the
                // category's own -/+ buttons. Never grayed out for
                // emptiness (an effect has no "contents" to protect,
                // unlike a category) -- only temporarily disabled while
                // some other edit/delete/create/rename is in flight
                // elsewhere. Not offered on a pending-update overlay or
                // rework/merge preview effect, same reasoning as Edit
                // just above.
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

                // Same reasoning as the category's own confirm prompt:
                // this sits right below the row, which is visible whether
                // or not `nodeOpen` is true, so the prompt stays visible
                // either way too.
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
                                        "The merged effects had different settings below -- review before applying.");
                            }
                            else if (effIsConflict)
                            {
                                // A GUID this effect absorbed used to belong
                                // to another effect that's surviving under
                                // its own separate update instead of being
                                // deleted -- nothing is disappearing, but
                                // that other effect's settings disagreed
                                // with this one's, so it's still worth a
                                // second look.
                                ImGui::TextColored(kDuplicateColor,
                                    "This effect absorbed a GUID from another effect with different settings below -- review before applying.");
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
                            // Show what's there now (default color) and
                            // what a pending update would change it to
                            // (kReworkColor), stacked so both are visible
                            // at once without needing to apply first.
                            std::vector<std::string> newGuids;
                            if (effect.contains("__vfxd_new_guids") && effect["__vfxd_new_guids"].is_array())
                                for (const auto& g : effect["__vfxd_new_guids"])
                                    if (g.is_string())
                                        newGuids.push_back(g.get<std::string>());

                            RenderGuidDiff(guids, newGuids);
                        }
                        else
                        {
                            RenderGuidList("guids", guids);
                        }

                        if (effect.contains("behaviors") && effect["behaviors"].is_array())
                        {
                            ImGui::TextDisabled("Behaviors (owned by VfxDenoiser):");
                            for (const auto& behavior : effect["behaviors"])
                                RenderBehavior(behavior);
                        }

                        // Anything beyond the confirmed schema (name/
                        // description/guids/behaviors) is unexpected --
                        // surface it generically rather than drop it.
                        for (const auto& [key, value] : effect.items())
                        {
                            if (key == "name" || key == "description" || key == "guids" || key == "behaviors"
                                || key == "__vfxd_new" || key == "__vfxd_rework" || key == "__vfxd_new_guids"
                                || key == "__vfxd_hasnew" || key == "__vfxd_hasrework"
                                || key == "__vfxd_dupe_guid" || key == "__vfxd_hasdupe"
                                || key == "__vfxd_old_name" || key == "__vfxd_old_category"
                                || key == "__vfxd_merged_count" || key == "__vfxd_conflict")
                                continue;
                            RenderJsonValue(key, value);
                        }
                    }

                    ImGui::TreePop();
                }
                else if (isEditingThis)
                {
                    // Collapsing the node that's mid-edit cancels the edit,
                    // same reasoning as the category case above.
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
        // This node is collapsed -- nothing inside it (including its own
        // rename UI, the "add subcategory" prompt, or any effect editor
        // further down) is being drawn this frame. Cancel rather than let
        // an edit keep running invisibly until the user reopens the node.
        // PathHasPrefix with equal-length paths also covers "this category
        // is the one being renamed"/"...the one a subcategory is being
        // added to", so there's no separate isRenamingThis/isCreatingHere
        // check needed. DeleteConfirmState is NOT included here on
        // purpose -- see its own comment for why: the "-" button and the
        // confirm prompt it opens sit on this row itself, which stays
        // visible whether or not the node below it is collapsed, so
        // there's nothing for collapsing to hide.
        if (IsCategoryRenameUnderPath(sinName, pathSoFar))
            CancelCategoryEdit();
        if (IsEffectEditUnderPath(sinName, pathSoFar))
            CancelEdit();
        if (IsCategoryCreateUnderPath(sinName, pathSoFar))
            CancelCreateCategory();
    }

    pathSoFar.pop_back();
}

} // namespace

// Draws the "Installed Effects" section: one top-level TreeNode per
// installed sin file (Gluttony / Pride / Sloth, whichever are actually
// present), each expanding into that file's real category tree via
// RenderCategoryTree. Read-only browsing by default; right-clicking an
// effect offers "Edit" (see EditState/BeginEdit/RenderEffectEditor above).
// Independent of whether a GitHub update is available.
void RenderInstalledEffects(const std::string& denoiserAddonDir)
{
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    if (ImGui::Button("Refresh##installed_tree"))
        LoadInstalledEffectsTree(denoiserAddonDir);

    ImGui::TextDisabled("Drag an effect onto a category to move it there (or to the end of its own category), "
                         "or onto another effect to place it just above that one. Categories can be dragged the "
                         "same way to reorder them among their own siblings.");

    // Tree search box -- filters every installed sin file's tree at once by
    // name, category name, description, or GUID, case-insensitively. Just
    // recomputes the lowercased query used by RenderCategoryTree's matching
    // helpers; the actual filtering/expansion happens down there.
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

    // Below the minimum length, treat the query as empty -- no filtering,
    // no forced expansion, same as an empty search box.
    std::string newQueryLower = (typedLower.size() >= kMinTreeSearchLength) ? typedLower : std::string();

    // Only true on this one frame if the query is different from what it
    // was last frame -- see s_treeSearchQueryChanged's own comment for why
    // RenderCategoryTree cares about this distinction rather than just
    // "search box has text in it".
    s_treeSearchQueryChanged = (newQueryLower != s_treeSearchQueryLower);
    s_treeSearchQueryLower   = std::move(newQueryLower);

    if (!GetEditResultMessage().empty())
        ImGui::TextWrapped("%s", GetEditResultMessage().c_str());

    if (GetInstalledSins().empty())
    {
        ImGui::TextDisabled("No Visual Sins effect files found in VfxDenoiser's folder.");
        return;
    }

    // A Ready diff plan overlays pending-update coloring onto this same
    // tree via BuildDiffOverlayTree, rather than a separate list -- see
    // RenderSinDiffStatus in the top action row. Sins with no plan yet (or
    // an empty one) just render the plain on-disk tree, same as always.
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

        // Build up whichever overlays apply, in order: duplicate-guid
        // tagging first (a property of the file itself), then the
        // pending-update diff on top of that same copy -- both markers can
        // coexist on one node (e.g. a duplicated effect that also has a
        // pending rework), and RenderCategoryTree picks duplicate-red over
        // rework-orange/new-green when both are present. Only ever a copy;
        // the store's own copy is never touched by either pass.
        //
        // Cached in s_overlayCache rather than rebuilt every frame -- see
        // that struct's own comment for why (this used to be the direct
        // cause of a reported scrolling bug).
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

        // While a tree search is active, check up front whether this file
        // has any match at all -- lets the root row force itself open (so a
        // match isn't hidden behind an unexpanded file) and, further down,
        // lets an empty result say so rather than claim there are no
        // categories in a file that actually has plenty.
        bool searchActive   = !s_treeSearchQueryLower.empty();
        bool anyMatchInFile = false;
        if (searchActive && fileToRender->contains("categories") && (*fileToRender)["categories"].is_array())
            for (const auto& cat : (*fileToRender)["categories"])
                if (CategorySubtreeMatchesSearch(cat, s_treeSearchQueryLower))
                {
                    anyMatchInFile = true;
                    break;
                }

        // Set explicitly either way on a query change, not just when true
        // -- a file that matched a shorter/different query but no longer
        // has anything under a narrower one needs to be forced back shut,
        // same reasoning as the category/effect force-open comments. Also
        // NOT gated on searchActive, same reasoning as those -- this needs
        // to fire on the way out of a search too (anyMatchInFile is simply
        // false whenever there's no active search), so a root file node
        // that got force-opened while searching folds back down once the
        // query is cleared/shortened, instead of staying open forever.
        if (s_treeSearchQueryChanged)
            ImGui::SetNextItemOpen(anyMatchInFile, ImGuiCond_Always);

        bool rootOpen = ImGui::TreeNode("root", "%s (%s)", sin.sinName.c_str(), sin.fileName.c_str());

        // Same reasoning as RenderCategoryTree's own version of this fix --
        // the top-level categories under this root are only ever visited
        // inside the "if (rootOpen)" block below, so if this file's root
        // node just closed on the frame the query changed, nothing will
        // visit them to reset whatever force-open state an earlier,
        // different query left further down. "root" is TreeNode's str_id
        // here (not the displayed text), so that's what must be pushed to
        // match the ID scope it would have used had it opened.
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
            std::vector<int> path; // this sin file's top level -- empty path, same convention as installed_tree_edit's CreateCategoryState::parentPath

            // Drop target for a top-level category being reordered (see
            // RenderCategoryTree's category-row target for the nested
            // case) -- this root row is the "shared parent's own row" a
            // top-level category doesn't otherwise have anything to drop
            // onto, since there's no category node above it. Reorder-only,
            // same as RenderCategoryTree's target: only offered when the
            // dragged category is already top-level *in this same sin
            // file* -- a top-level category from a different sin file, or
            // a nested category from anywhere, would both be reparenting,
            // not offered yet (see this file's drag-and-drop header
            // comment).
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
            // Same "collapsing cancels" reasoning as RenderCategoryTree's
            // own collapsed branch -- the "+" button and prompt above only
            // exist inside this TreeNode, so collapsing it hides them.
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

    // Apply any edit that was saved during this frame's tree walk above --
    // deferred to here, after every category/effect array has finished
    // being iterated, so nothing is ever mutated mid-walk. All six are
    // called unconditionally.
    ApplyPendingEdit();
    ApplyPendingCategoryRename();
    ApplyPendingMove();
    ApplyPendingCategoryMove();
    ApplyPendingDelete();
    ApplyPendingCreateCategory();
}
