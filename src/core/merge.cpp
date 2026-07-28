//################################################################################
// merge.cpp
//--------------------------------------------------------------------------------
// ResolveMergePlan()   builds a MergePlan from oldFile/newFile (see merge.h)
// ApplyMergePlan()     applies a resolved MergePlan to oldFile in place
// FindDuplicateGuids() guid-uniqueness check for a single file
//--------------------------------------------------------------------------------
// Implements the guid-first/name-fallback matching algorithm described in
// merge.h. An OldIndex (guid/name lookup over oldFile) is built once per
// call and consumed by a set of small helpers -- FindAllByGuid, GuidDiff,
// BuildRework/BuildMergedRework -- rather than one large function, so each
// piece of the decision table (skip/add-only/replace, 1a/1b/1c/2a/2b/2c)
// can be tested and read in isolation. ApplyMergePlan mutates in four
// strict phases (field updates, then removals, then relocations, then
// inserts) to keep every pointer captured from its own OldIndex valid
// until it's no longer needed -- see its own comment for why the order
// matters.
//--------------------------------------------------------------------------------

#include "core/merge.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

using json = nlohmann::ordered_json;

namespace {

//********************************************************************************
// OldIndex
//--------------------------------------------------------------------------------
// guidToEffect   guid -> owning old effect; last-indexed wins if the
//                guid-uniqueness premise below is ever violated
// guidToPath     guid -> category path (root -> immediate parent) the
//                owning effect lives under; used to fill oldCategoryPath
// effectsByName  name -> every old effect sharing it (multimap: GW2 reuses
//                display names across distinct effects, so a single
//                pointer per name silently picked one arbitrary match)
//--------------------------------------------------------------------------------
// A flat, built-once snapshot of oldFile, built fresh before each of
// ResolveMergePlan/ApplyMergePlan's own walks and never mutated while in
// use -- see each caller for why that matters there. Leans on guids never
// repeating within a file (confirmed for how ArenaNet ships these, not
// enforced against a hand-edited copy -- see FindDuplicateGuids); a
// violation wouldn't crash anything, just risk resolving against the
// wrong same-guid effect.
//--------------------------------------------------------------------------------
struct OldIndex
{
    std::unordered_map<std::string, json*> guidToEffect;
    std::unordered_map<std::string, std::vector<std::string>> guidToPath;
    std::unordered_multimap<std::string, json*> effectsByName;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IndexCategory
//--------------------------------------------------------------------------------
// `pathSoFar` already includes `category`'s own name (same convention as
// ResolvePlan's own pathSoFar) -- the caller pushes it on before recursing,
// mirroring exactly how ResolvePlan builds newFile's path on the way down.
//--------------------------------------------------------------------------------
void IndexCategory(json& category, const std::vector<std::string>& pathSoFar, OldIndex& idx)
{
    if (category.contains("effects") && category["effects"].is_array())
    {
        for (auto& effect : category["effects"])
        {
            if (effect.contains("name") && effect["name"].is_string())
                idx.effectsByName.emplace(effect["name"].get<std::string>(), &effect);

            if (effect.contains("guids") && effect["guids"].is_array())
                for (auto& g : effect["guids"])
                    if (g.is_string())
                    {
                        idx.guidToEffect[g.get<std::string>()] = &effect;
                        idx.guidToPath[g.get<std::string>()]   = pathSoFar;
                    }
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
    {
        for (auto& sub : category["categories"])
        {
            if (!sub.contains("name") || !sub["name"].is_string())
                continue; //. no name, skip
            std::vector<std::string> subPath = pathSoFar;
            subPath.push_back(sub["name"].get<std::string>());
            IndexCategory(sub, subPath, idx);
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// HasAnyGuid
//--------------------------------------------------------------------------------
// True if `effect` has at least one string guid. A guid-less effect can
// never be tracked reliably across releases -- see the case-0 skip in
// ResolvePlan below.
//--------------------------------------------------------------------------------
bool HasAnyGuid(const json& effect)
{
    if (!effect.contains("guids") || !effect["guids"].is_array())
        return false;
    for (const auto& g : effect["guids"])
        if (g.is_string())
            return true;
    return false;
}

std::vector<std::string> ExtractGuids(const json& effect)
{
    std::vector<std::string> out;
    if (effect.contains("guids") && effect["guids"].is_array())
        for (const auto& g : effect["guids"])
            if (g.is_string())
                out.push_back(g.get<std::string>());
    return out;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindAllByGuid
//--------------------------------------------------------------------------------
// Every distinct old effect owning at least one of `newEffect`'s guids
// (each listed once), or empty if none are claimed in oldFile. Checks
// every guid rather than stopping at the first hit: a new effect's guid
// list can straddle more than one old effect (e.g. an upstream merge of
// two effects into one) -- stopping early would silently pick one
// candidate and miss the other. See ResolvePlan for how the full set
// returned here resolves into 1a/1b/1c.
//--------------------------------------------------------------------------------
std::vector<json*> FindAllByGuid(const json& newEffect, const OldIndex& idx)
{
    std::vector<json*> matches;
    if (!newEffect.contains("guids") || !newEffect["guids"].is_array())
        return matches;

    for (const auto& g : newEffect["guids"])
    {
        if (!g.is_string())
            continue;
        auto it = idx.guidToEffect.find(g.get<std::string>());
        if (it == idx.guidToEffect.end())
            continue;

        bool alreadyFound = false;
        for (json* m : matches)
            if (m == it->second) { alreadyFound = true; break; }
        if (!alreadyFound)
            matches.push_back(it->second);
    }
    return matches;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GuidDiff
//--------------------------------------------------------------------------------
// Diffs a matched effect's old guid list `o` against its new list `n`:
// returns guids in `n` but not `o` (de-duplicated, `n`'s order), and
// reports via `outHasRemoved` whether `o` has anything `n` doesn't.
// Together these decide skip/add-only/replace -- see BuildRework -- raw
// counts alone can't, since the same count can mean identical, disjoint,
// or partially-overlapping sets.
//--------------------------------------------------------------------------------
std::vector<std::string> GuidDiff(const std::vector<std::string>& o,
                                   const std::vector<std::string>& n,
                                   bool& outHasRemoved)
{
    std::unordered_set<std::string> oSet(o.begin(), o.end());
    std::unordered_set<std::string> nSet(n.begin(), n.end());

    std::vector<std::string> added;
    std::unordered_set<std::string> addedSeen;
    for (const auto& g : n)
        if (!oSet.count(g) && addedSeen.insert(g).second)
            added.push_back(g);

    outHasRemoved = false;
    for (const auto& g : o)
        if (!nSet.count(g)) { outHasRemoved = true; break; }

    return added;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildRework
//--------------------------------------------------------------------------------
// Decides skip/add-only/replace for a matched old/new effect pair (guid or
// unambiguous name match) by diffing this effect's own guid list -- never
// a whole-file set, which would let one already-known guid mask other,
// genuinely new ones on the same effect:
//   added empty                      -> skip
//   added, nothing removed           -> add-only, old guids kept
//   added and removed, counts differ -> add-only (too ambiguous to drop)
//   added and removed, counts match  -> replace (clean upstream renumber)
// Returns false (no outRework) only when nothing upstream adds.
//--------------------------------------------------------------------------------
bool BuildRework(const json& oldEffect, const json& newEffect,
                  const std::string& name, MergePlanRework& outRework)
{
    std::vector<std::string> oldGuids = ExtractGuids(oldEffect);
    std::vector<std::string> newGuidsRaw = ExtractGuids(newEffect);

    bool hasRemoved = false;
    std::vector<std::string> added = GuidDiff(oldGuids, newGuidsRaw, hasRemoved);

    if (added.empty())
        return false; //. nothing upstream adds

    std::vector<std::string> finalGuids;
    if (!hasRemoved || newGuidsRaw.size() != oldGuids.size())
    {
        //_ Add-only: old guids kept, new ones appended (see doc above)
        finalGuids = oldGuids;
        finalGuids.insert(finalGuids.end(), added.begin(), added.end());
    }
    else
    {
        //_ Same count, disjoint: reads as a clean upstream renumber
        finalGuids = newGuidsRaw;
    }

    //_ 1a/2a: name already matches, so oldName == newName; category is
    // left empty on both sides -- these cases never relocate (see merge.h)
    outRework.oldName         = name;
    outRework.newName         = name;
    outRework.oldGuids        = oldGuids;
    outRework.newGuids        = finalGuids;
    return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// PathOf
//--------------------------------------------------------------------------------
// The category path `effect` (a known old effect) currently lives under,
// found via whichever of its guids `idx.guidToPath` recognizes -- every
// guid on a given effect indexes to the same path, so the first hit is as
// good as any. Empty only if none of `effect`'s guids are in `idx`, which
// shouldn't happen for an effect found through `idx` in the first place.
//--------------------------------------------------------------------------------
std::vector<std::string> PathOf(const OldIndex& idx, const json& effect)
{
    for (const auto& g : ExtractGuids(effect))
    {
        auto it = idx.guidToPath.find(g);
        if (it != idx.guidToPath.end())
            return it->second;
    }
    return {};
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BehaviorsConflict
//--------------------------------------------------------------------------------
// True if `candidates`' behaviors don't all agree (1c merges only). An
// empty behaviors list isn't a disagreement -- only candidates with
// something configured are compared, so 0 or 1 such candidates trivially
// can't conflict.
//--------------------------------------------------------------------------------
bool BehaviorsConflict(const std::vector<json*>& candidates)
{
    std::vector<json> nonEmpty;
    for (json* c : candidates)
    {
        if (c->contains("behaviors") && (*c)["behaviors"].is_array() && !(*c)["behaviors"].empty())
            nonEmpty.push_back((*c)["behaviors"]);
    }

    for (size_t i = 1; i < nonEmpty.size(); ++i)
        if (nonEmpty[i] != nonEmpty[0])
            return true;

    return false;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildMergedRework
//--------------------------------------------------------------------------------
// Builds the rework entry for however many old effects `candidates`
// (FindAllByGuid's match order) a new effect's guids touch. candidates[0]
// is always the survivor -- any consistent, deterministic pick is enough,
// since every other candidate is deleted regardless. One candidate is 1b
// (guid known, name differs); more than one is 1c (upstream folded
// several effects into one). Both get the same treatment: upstream's
// name/category win, guids are unioned across every candidate (not just
// the survivor's) so a guid unique to a soon-to-be-deleted candidate isn't
// lost, and 1c additionally records the losers for deletion and checks
// their behaviors for a conflict. Returns false only when there's truly
// nothing to record.
//--------------------------------------------------------------------------------
bool BuildMergedRework(const std::vector<json*>& candidates,
                        const json& newEffect,
                        const std::string& newName,
                        const std::vector<std::string>& newCategoryPath,
                        const OldIndex& idx,
                        MergePlanRework& outRework)
{
    json* survivor = candidates[0];
    const bool isMerge = candidates.size() > 1;

    //_ Union of every candidate's guids, not just the survivor's (see doc)
    std::vector<std::string> unionOldGuids;
    for (json* c : candidates)
        for (const auto& g : ExtractGuids(*c))
            if (std::find(unionOldGuids.begin(), unionOldGuids.end(), g) == unionOldGuids.end())
                unionOldGuids.push_back(g);

    std::vector<std::string> newGuidsRaw = ExtractGuids(newEffect);

    bool hasRemoved = false;
    std::vector<std::string> added = GuidDiff(unionOldGuids, newGuidsRaw, hasRemoved);

    std::vector<std::string> finalGuids;
    bool guidsChanged;
    if (added.empty())
    {
        finalGuids   = unionOldGuids;
        guidsChanged = false;
    }
    else if (!hasRemoved || newGuidsRaw.size() != unionOldGuids.size())
    {
        finalGuids = unionOldGuids;
        finalGuids.insert(finalGuids.end(), added.begin(), added.end());
        guidsChanged = true;
    }
    else
    {
        finalGuids   = newGuidsRaw;
        guidsChanged = true;
    }

    const std::string oldName               = survivor->value("name", std::string());
    const std::vector<std::string> oldPath  = PathOf(idx, *survivor);
    const bool nameChanged     = (oldName != newName);
    const bool categoryChanged = (oldPath != newCategoryPath);

    //_ Single-candidate no-op guard (mirrors BuildRework's own check) --
    // shouldn't normally trigger since 1b is only reached when name differs
    if (!isMerge && !guidsChanged && !nameChanged && !categoryChanged)
        return false;

    outRework.oldName         = oldName;
    outRework.newName         = newName;
    outRework.oldGuids        = ExtractGuids(*survivor); //. identity key, see struct
    outRework.newGuids        = finalGuids;
    outRework.oldCategoryPath = oldPath;
    outRework.newCategoryPath = newCategoryPath;

    outRework.mergedAwayGuids.clear();
    for (size_t i = 1; i < candidates.size(); ++i)
    {
        std::vector<std::string> loserGuids = ExtractGuids(*candidates[i]);
        if (!loserGuids.empty())
            outRework.mergedAwayGuids.push_back(loserGuids.front());
    }

    outRework.behaviorsConflict = isMerge && BehaviorsConflict(candidates);

    //_ Captured now, while `candidates` still has pre-merge data intact --
    // survives even if StripConflictingMergedAwayGuids un-deletes a candidate
    if (outRework.behaviorsConflict)
    {
        for (size_t i = 1; i < candidates.size(); ++i)
        {
            MergePlanMergeCandidate mc;
            mc.name         = candidates[i]->value("name", std::string());
            mc.categoryPath = PathOf(idx, *candidates[i]);
            mc.behaviors    = (candidates[i]->contains("behaviors") && (*candidates[i])["behaviors"].is_array())
                                  ? (*candidates[i])["behaviors"]
                                  : json::array();
            outRework.otherCandidates.push_back(std::move(mc));
        }
    }

    return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ResolvePlan
//--------------------------------------------------------------------------------
// Walks newCategory (read-only) and appends every effect's disposition to
// `plan`, implementing the guid-first/name-fallback rules from merge.h.
// idx reflects oldFile from before this resolve started, so a new effect
// is never compared against another new effect from the same pass.
// Guid-first is a deliberate reversal of this file's original name-first
// design, which silently mismatched whenever multiple old effects shared
// a name.
//--------------------------------------------------------------------------------
void ResolvePlan(const json& newCategory,
                  std::vector<std::string>& pathSoFar,
                  const OldIndex& idx,
                  MergePlan& plan)
{
    if (newCategory.contains("effects") && newCategory["effects"].is_array())
    {
        for (const auto& newEffect : newCategory["effects"])
        {
            if (!newEffect.contains("name") || !newEffect["name"].is_string())
                continue; //. malformed, skip

            //_ Case 0, guid-less: can't be matched reliably; skip so it
            // never collides with another guid-less same-named effect
            if (!HasAnyGuid(newEffect))
                continue;

            const std::string name = newEffect["name"].get<std::string>();

            std::vector<json*> guidMatches = FindAllByGuid(newEffect, idx);

            if (guidMatches.size() == 1)
            {
                //_ Step 1: guid identifies exactly one old effect
                json* guidMatch = guidMatches[0];
                const bool nameMatches =
                    guidMatch->contains("name") && guidMatch->value("name", std::string()) == name;

                if (nameMatches)
                {
                    //_ 1a: identity and name already agree -- refresh guids only
                    MergePlanRework rework;
                    if (BuildRework(*guidMatch, newEffect, name, rework))
                        plan.reworks.push_back(std::move(rework));
                }
                else
                {
                    //_ 1b: same guid-diff logic as 1a, but name/category are
                    // overwritten from the update
                    MergePlanRework rework;
                    if (BuildMergedRework({ guidMatch }, newEffect, name, pathSoFar, idx, rework))
                        plan.reworks.push_back(std::move(rework));
                }
                continue;
            }

            if (guidMatches.size() > 1)
            {
                //_ 1c: guids split across multiple old effects -- always
                // folded into one entry now (see BuildMergedRework)
                MergePlanRework rework;
                if (BuildMergedRework(guidMatches, newEffect, name, pathSoFar, idx, rework))
                    plan.reworks.push_back(std::move(rework));
                continue;
            }

            //_ Step 2: no guid overlap anywhere in oldFile -- fall back to name
            auto range = idx.effectsByName.equal_range(name);
            const size_t candidateCount = static_cast<size_t>(std::distance(range.first, range.second));

            if (candidateCount == 1)
            {
                //_ 2a: exactly one same-named old effect -- full guid
                // refresh under an unchanged name
                MergePlanRework rework;
                if (BuildRework(*range.first->second, newEffect, name, rework))
                    plan.reworks.push_back(std::move(rework));
            }
            else
            {
                //_ candidateCount == 0 (2b: genuinely new) or > 1 (2c:
                // ambiguous, no guid signal to pick) -- insert as new either way
                MergePlanNewEffect insert;
                insert.categoryPath = pathSoFar;
                insert.name         = name;
                insert.effect       = newEffect;
                plan.inserts.push_back(std::move(insert));
            }
        }
    }

    if (newCategory.contains("categories") && newCategory["categories"].is_array())
    {
        for (const auto& newSub : newCategory["categories"])
        {
            if (!newSub.contains("name") || !newSub["name"].is_string())
                continue;
            pathSoFar.push_back(newSub["name"].get<std::string>());
            ResolvePlan(newSub, pathSoFar, idx, plan);
            pathSoFar.pop_back();
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CountGuidsRecursive
//--------------------------------------------------------------------------------
// Recursively tallies how many times each guid string appears across
// every effect under `category`. A guid on two effects increments the
// same key twice; a single effect repeating a guid within its own array
// isn't de-duped, since only file-wide duplication (FindDuplicateGuids'
// concern) matters here.
//--------------------------------------------------------------------------------
void CountGuidsRecursive(const json& category, std::unordered_map<std::string, int>& counts)
{
    if (category.contains("effects") && category["effects"].is_array())
        for (const auto& effect : category["effects"])
            if (effect.contains("guids") && effect["guids"].is_array())
                for (const auto& g : effect["guids"])
                    if (g.is_string())
                        ++counts[g.get<std::string>()];

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            CountGuidsRecursive(sub, counts);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RemoveEffectsRecursive
//--------------------------------------------------------------------------------
// Address-based removal, single pass: strips any effect under `category`
// whose address is in `toRemove`. Only ever called once, against addresses
// from a still-fully-valid OldIndex (see ApplyMergePlan's phase ordering).
// Iterates back-to-front by index, not forward begin()/erase(): erasing
// index i shifts every later element down one slot, reusing its freed
// address, so a forward pass would re-check the shifted-in element against
// the STALE address of what used to be there and cascade into erasing
// everything after it. Back-to-front only ever shifts already-visited
// indices, so this can't happen.
//--------------------------------------------------------------------------------
void RemoveEffectsRecursive(json& category, const std::unordered_set<const json*>& toRemove)
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
            RemoveEffectsRecursive(sub, toRemove);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateCategory
//--------------------------------------------------------------------------------
// Finds, or creates and appends, the child category of oldParent whose name
// matches `name`. Used while applying inserts (after every guid refresh
// has already been applied) and while relocating a 1b/1c rework to its
// new category.
//--------------------------------------------------------------------------------
json& FindOrCreateCategory(json& oldParent, const std::string& name)
{
    if (!oldParent.contains("categories") || !oldParent["categories"].is_array())
        oldParent["categories"] = json::array();

    for (auto& sub : oldParent["categories"])
        if (sub.contains("name") && sub["name"] == name)
            return sub;

    json fresh;
    fresh["name"] = name;
    oldParent["categories"].push_back(std::move(fresh));
    return oldParent["categories"].back();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// StripConflictingMergedAwayGuids
//--------------------------------------------------------------------------------
// Second pass over the fully-resolved plan: a rework's own oldGuids is its
// identity key, but another rework's mergedAwayGuids can end up naming
// that same guid (e.g. a guid gets folded into an unrelated effect's 1c
// merge upstream, while the original effect picks up a new guid and is
// found again via the name fallback). Both can't be right -- the effect is
// alive under its own rework, not a duplicate to delete -- so any
// mergedAwayGuids entry colliding with another rework's identity key is
// dropped here, before ApplyMergePlan can destroy an object a sibling
// rework just updated. Guid-based, not pointer-based, since this only
// touches MergePlan's own strings, never oldFile/newFile.
//--------------------------------------------------------------------------------
void StripConflictingMergedAwayGuids(MergePlan& plan)
{
    std::unordered_set<std::string> claimedByOwnRework;
    for (const auto& rw : plan.reworks)
        for (const auto& g : rw.oldGuids)
            claimedByOwnRework.insert(g);

    for (auto& rw : plan.reworks)
    {
        rw.mergedAwayGuids.erase(
            std::remove_if(rw.mergedAwayGuids.begin(), rw.mergedAwayGuids.end(),
                [&](const std::string& g) { return claimedByOwnRework.count(g) != 0; }),
            rw.mergedAwayGuids.end());
    }
}

} // namespace

MergePlan ResolveMergePlan(const json& oldFile, const json& newFile, bool& outOk)
{
    MergePlan plan;
    outOk = false;

    if (!oldFile.contains("categories") || !oldFile["categories"].is_array())
        return plan;
    if (!newFile.contains("categories") || !newFile["categories"].is_array())
        return plan;

    //_ oldFile is untouched here -- IndexCategory's non-const overload
    // just also serves ApplyMergePlan's mutable-pointer needs below
    OldIndex idx;
    for (auto& cat : const_cast<json&>(oldFile)["categories"])
    {
        if (!cat.contains("name") || !cat["name"].is_string())
            continue; //. malformed, skip
        IndexCategory(cat, std::vector<std::string>{ cat["name"].get<std::string>() }, idx);
    }

    for (const auto& newTop : newFile["categories"])
    {
        if (!newTop.contains("name") || !newTop["name"].is_string())
            continue;
        std::vector<std::string> path{ newTop["name"].get<std::string>() };
        ResolvePlan(newTop, path, idx, plan);
    }

    StripConflictingMergedAwayGuids(plan);

    outOk = true;
    return plan;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ApplyMergePlan
//--------------------------------------------------------------------------------
// Built as one OldIndex up front -- O(effects), not O(reworks x effects) --
// then mutated in four strict phases so every captured pointer stays valid
// throughout: (1) field updates only, never resizing an effects array; (2)
// a single removal pass over the whole tree, using addresses from phase 1
// while they're still valid; (3) re-insert every moved survivor at its new
// location, only now that resizes are safe; (4) apply new-effect
// insertions. Reworks are looked up by guid, never name, since
// rw.oldGuids/mergedAwayGuids pin down the exact node even when several
// old effects share a name.
//--------------------------------------------------------------------------------
void ApplyMergePlan(json& oldFile, const MergePlan& plan)
{
    if (!oldFile.contains("categories") || !oldFile["categories"].is_array())
        return;

    OldIndex idx;
    for (auto& cat : oldFile["categories"])
    {
        if (!cat.contains("name") || !cat["name"].is_string())
            continue;
        IndexCategory(cat, std::vector<std::string>{ cat["name"].get<std::string>() }, idx);
    }

    std::unordered_set<const json*> toRemove;
    std::vector<std::pair<json, std::vector<std::string>>> pendingMoves; // (post-update snapshot, target category path)

    for (const auto& rw : plan.reworks)
    {
        json* survivor = nullptr;
        for (const auto& g : rw.oldGuids)
        {
            auto it = idx.guidToEffect.find(g);
            if (it != idx.guidToEffect.end()) { survivor = it->second; break; }
        }
        if (!survivor)
            continue; //. stale plan, skip

        json guids = json::array();
        for (const auto& g : rw.newGuids)
            guids.push_back(g);
        (*survivor)["guids"] = std::move(guids);
        (*survivor)["name"]  = rw.newName; //. no-op for 1a/2a

        for (const auto& loserGuid : rw.mergedAwayGuids)
        {
            auto it = idx.guidToEffect.find(loserGuid);
            if (it != idx.guidToEffect.end() && it->second != survivor)
                toRemove.insert(it->second);
        }

        //_ Relocate only if the update's path actually differs -- 1a/2a
        // leave both sides empty, so this never triggers for those
        if (!rw.newCategoryPath.empty() && rw.newCategoryPath != rw.oldCategoryPath)
        {
            toRemove.insert(survivor);
            pendingMoves.emplace_back(*survivor, rw.newCategoryPath);
        }
    }

    for (auto& cat : oldFile["categories"])
        RemoveEffectsRecursive(cat, toRemove);

    for (auto& [snapshot, targetPath] : pendingMoves)
    {
        json* cursor = nullptr;
        for (size_t i = 0; i < targetPath.size(); ++i)
        {
            json& parent = (i == 0) ? oldFile : *cursor;
            cursor = &FindOrCreateCategory(parent, targetPath[i]);
        }
        if (!cursor->contains("effects") || !(*cursor)["effects"].is_array())
            (*cursor)["effects"] = json::array();
        (*cursor)["effects"].push_back(std::move(snapshot));
    }

    //_ Phase 4: new-effect insertions
    for (const auto& ins : plan.inserts)
    {
        json* cursor = nullptr;
        for (size_t i = 0; i < ins.categoryPath.size(); ++i)
        {
            json& parent = (i == 0) ? oldFile : *cursor;
            cursor = &FindOrCreateCategory(parent, ins.categoryPath[i]);
        }
        if (!cursor->contains("effects") || !(*cursor)["effects"].is_array())
            (*cursor)["effects"] = json::array();
        (*cursor)["effects"].push_back(ins.effect);
    }
}

std::vector<std::string> FindDuplicateGuids(const json& file)
{
    std::vector<std::string> out;

    if (!file.contains("categories") || !file["categories"].is_array())
        return out;

    std::unordered_map<std::string, int> counts;
    for (const auto& cat : file["categories"])
        CountGuidsRecursive(cat, counts);

    for (const auto& [guid, count] : counts)
        if (count > 1)
            out.push_back(guid);

    return out;
}