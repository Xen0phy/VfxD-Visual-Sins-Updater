//################################################################################
// installed_tree_overlay.cpp
//--------------------------------------------------------------------------------
// See installed_tree_overlay.h for the module contract. Diff/duplicate
// overlay tree builders, extracted from addon.cpp -- a mechanical move, no
// behavior change.
//--------------------------------------------------------------------------------

#include "installed_tree_overlay.h"

#include <unordered_map>
#include <unordered_set>

namespace {

//********************************************************************************
// DiffGuidIndex
//--------------------------------------------------------------------------------
// guidToEffect         guid -> owning effect, over the overlay copy being built
// guidToCategoryPath   guid -> the category path that effect currently
//                      lives under. Populated alongside guidToEffect
//                      purely so BuildEffectDbOverlayTree can sync an
//                      already-JSON-known guid's REAL placement back into
//                      effect_db's own category_path (see its use there)
//                      -- otherwise only guids placed via a drag ever get
//                      a category_path in the db at all, even though
//                      most captured guids were already sitting somewhere
//                      real in a sin file before "for science" ever saw
//                      them.
//--------------------------------------------------------------------------------
// Built once up front -- O(effects) -- rather than a fresh linear scan per
// lookup, same idea as merge.cpp's own OldIndex (see ApplyMergePlan
// there). Replaces what used to be a straight per-lookup scan
// (FindOverlayEffectLocation); once ApplyMergePlan itself got the same
// fix, there was no reason for this preview builder, doing the same shape
// of work against the same size of tree, to stay slow.
//--------------------------------------------------------------------------------
struct DiffGuidIndex
{
    std::unordered_map<std::string, nlohmann::ordered_json*> guidToEffect;
    std::unordered_map<std::string, std::vector<std::string>> guidToCategoryPath;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IndexDiffCategory
//--------------------------------------------------------------------------------
// Recursively populates `idx` from every effect under `category`,
// including `category`'s own name in the tracked path -- same
// push-before-recurse/pop-after shape as pathSoFar elsewhere in this
// codebase (e.g. RenderCategoryTree), so a top-level call already
// includes that category's own name at path[0], not just its children's.
//--------------------------------------------------------------------------------
void IndexDiffCategory(nlohmann::ordered_json& category, DiffGuidIndex& idx, std::vector<std::string>& pathSoFar)
{
    pathSoFar.push_back(category.value("name", std::string()));

    if (category.contains("effects") && category["effects"].is_array())
        for (auto& eff : category["effects"])
            if (eff.contains("guids") && eff["guids"].is_array())
                for (auto& g : eff["guids"])
                    if (g.is_string())
                    {
                        idx.guidToEffect[g.get<std::string>()]       = &eff;
                        idx.guidToCategoryPath[g.get<std::string>()] = pathSoFar;
                    }

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
            IndexDiffCategory(sub, idx, pathSoFar);

    pathSoFar.pop_back();
}

//_ Convenience overload for the (more common) case where a caller
// doesn't need to seed or reuse the path vector itself -- both existing
// call sites use this one unchanged.
void IndexDiffCategory(nlohmann::ordered_json& category, DiffGuidIndex& idx)
{
    std::vector<std::string> path;
    IndexDiffCategory(category, idx, path);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RemoveDiffEffects
//--------------------------------------------------------------------------------
// Address-based removal, single pass -- same shape and reasoning as
// merge.cpp's RemoveEffectsRecursive: every address in `toRemove` must
// come from a still-fully-valid index, so removal goes back-to-front by
// index rather than a forward begin()/erase(it) walk. Erasing index i
// shifts every index > i down into the slot i used to occupy; a forward
// pass would re-check toRemove against that reused address on its very
// next iteration, cascading into deleting everything after the first
// removed element. Back-to-front never touches an as-yet-unvisited
// index's address, so every check compares against the real original one.
//--------------------------------------------------------------------------------
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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateDiffCategory
//--------------------------------------------------------------------------------
// Finds, or creates and appends (tagged "__vfxd_virtual"), the category at
// `path` under `root`. Shared by a moving rework's destination and a plain
// insert's destination. Ancestor tint ("__vfxd_hasnew"/"__vfxd_hasrework"/
// "__vfxd_hasconflict") is NOT set here -- see BubbleDiffTags below for why
// that's a separate bottom-up pass instead of tagged inline during creation.
//--------------------------------------------------------------------------------
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
            newCat["__vfxd_virtual"] = true;   //. doesn't exist on disk yet
            (*cursor)["categories"].push_back(std::move(newCat));
            next = &(*cursor)["categories"].back();
        }
        cursor = next;
    }
    return cursor;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BubbleDiffTags
//--------------------------------------------------------------------------------
// Recomputes every category's "__vfxd_hasnew"/"__vfxd_hasrework"/
// "__vfxd_hasconflict" flags bottom-up, from whatever per-effect markers
// are already set on its descendants. A separate pass rather than tagged
// inline while walking down to create/relocate a category: a moved
// survivor or a merge's deleted candidates can change what's true about a
// category *after* the walk that would have tagged it, so a single
// bottom-up pass over the final leaf markers avoids that ordering
// dependency. RenderCategoryTree reads these three flags to tint an
// ancestor category header, conflict taking priority over rework over new.
//--------------------------------------------------------------------------------
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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// TagDuplicateGuidEffects
//--------------------------------------------------------------------------------
// Recursively marks every effect owning one of `dupeGuids` with
// "__vfxd_dupe_guid", bubbling "__vfxd_hasdupe" onto every ancestor
// category that contains one, tagging and bubbling in the same walk --
// unlike BuildDiffOverlayTree's separate bottom-up BubbleDiffTags pass,
// there's no relocation/removal here that could invalidate an inline
// bubble, so the simpler single-pass shape is fine. Flags a correctness
// problem already present in the installed file (see FindDuplicateGuids),
// not a pending update. Returns whether anything under `category` was
// tagged, so the caller can tag ancestors too.
//--------------------------------------------------------------------------------
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

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildDiffOverlayTree
//--------------------------------------------------------------------------------
// Mirrors ApplyMergePlan's own index-once / mutate-in-place / remove-once /
// reinsert-once phase ordering (same pointer-invalidation hazards apply to
// this throwaway copy as to the real oldFile), so the preview can never
// disagree with what Apply actually produces:
//   - every rework's survivor is found via `idx` and tagged in place --
//     "__vfxd_rework"/"__vfxd_new_guids"/"__vfxd_old_name" (only if the
//     name changed)/"__vfxd_merged_count"/"__vfxd_conflict" for a merge
//   - every merged-away candidate is found via `idx` and marked for
//     removal, since post-merge there's only ever one node left
//   - a survivor that also moves category is marked for removal too and
//     queued to be re-inserted at its new path afterward, tagged
//     "__vfxd_old_category" so the detail view can say "moved from X"
//   - a single removal pass runs once every rework has been examined,
//     then every queued survivor is re-inserted at its destination
//     (creating categories as needed, tagged "__vfxd_virtual")
//   - inserts are appended under their target category path the same way,
//     tagged "__vfxd_new"
//   - every category's tint flag is then recomputed bottom-up (see
//     BubbleDiffTags)
// "__vfxd_virtual" additionally tells RenderCategoryTree to suppress the
// right-click "Rename" menu, and "__vfxd_new"/"__vfxd_rework" suppress
// Edit/Delete/drag on that effect -- neither has a stable real on-disk
// position while only previewed, so acting on them now would target the
// wrong thing once actually applied. RenderCategoryTree's "unexpected
// field" fallback for effects explicitly skips every "__vfxd_*" marker so
// none of them can leak into the visible field list.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildDiffOverlayTree(const nlohmann::ordered_json& installed, const MergePlan& plan)
{
    nlohmann::ordered_json overlay = installed;
    if (!overlay.contains("categories") || !overlay["categories"].is_array())
        overlay["categories"] = nlohmann::ordered_json::array();

    DiffGuidIndex idx;
    for (auto& cat : overlay["categories"])
        IndexDiffCategory(cat, idx);

    std::unordered_set<const nlohmann::ordered_json*> toRemove;
    std::vector<std::pair<nlohmann::ordered_json, std::vector<std::string>>> pendingMoves;   //. snapshot + target path

    for (const auto& rw : plan.reworks)
    {
        nlohmann::ordered_json* survivor = nullptr;
        for (const auto& g : rw.oldGuids)
        {
            auto it = idx.guidToEffect.find(g);
            if (it != idx.guidToEffect.end()) { survivor = it->second; break; }
        }
        //_ Shouldn't happen -- the overlay is freshly built from the same
        // installed tree the plan was resolved against.
        if (!survivor)
            continue;

        //_ Shown as the final upstream name directly; "__vfxd_old_name" is
        // only set below when it's actually changing. Guids are left alone
        // here so "Current GUIDs" / "GUIDs after update" can still compare.
        (*survivor)["name"]             = rw.newName;
        (*survivor)["__vfxd_rework"]    = true;
        (*survivor)["__vfxd_new_guids"] = rw.newGuids;

        if (rw.oldName != rw.newName)
            (*survivor)["__vfxd_old_name"] = rw.oldName;

        //_ Checked unconditionally, not nested under mergedAwayGuids: this
        // is the ORIGINAL matched-candidate disagreement, still real even
        // if StripConflictingMergedAwayGuids later empties that out.
        if (rw.behaviorsConflict)
        {
            (*survivor)["__vfxd_conflict"] = true;

            //_ One entry per other matched candidate's name/category/
            // behaviors, so the tree view can show what actually
            // disagreed, not just that something did.
            nlohmann::ordered_json sources = nlohmann::ordered_json::array();
            for (const auto& c : rw.otherCandidates)
            {
                nlohmann::ordered_json src;
                src["name"]      = c.name;
                src["category"]  = JoinPath(c.categoryPath);
                src["behaviors"] = c.behaviors;
                sources.push_back(std::move(src));
            }
            (*survivor)["__vfxd_conflict_sources"] = std::move(sources);
        }

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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildDuplicateOverlayTree
//--------------------------------------------------------------------------------
// Called independently of any update/diff overlay -- this is about the
// file as it sits on disk right now, not about a pending change.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildDuplicateOverlayTree(const nlohmann::ordered_json& installed, const std::vector<std::string>& dupeGuids)
{
    nlohmann::ordered_json overlay = installed;
    std::unordered_set<std::string> dset(dupeGuids.begin(), dupeGuids.end());

    if (overlay.contains("categories") && overlay["categories"].is_array())
        for (auto& cat : overlay["categories"])
            TagDuplicateGuidEffects(cat, dset);

    return overlay;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildOccurrencesJson
//--------------------------------------------------------------------------------
// EffectDb_GetOccurrences(guid), reshaped into the flat JSON array
// RenderEffectDbDetail groups at render time. Shared by both branches of
// BuildEffectDbOverlayTree below -- a db-only guid and an already-
// JSON-backed guid that also has capture data both need exactly this.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildOccurrencesJson(const std::string& guid_b64)
{
    nlohmann::ordered_json occurrences = nlohmann::ordered_json::array();
    for (const auto& occ : EffectDb_GetOccurrences(guid_b64))
    {
        nlohmann::ordered_json o;
        o["duration"]       = occ.duration;
        o["a4"]             = occ.a4;
        o["a6"]             = occ.a6;
        o["self_mask"]      = static_cast<int>(occ.self_mask);
        o["profession"]     = static_cast<int>(static_cast<unsigned char>(occ.profession));
        o["race"]           = static_cast<int>(static_cast<unsigned char>(occ.race));
        o["specialization"] = occ.specialization;
        occurrences.push_back(std::move(o));
    }
    return occurrences;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildGroupsJson
//--------------------------------------------------------------------------------
// EffectDb_GetGroupsStarted/EffectDb_GetGroupsMemberOf(guid), reshaped
// into the flat JSON RenderEffectDbDetail reads at render time -- same
// "bake it into the cache once per rebuild, not once per frame" shape as
// BuildOccurrencesJson right above. Two arrays under one object so the
// tree's render side can tell "guids this one swept up when it opened a
// group" apart from "groups this one got swept into" without a second
// lookup.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildGroupsJson(const std::string& guid_b64)
{
    nlohmann::ordered_json groups;

    nlohmann::ordered_json started = nlohmann::ordered_json::array();
    for (const auto& inst : EffectDb_GetGroupsStarted(guid_b64))
    {
        nlohmann::ordered_json s;
        s["duration"] = inst.duration;
        s["a4"]       = inst.a4;
        s["members"]  = inst.memberGuids;
        started.push_back(std::move(s));
    }
    groups["started"] = std::move(started);

    nlohmann::ordered_json memberOf = nlohmann::ordered_json::array();
    for (const auto& m : EffectDb_GetGroupsMemberOf(guid_b64))
    {
        nlohmann::ordered_json mo;
        mo["starter_guid_b64"] = m.starterGuid_b64;
        mo["duration"]         = m.duration;
        mo["a4"]               = m.a4;
        memberOf.push_back(std::move(mo));
    }
    groups["member_of"] = std::move(memberOf);

    return groups;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildEffectDbOverlayTree
//--------------------------------------------------------------------------------
// See installed_tree_overlay.h. Reuses IndexDiffCategory (to find/skip
// guids that are already real JSON entries) and FindOrCreateDiffCategory
// (to place a genuinely db-only guid at its categoryPath, materializing
// categories as needed, tagged "__vfxd_virtual" the same as a pending
// update's brand-new category) -- both already file-local above, genuine
// reuse rather than a third copy of the same path-walk.
//
// Two distinct things happen here, not one:
//  - A guid with NO existing JSON entry gets a full synthetic
//    "__vfxd_db_only" node, same as before.
//  - A guid that's ALREADY a real JSON entry does NOT get skipped --
//    FeedEffectDb (live_log.cpp) records capture data for every
//    self-cast effect regardless of whether it's already curated into an
//    installed sin, so most of what "for science" actually captures
//    during ordinary play is data about already-known effects, not novel
//    ones. That data gets attached onto the EXISTING node under
//    "__vfxd_db_by_guid" (no "__vfxd_db_only" tag, so it keeps its
//    normal name/color/edit/drag/delete behavior entirely) -- an
//    additional expandable detail section, not a takeover of the node.
//
// "__vfxd_db_by_guid" is an object keyed by guid, not a flat field on
// the effect, because guidToEffect maps every guid of a multi-guid
// (merged) effect to the SAME json object -- a flat field would silently
// overwrite one guid's captured data with another's the moment more than
// one of an effect's guids has separately been captured. A synthetic
// db-only node's object always has exactly one key (it only ever has one
// guid), but sharing the shape means RenderEffectDbDetail only needs one
// code path for both cases.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildEffectDbOverlayTree(const nlohmann::ordered_json& installed, const std::vector<EffectDbEffect>& dbEffects,
                                                 size_t* outAddedCount)
{
    nlohmann::ordered_json overlay = installed;
    if (!overlay.contains("categories") || !overlay["categories"].is_array())
        overlay["categories"] = nlohmann::ordered_json::array();

    DiffGuidIndex idx;
    for (auto& cat : overlay["categories"])
        IndexDiffCategory(cat, idx);

    static const std::vector<std::string> kUnplacedBucket = { "Unrecognized (for science)" };

    size_t added = 0;
    for (const auto& dbEff : dbEffects)
    {
        nlohmann::ordered_json detail;
        detail["block_group"]  = dbEff.blockGroup;
        detail["block_member"] = dbEff.blockMember;
        detail["type"]         = dbEff.type;
        detail["occurrences"]  = BuildOccurrencesJson(dbEff.guid_b64);
        detail["groups"]       = BuildGroupsJson(dbEff.guid_b64);

        auto existingIt = idx.guidToEffect.find(dbEff.guid_b64);
        if (existingIt != idx.guidToEffect.end())
        {
            //_ Enrich the real node in place (not skipped, not tagged
            // "__vfxd_db_only") -- keyed by guid under "__vfxd_db_by_guid",
            // not a flat field, since a merged effect's guids share one object.
            nlohmann::ordered_json& existing = *existingIt->second;
            if (!existing.contains("__vfxd_db_by_guid") || !existing["__vfxd_db_by_guid"].is_object())
                existing["__vfxd_db_by_guid"] = nlohmann::ordered_json::object();
            existing["__vfxd_db_by_guid"][dbEff.guid_b64] = std::move(detail);

            //_ A guid that was JSON-known before capture never gets
            // EffectDb_SetCategoryPath called (that's drag-only, for db-only
            // guids) -- sync it here from `idx`; a no-op most rebuilds.
            auto pathIt = idx.guidToCategoryPath.find(dbEff.guid_b64);
            if (pathIt != idx.guidToCategoryPath.end() && pathIt->second != dbEff.categoryPath)
                EffectDb_SetCategoryPath(dbEff.guid_b64, pathIt->second);

            continue;
        }

        const std::vector<std::string>& path = dbEff.categoryPath.empty() ? kUnplacedBucket : dbEff.categoryPath;
        nlohmann::ordered_json* cursor = FindOrCreateDiffCategory(overlay, path);

        if (!cursor->contains("effects") || !(*cursor)["effects"].is_array())
            (*cursor)["effects"] = nlohmann::ordered_json::array();

        nlohmann::ordered_json newEffect;
        //_ Falls back to the guid itself when name is "" (an unnamed
        // type 1/11 marker row, or simply never renamed) -- an empty
        // tree label would be worse than a guid-shaped one.
        newEffect["name"]           = dbEff.name.empty() ? dbEff.guid_b64 : dbEff.name;
        newEffect["guids"]          = nlohmann::ordered_json::array({ dbEff.guid_b64 });
        newEffect["__vfxd_db_only"] = true;

        //_ Same by-guid shape as the enrichment branch above -- always
        // exactly one key here (a synthetic node has only one guid), but
        // it lets RenderEffectDbDetail use one loop for both cases.
        nlohmann::ordered_json byGuid = nlohmann::ordered_json::object();
        byGuid[dbEff.guid_b64] = std::move(detail);
        newEffect["__vfxd_db_by_guid"] = std::move(byGuid);

        (*cursor)["effects"].push_back(std::move(newEffect));
        ++added;
    }

    if (outAddedCount) *outAddedCount = added;
    return overlay;
}