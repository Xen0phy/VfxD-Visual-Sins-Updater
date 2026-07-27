// installed_tree_overlay.cpp
//
// Diff/duplicate overlay tree builders. Extracted from addon.cpp
// -- a mechanical move, no behavior
// change. See installed_tree_overlay.h for what's exposed and why.
#include "core/tree/installed_tree_overlay.h"
#include <unordered_map>
#include <unordered_set>

namespace {

// guid -> owning effect, over the overlay copy being built. Same idea as
// merge.cpp's own OldIndex (see ApplyMergePlan there), built once up front
// -- O(effects) -- rather than a fresh linear scan per lookup, which is
// what this used to be (FindOverlayEffectLocation, a straight port of
// ApplyMergePlan's original per-lookup scan). At the effect counts this
// addon deals with, a fresh scan per rework only actually costs anything
// once a single update reworks a large fraction of the file at once, but
// since ApplyMergePlan itself got the same fix (see its own comment),
// there was no reason for the preview builder -- which does the same
// shape of work against the same size of tree -- to stay slow.
struct DiffGuidIndex
{
    std::unordered_map<std::string, nlohmann::ordered_json*> guidToEffect;
};

void IndexDiffCategory(nlohmann::ordered_json& category, DiffGuidIndex& idx)
{
    if (category.contains("effects") && category["effects"].is_array())
        for (auto& eff : category["effects"])
            if (eff.contains("guids") && eff["guids"].is_array())
                for (auto& g : eff["guids"])
                    if (g.is_string())
                        idx.guidToEffect[g.get<std::string>()] = &eff;

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
            IndexDiffCategory(sub, idx);
}

// Address-based removal, single pass -- same shape and same reasoning as
// merge.cpp's RemoveEffectsRecursive: every address in `toRemove` has to
// come from a still-fully-valid index (nothing resized yet), removed in
// one pass, or an earlier erase in the same array would shift a later
// element's address out from under a pointer this pass still means to
// match. Back-to-front, by index -- NOT forward begin()/erase(it) --
// since erasing index i shifts every index > i down into the slot i used
// to occupy; a forward pass re-checks toRemove against that reused
// address on its very next iteration and can match it again, cascading
// into deleting everything after the first removed element. Going
// back-to-front never touches an as-yet-unvisited (lower) index's
// address, so every check compares against that element's real, original
// address.
void RemoveDiffEffects(nlohmann::ordered_json& category, const std::unordered_set<const nlohmann::ordered_json*>& toRemove)
{
    if (category.contains("effects") && category["effects"].is_array())
    {
        auto& effects = category["effects"];
        for (size_t i = effects.size(); i-- > 0; )
            if (toRemove.count(&effects[i]))
                effects.erase(effects.begin() + i);
    }

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
            RemoveDiffEffects(sub, toRemove);
}

// Finds, or creates and appends (tagged "__vfxd_virtual" -- doesn't exist
// on disk yet), the category at `path` under `root`. Shared by a moving
// rework's destination and a plain insert's destination. Ancestor tint
// ("__vfxd_hasnew"/"__vfxd_hasrework"/"__vfxd_hasconflict") is NOT set
// here -- see BubbleDiffTags below for why that's a separate bottom-up
// pass instead of tagged inline during creation.
nlohmann::ordered_json* FindOrCreateDiffCategory(nlohmann::ordered_json& root, const std::vector<std::string>& path)
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
            newCat["name"]           = segment;
            newCat["categories"]     = nlohmann::ordered_json::array();
            newCat["effects"]        = nlohmann::ordered_json::array();
            newCat["__vfxd_virtual"] = true; // doesn't exist on disk yet
            (*cursor)["categories"].push_back(std::move(newCat));
            next = &(*cursor)["categories"].back();
        }
        cursor = next;
    }
    return cursor;
}

// Recomputes every category's "__vfxd_hasnew"/"__vfxd_hasrework"/
// "__vfxd_hasconflict" bubble-up flags bottom-up, from whatever per-effect
// markers are already set on its descendants. A separate pass rather than
// tagged inline while walking down to create/relocate a category (the
// original design, still visible in FindOrCreateDiffCategory's own doc
// comment above): a moved rework survivor or a merge's deleted losing
// candidates can change what's true about a category *after* the walk
// that would have tagged it, so inline tagging had to be trusted to run
// again for every path that touches a given category -- a single bottom-up
// pass after every move/merge/insert has already happened doesn't have
// that ordering dependency, since it looks at the actual final leaf
// markers rather than replaying "what happened while I walked past here."
// RenderCategoryTree reads these three flags to tint an ancestor category
// header, conflict taking priority over rework over new (see the
// color-block comment near kConflictColor).
void BubbleDiffTags(nlohmann::ordered_json& category)
{
    bool hasNew = false, hasRework = false, hasConflict = false;

    if (category.contains("effects") && category["effects"].is_array())
        for (auto& eff : category["effects"])
        {
            hasNew      |= eff.value("__vfxd_new", false);
            hasRework   |= eff.value("__vfxd_rework", false);
            hasConflict |= eff.value("__vfxd_conflict", false);
        }

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
        {
            BubbleDiffTags(sub);
            hasNew      |= sub.value("__vfxd_hasnew", false);
            hasRework   |= sub.value("__vfxd_hasrework", false);
            hasConflict |= sub.value("__vfxd_hasconflict", false);
        }

    if (hasNew)      category["__vfxd_hasnew"]      = true;
    if (hasRework)   category["__vfxd_hasrework"]   = true;
    if (hasConflict) category["__vfxd_hasconflict"] = true;
}

// Recursively marks every effect owning one of `dupeGuids` with a display-
// only "__vfxd_dupe_guid" marker, and bubbles a "__vfxd_hasdupe" marker up
// onto every ancestor category that contains one, tagging and bubbling in
// the same recursive walk (unlike BuildDiffOverlayTree's reworks/inserts,
// which tag leaf effects first and recompute ancestor tint in a separate
// bottom-up BubbleDiffTags pass afterward -- there's no relocation/removal
// happening here that could invalidate an inline bubble as it walks, so
// the simpler single-pass shape is fine). Flags a correctness problem
// already present in the installed file itself (see FindDuplicateGuids),
// not a pending update. Returns whether anything under `category` was
// tagged, so the caller can tag ancestors too.
bool TagDuplicateGuidEffects(nlohmann::ordered_json& category, const std::unordered_set<std::string>& dupeGuids)
{
    bool changed = false;

    if (category.contains("effects") && category["effects"].is_array())
    {
        for (auto& eff : category["effects"])
        {
            if (!eff.contains("guids") || !eff["guids"].is_array())
                continue;

            bool isDupe = false;
            for (const auto& g : eff["guids"])
                if (g.is_string() && dupeGuids.count(g.get<std::string>()))
                {
                    isDupe = true;
                    break;
                }

            if (isDupe)
            {
                eff["__vfxd_dupe_guid"] = true;
                changed = true;
            }
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
            if (TagDuplicateGuidEffects(sub, dupeGuids))
                changed = true;

    if (changed)
        category["__vfxd_hasdupe"] = true;

    return changed;
}

} // namespace

// Deep-copies `installed` and overlays `plan` onto it purely for display,
// so the installed-effects tree stays the single source of truth for what
// a pending update would do instead of a second, separate list. Mirrors
// ApplyMergePlan's own index-once / mutate-in-place / remove-once /
// reinsert-once phase ordering (same pointer-invalidation hazards apply to
// this throwaway copy as to the real oldFile), so the preview can never
// disagree with what Apply actually produces:
//   - every rework's survivor is found via `idx` and tagged in place --
//     "__vfxd_rework", its `__vfxd_new_guids` (guids themselves are left
//     alone here so the detail view can still show current-vs-after side
//     by side, same as before), "__vfxd_old_name" only when it's actually
//     changing, and "__vfxd_merged_count"/"__vfxd_conflict" for a merge
//   - every merged-away candidate is found via `idx` and marked for
//     removal, since post-merge there's only ever one node left
//   - if the update also moves the survivor to a new category, it's
//     marked for removal from its current spot too, and queued to be
//     re-inserted at the new path afterward (with "__vfxd_old_category"
//     recorded on it so the detail view can say "moved from X")
//   - a single removal pass runs once every rework has been examined,
//     then every queued survivor is re-inserted at its destination
//     (creating categories as needed, tagged "__vfxd_virtual")
//   - inserts are appended under their target category path the same way,
//     tagged "__vfxd_new"
//   - every category's tint flag is then recomputed bottom-up (see
//     BubbleDiffTags)
// "__vfxd_virtual" additionally tells RenderCategoryTree to suppress the
// right-click "Rename" menu on that category, and "__vfxd_new"/
// "__vfxd_rework" both suppress Edit/Delete/drag on that effect -- neither
// has a stable real on-disk position while only previewed (a rework's
// position in this copy is provisional too, same as a brand-new insert's,
// now that it can be physically relocated/merged here), so acting on them
// now would target the wrong thing once actually applied. Nothing here is
// ever written back to disk -- these marker fields exist only in this
// in-memory copy. RenderCategoryTree's existing "unexpected field"
// fallback for effects explicitly skips them so a marker can never leak
// into the visible field list (see the skip-list there).
nlohmann::ordered_json BuildDiffOverlayTree(const nlohmann::ordered_json& installed, const MergePlan& plan)
{
    nlohmann::ordered_json overlay = installed;
    if (!overlay.contains("categories") || !overlay["categories"].is_array())
        overlay["categories"] = nlohmann::ordered_json::array();

    DiffGuidIndex idx;
    for (auto& cat : overlay["categories"])
        IndexDiffCategory(cat, idx);

    std::unordered_set<const nlohmann::ordered_json*> toRemove;
    std::vector<std::pair<nlohmann::ordered_json, std::vector<std::string>>> pendingMoves; // (post-tag snapshot, target category path)

    for (const auto& rw : plan.reworks)
    {
        nlohmann::ordered_json* survivor = nullptr;
        for (const auto& g : rw.oldGuids)
        {
            auto it = idx.guidToEffect.find(g);
            if (it != idx.guidToEffect.end()) { survivor = it->second; break; }
        }
        if (!survivor)
            continue; // shouldn't happen -- overlay is freshly built from the same installed tree the plan was resolved against

        // Shown as its final, upstream name directly (same as how a
        // brand-new insert already shows newFile's own name verbatim) --
        // "__vfxd_old_name" is only added when it's actually changing, so
        // the detail view can additionally say "renamed from X" rather
        // than implying every rework renames something. Guids themselves
        // are deliberately left alone here (only "__vfxd_new_guids" is
        // set) so the detail view's "Current GUIDs" / "GUIDs after
        // update" split still shows the real current list.
        (*survivor)["name"]             = rw.newName;
        (*survivor)["__vfxd_rework"]    = true;
        (*survivor)["__vfxd_new_guids"] = rw.newGuids;

        if (rw.oldName != rw.newName)
            (*survivor)["__vfxd_old_name"] = rw.oldName;

        // Checked unconditionally, NOT nested inside the mergedAwayGuids
        // branch below: behaviorsConflict reflects a disagreement across
        // this rework's ORIGINAL matched candidates (see BuildMergedRework
        // in merge.cpp) and is computed once, before
        // StripConflictingMergedAwayGuids ever runs. That later pass can
        // legitimately empty out mergedAwayGuids -- e.g. a merged-away
        // candidate that turns out to have its own separate rework
        // elsewhere in this same plan and so must survive rather than be
        // deleted -- without the underlying settings disagreement it
        // already found becoming any less real. Gating this tag behind
        // "still has something left to delete" would silently hide a
        // genuine conflict merely because nothing physically disappears.
        if (rw.behaviorsConflict)
            (*survivor)["__vfxd_conflict"] = true;

        if (!rw.mergedAwayGuids.empty())
        {
            (*survivor)["__vfxd_merged_count"] = static_cast<int>(rw.mergedAwayGuids.size());

            for (const auto& g : rw.mergedAwayGuids)
            {
                auto it = idx.guidToEffect.find(g);
                if (it != idx.guidToEffect.end() && it->second != survivor)
                    toRemove.insert(it->second);
            }
        }

        if (!rw.newCategoryPath.empty() && rw.newCategoryPath != rw.oldCategoryPath)
        {
            (*survivor)["__vfxd_old_category"] = JoinPath(rw.oldCategoryPath);
            toRemove.insert(survivor);
            pendingMoves.emplace_back(*survivor, rw.newCategoryPath);
        }
    }

    for (auto& cat : overlay["categories"])
        RemoveDiffEffects(cat, toRemove);

    for (auto& [snapshot, targetPath] : pendingMoves)
    {
        nlohmann::ordered_json* cursor = FindOrCreateDiffCategory(overlay, targetPath);
        if (!cursor->contains("effects") || !(*cursor)["effects"].is_array())
            (*cursor)["effects"] = nlohmann::ordered_json::array();
        (*cursor)["effects"].push_back(std::move(snapshot));
    }

    for (const auto& ins : plan.inserts)
    {
        nlohmann::ordered_json* cursor = FindOrCreateDiffCategory(overlay, ins.categoryPath);
        if (!cursor->contains("effects") || !(*cursor)["effects"].is_array())
            (*cursor)["effects"] = nlohmann::ordered_json::array();

        nlohmann::ordered_json newEffect = ins.effect;
        newEffect["__vfxd_new"] = true;
        (*cursor)["effects"].push_back(std::move(newEffect));
    }

    for (auto& cat : overlay["categories"])
        BubbleDiffTags(cat);

    return overlay;
}

// Deep-copies `installed` and tags it with duplicate-guid markers for
// display, same reasoning as BuildDiffOverlayTree below: these markers must
// never reach the store's own copy, since that's what ApplyPendingEdit/
// SaveInstalledSinFile eventually serialize back to disk verbatim. Called
// independently of any update/diff overlay -- this is about the file as it
// sits on disk right now, not about a pending change.
nlohmann::ordered_json BuildDuplicateOverlayTree(const nlohmann::ordered_json& installed, const std::vector<std::string>& dupeGuids)
{
    nlohmann::ordered_json overlay = installed;
    std::unordered_set<std::string> dset(dupeGuids.begin(), dupeGuids.end());

    if (overlay.contains("categories") && overlay["categories"].is_array())
        for (auto& cat : overlay["categories"])
            TagDuplicateGuidEffects(cat, dset);

    return overlay;
}
