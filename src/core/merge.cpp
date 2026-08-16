//################################################################################
// merge.cpp   (see: merge.h)
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
// guidToEffect   guid -> owning old effect; last-indexed wins if guids repeat
// guidToPath     guid -> category path (root -> parent); fills oldCategoryPath
// effectsByName  name -> every old effect sharing it (names can repeat in GW2)
//--------------------------------------------------------------------------------
// A flat, built-once snapshot of oldFile, built fresh before each of
// ResolveMergePlan/ApplyMergePlan's own walks and never mutated while in use --
// see each caller for why that matters there. Leans on guids never repeating
// within a file (confirmed for how ArenaNet ships these, not enforced against a
// hand-edited copy -- see FindDuplicateGuids); a violation wouldn't crash
// anything, just risk resolving against the wrong same-guid effect.
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
// ResolveGuidPass's own pathSoFar) -- the caller pushes it on before recursing,
// mirroring exactly how ResolveGuidPass builds newFile's path on the way down.
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
// True if `effect` has at least one string guid. A guid-less effect can never
// be tracked reliably across releases -- see the case-0 skip in ResolveGuidPass
// below.
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
// Every distinct old effect owning at least one of `newEffect`'s guids (each
// listed once), or empty if none are claimed in oldFile. Checks every guid
// instead of stopping at the first hit: a new effect's guid list can straddle
// more than one old effect (e.g. an upstream merge of two effects into one) --
// stopping early would silently pick one candidate and miss the other. See
// ResolveGuidPass for how the full set returned here resolves into 1a/1b/1c.
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
// Diffs a matched effect's old guid list `o` against its new list `n`: returns
// guids in `n` but not `o` (de-duplicated, `n`'s order), and reports via
// `outHasRemoved` whether `o` has anything `n` doesn't. Together these decide
// skip/add-only/replace -- see BuildRework -- raw counts alone can't, since the
// same count can mean identical, disjoint, or partially-overlapping sets.
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
// PathOf
//--------------------------------------------------------------------------------
// The category path `effect` (a known old effect) currently lives under, found
// via whichever of its guids `idx.guidToPath` recognizes -- every guid on a
// given effect indexes to the same path, so the first hit is as good as any.
// Empty only if none of `effect`'s guids are in `idx`, which shouldn't happen
// for an effect found through `idx` in the first place.
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
// BuildRework
//--------------------------------------------------------------------------------
// Decides skip/add-only/replace for a matched pair by diffing this effect's own
// guid list, never a whole-file set (which would let one known guid mask other
// genuinely new ones on the same effect):
//   added empty                      -> guids untouched
//   added, nothing removed           -> add-only, old guids kept
//   added and removed, counts differ -> add-only (too ambiguous to drop)
//   added and removed, counts match  -> replace (clean upstream renumber)
// Also compares oldEffect's category (via idx) against newCategoryPath, since
// guid/name matching alone says nothing about a tree reorg -- see
// NEXUS_REVIEW.md. Returns false only when neither guids nor category changed.
//--------------------------------------------------------------------------------
bool BuildRework(const json& oldEffect, const json& newEffect,
                  const std::string& name, const OldIndex& idx,
                  const std::vector<std::string>& newCategoryPath,
                  MergePlanRework& outRework)
{
    std::vector<std::string> oldGuids = ExtractGuids(oldEffect);
    std::vector<std::string> newGuidsRaw = ExtractGuids(newEffect);

    bool hasRemoved = false;
    std::vector<std::string> added = GuidDiff(oldGuids, newGuidsRaw, hasRemoved);

    std::vector<std::string> oldCategoryPath = PathOf(idx, oldEffect);
    const bool categoryChanged = (oldCategoryPath != newCategoryPath);

    if (added.empty() && !categoryChanged)
        return false; //. nothing upstream added or moved

    std::vector<std::string> finalGuids;
    if (added.empty())
    {
        //_ Guids untouched -- only the category moved
        finalGuids = oldGuids;
    }
    else if (!hasRemoved || newGuidsRaw.size() != oldGuids.size())
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

    //_ 1a/2a: name unchanged; category recorded both sides for relocation
    outRework.oldName         = name;
    outRework.newName         = name;
    outRework.oldGuids        = oldGuids;
    outRework.newGuids        = finalGuids;
    outRework.oldCategoryPath = oldCategoryPath;
    outRework.newCategoryPath = newCategoryPath;
    return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BehaviorsConflict
//--------------------------------------------------------------------------------
// True if `candidates`' behaviors don't all agree (1c merges only). An empty
// behaviors list isn't a disagreement -- only candidates with something
// configured are compared, so 0 or 1 such candidates trivially can't conflict.
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
// (FindAllByGuid's match order) a new effect's guids touch. candidates[0] is
// always the survivor -- any consistent, deterministic pick is enough, since
// every other candidate is deleted regardless. One candidate is 1b (guid known,
// name differs); more than one is 1c (upstream folded several effects into
// one). Both get the same treatment: upstream's name/category win, guids are
// unioned across every candidate (not just the survivor's) so a guid unique to
// a soon-to-be-deleted candidate isn't lost, and 1c additionally records the
// losers for deletion and checks their behaviors for a conflict. Returns false
// only when there's nothing to record.
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

    //_ No-op guard (mirrors BuildRework), rarely fires -- 1b implies renaming
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

    //_ Captured now, before an un-delete by StripConflictingMergedAwayGuids
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

//********************************************************************************
// PendingByName
//--------------------------------------------------------------------------------
// newEffect      the new-file effect deferred to Step 2 (owned by newFile,
//                which outlives both passes -- see ResolveMergePlan)
// name           pulled out once so ResolveNamePass never re-checks it
// categoryPath   root -> immediate parent, same convention as pathSoFar
//--------------------------------------------------------------------------------
// A new effect ResolveGuidPass found no guid overlap for. Held until every guid
// match across the whole newFile has run, so ResolveNamePass can tell a
// genuinely unclaimed same-named old effect from one that's already spoken for
// -- see ResolveMergePlan for why that ordering matters.
//--------------------------------------------------------------------------------
struct PendingByName
{
    const json*                newEffect;
    std::string                name;
    std::vector<std::string>   categoryPath;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ResolveGuidPass
//--------------------------------------------------------------------------------
// Walks newCategory (read-only), resolving every Step-1 guid match on the spot
// exactly as before, and deferring anything guid-less or with no guid overlap
// into `pending` instead of touching idx.effectsByName itself. Every old effect
// any guid match touches -- survivor or merged-away candidate alike -- goes
// into `claimed`, even when BuildRework/BuildMergedRework end up returning
// false (nothing to rework): the guid identity is still spoken for, so
// ResolveNamePass must never pick it up as an unrelated same-named candidate.
//--------------------------------------------------------------------------------
void ResolveGuidPass(const json& newCategory,
                      std::vector<std::string>& pathSoFar,
                      const OldIndex& idx,
                      MergePlan& plan,
                      std::unordered_set<const json*>& claimed,
                      std::vector<PendingByName>& pending)
{
    if (newCategory.contains("effects") && newCategory["effects"].is_array())
    {
        for (const auto& newEffect : newCategory["effects"])
        {
            if (!newEffect.contains("name") || !newEffect["name"].is_string())
                continue; //. malformed, skip

            //_ Case 0, guid-less: skip -- unmatchable, avoids name collisions
            if (!HasAnyGuid(newEffect))
                continue;

            const std::string name = newEffect["name"].get<std::string>();

            std::vector<json*> guidMatches = FindAllByGuid(newEffect, idx);

            if (guidMatches.empty())
            {
                //_ Step 2 deferred -- see ResolveNamePass
                pending.push_back({ &newEffect, name, pathSoFar });
                continue;
            }

            for (json* m : guidMatches)
                claimed.insert(m); //. spoken for regardless of outcome

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
                    if (BuildRework(*guidMatch, newEffect, name, idx, pathSoFar, rework))
                        plan.reworks.push_back(std::move(rework));
                }
                else
                {
                    //_ 1b: like 1a, but overwrites name/category too
                    MergePlanRework rework;
                    if (BuildMergedRework({ guidMatch }, newEffect, name, pathSoFar, idx, rework))
                        plan.reworks.push_back(std::move(rework));
                }
            }
            else
            {
                //_ 1c: guids split across old effects, folded into one entry
                MergePlanRework rework;
                if (BuildMergedRework(guidMatches, newEffect, name, pathSoFar, idx, rework))
                    plan.reworks.push_back(std::move(rework));
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
            ResolveGuidPass(newSub, pathSoFar, idx, plan, claimed, pending);
            pathSoFar.pop_back();
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ResolveNamePass
//--------------------------------------------------------------------------------
// Runs Step 2 over every effect ResolveGuidPass deferred, once guid matching
// for the entire newFile is done and `claimed` is final. Filters
// idx.effectsByName's candidates against `claimed` before counting, so an old
// effect a Step-1 match already consumed can't also be matched here by
// coincidence of name. A candidate this pass itself resolves (2a) is added to
// `claimed` too, so two pending effects that happen to share a name still can't
// both land on the same single unclaimed candidate.
//--------------------------------------------------------------------------------
void ResolveNamePass(const std::vector<PendingByName>& pending,
                      const OldIndex& idx,
                      std::unordered_set<const json*>& claimed,
                      MergePlan& plan)
{
    for (const auto& item : pending)
    {
        auto range = idx.effectsByName.equal_range(item.name);

        std::vector<json*> candidates;
        for (auto it = range.first; it != range.second; ++it)
            if (!claimed.count(it->second))
                candidates.push_back(it->second);

        if (candidates.size() == 1)
        {
            //_ 2a: one unclaimed same-named match, full guid refresh
            MergePlanRework rework;
            if (BuildRework(*candidates[0], *item.newEffect, item.name, idx, item.categoryPath, rework))
                plan.reworks.push_back(std::move(rework));
            claimed.insert(candidates[0]);
        }
        else
        {
            //_ 2b/2c: none unclaimed, or several ambiguous -- insert as new
            MergePlanNewEffect insert;
            insert.categoryPath = item.categoryPath;
            insert.name         = item.name;
            insert.effect       = *item.newEffect;
            plan.inserts.push_back(std::move(insert));
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CountGuidsRecursive
//--------------------------------------------------------------------------------
// Recursively tallies how many times each guid string appears across every
// effect under `category`. A guid on two effects increments the same key twice;
// a single effect repeating a guid within its own array isn't de-duped, since
// only file-wide duplication (FindDuplicateGuids' concern) matters here.
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
// Address-based removal, single pass: strips any effect under `category` whose
// address is in `toRemove`. Only ever called once, against addresses from a
// still-fully-valid OldIndex (see ApplyMergePlan's phase ordering). Iterates
// back-to-front by index, not forward begin()/erase(): erasing index i shifts
// every later element down one slot, reusing its freed address, so a forward
// pass would re-check the shifted-in element against the STALE address of what
// used to be there and cascade into erasing everything after it. Back-to-front
// only ever shifts already-visited indices, so this can't happen.
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
// CollectCategoryDescriptions
//--------------------------------------------------------------------------------
// Recursively walks newFile's category tree, recording every non-empty
// "description" into `out`, keyed by JoinCategoryPathKey(pathSoFar).
// Independent of effect matching -- a category's description is upstream
// metadata about the category itself, not something tied to any one effect
// inside it, so this doesn't piggyback on ResolveGuidPass/ResolveNamePass at
// all.
//--------------------------------------------------------------------------------
void CollectCategoryDescriptions(const json& category, std::vector<std::string>& pathSoFar,
                                  std::unordered_map<std::string, std::string>& out)
{
    if (category.contains("description") && category["description"].is_string())
    {
        const std::string desc = category["description"].get<std::string>();
        if (!desc.empty())
            out[JoinCategoryPathKey(pathSoFar)] = desc;
    }

    if (category.contains("categories") && category["categories"].is_array())
    {
        for (const auto& sub : category["categories"])
        {
            if (!sub.contains("name") || !sub["name"].is_string())
                continue;
            pathSoFar.push_back(sub["name"].get<std::string>());
            CollectCategoryDescriptions(sub, pathSoFar, out);
            pathSoFar.pop_back();
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FillBlankCategoryDescriptions
//--------------------------------------------------------------------------------
// Walks every category in `category` -- freshly created, relocation targets,
// and pre-existing ones alike -- filling `descriptions[path]` into any whose
// own "description" is missing or empty; a category with any non-empty
// description, however it got there, is never touched (the same blankness rule
// BuildRework/BuildMergedRework apply to effect fields). A single top-to-bottom
// pass over the whole tree, not scoped to what the merge plan touched: a
// category can sit blank for releases with nothing ever relocating through it,
// so there's no rework/insert entry to hang this off of. Runs once, at the end
// of ApplyMergePlan, mirrored by BuildDiffOverlayTree for the preview side.
//--------------------------------------------------------------------------------
void FillBlankCategoryDescriptions(json& category, std::vector<std::string>& pathSoFar,
                                    const std::unordered_map<std::string, std::string>& descriptions)
{
    const bool hasDesc = category.contains("description") && category["description"].is_string()
                          && !category["description"].get<std::string>().empty();
    if (!hasDesc)
    {
        auto it = descriptions.find(JoinCategoryPathKey(pathSoFar));
        if (it != descriptions.end())
            category["description"] = it->second;
    }

    if (category.contains("categories") && category["categories"].is_array())
    {
        for (auto& sub : category["categories"])
        {
            if (!sub.contains("name") || !sub["name"].is_string())
                continue;
            pathSoFar.push_back(sub["name"].get<std::string>());
            FillBlankCategoryDescriptions(sub, pathSoFar, descriptions);
            pathSoFar.pop_back();
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateCategory
//--------------------------------------------------------------------------------
// Finds, or creates and appends, the child category of oldParent whose name
// matches `name`. Used while applying inserts (after every guid refresh has
// already been applied) and while relocating any rework whose category changed
// (1a/1b/1c/2a alike -- see BuildRework/BuildMergedRework).
// Description-agnostic -- a freshly-created category comes out name-only here;
// FillBlankCategoryDescriptions (run once, at the end of ApplyMergePlan) is
// what seeds it, in the exact same pass that also backfills a
// pre-existing-but-blank category elsewhere in the tree, so one rule covers
// both cases instead of two.
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
// identity key, but another rework's mergedAwayGuids can end up naming that
// same guid (e.g. a guid gets folded into an unrelated effect's 1c merge
// upstream, while the original effect picks up a new guid and is found again
// via the name fallback). Both can't be right -- the effect is alive under its
// own rework, not a duplicate to delete -- so any mergedAwayGuids entry
// colliding with another rework's identity key is dropped here, before
// ApplyMergePlan can destroy an object a sibling rework just updated.
// Guid-based, not pointer-based, since this only touches MergePlan's own
// strings, never oldFile/newFile.
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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// PruneEmptyCategories
//--------------------------------------------------------------------------------
// Recursively strips any subcategory left with no effects and no non-empty
// subcategories of its own, post-order so a parent that's only empty because
// its last surviving child was just pruned in this same pass is caught too.
// Runs at the end of ApplyMergePlan so a relocation that fully vacates an old
// branch (e.g. a category ArenaNet reorganized upstream) doesn't leave a dead,
// effect-less shell of the old tree alongside the new one. Only ever prunes
// *subcategories* -- top-level categories are left alone even if empty, since
// those are stable named groups the UI expects to always find. `category` is
// mutated in place; the return value tells the caller whether it is now empty
// too, so it can be pruned by its own parent in turn.
//--------------------------------------------------------------------------------
bool PruneEmptyCategories(json& category)
{
    if (category.contains("categories") && category["categories"].is_array())
    {
        auto& subs = category["categories"];
        for (size_t i = subs.size(); i-- > 0; )
            if (PruneEmptyCategories(subs[i]))
                subs.erase(subs.begin() + i);
    }

    const bool hasEffects =
        category.contains("effects") && category["effects"].is_array() && !category["effects"].empty();
    const bool hasSubcategories =
        category.contains("categories") && category["categories"].is_array() && !category["categories"].empty();

    return !hasEffects && !hasSubcategories;
}

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ResolveMergePlan
//--------------------------------------------------------------------------------
// Builds oldFile's OldIndex once, then runs ResolveGuidPass over every
// top-level category before ResolveNamePass runs at all -- see merge.h for the
// case table. A single interleaved pass could let a guid match on one new
// effect and a name match on an unrelated new effect resolve against the same
// old effect (its guid reassigned upstream while its vacated name/slot got
// independently reused), producing two reworks with the same identity key that
// ApplyMergePlan would apply against the same node, silently dropping whichever
// ran first.
//--------------------------------------------------------------------------------
MergePlan ResolveMergePlan(const json& oldFile, const json& newFile, bool& outOk)
{
    MergePlan plan;
    outOk = false;

    if (!oldFile.contains("categories") || !oldFile["categories"].is_array())
        return plan;
    if (!newFile.contains("categories") || !newFile["categories"].is_array())
        return plan;

    //_ oldFile untouched; IndexCategory overload also serves ApplyMergePlan
    OldIndex idx;
    for (auto& cat : const_cast<json&>(oldFile)["categories"])
    {
        if (!cat.contains("name") || !cat["name"].is_string())
            continue; //. malformed, skip
        IndexCategory(cat, std::vector<std::string>{ cat["name"].get<std::string>() }, idx);
    }

    //_ Shared across top-level categories -- see comment above for why
    std::unordered_set<const json*> claimed;
    std::vector<PendingByName> pending;

    for (const auto& newTop : newFile["categories"])
    {
        if (!newTop.contains("name") || !newTop["name"].is_string())
            continue;
        std::vector<std::string> path{ newTop["name"].get<std::string>() };
        ResolveGuidPass(newTop, path, idx, plan, claimed, pending);
    }

    ResolveNamePass(pending, idx, claimed, plan);

    StripConflictingMergedAwayGuids(plan);

    //_ Independent of guid/name matching; always populated, used or not
    for (const auto& newTop : newFile["categories"])
    {
        if (!newTop.contains("name") || !newTop["name"].is_string())
            continue;
        std::vector<std::string> path{ newTop["name"].get<std::string>() };
        CollectCategoryDescriptions(newTop, path, plan.newCategoryDescriptions);
    }

    outOk = true;
    return plan;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ApplyMergePlan
//--------------------------------------------------------------------------------
// Built as one OldIndex up front -- O(effects), not O(reworks x effects) --
// then mutated in six strict phases so every captured pointer stays valid
// throughout: (1) field updates only, never resizing an effects array; (2) one
// removal pass over the whole tree, using phase-1 addresses while they're still
// valid; (3) re-insert every moved survivor at its new location, only now that
// resizes are safe; (4) new-effect insertions; (5) prune any subcategory branch
// left empty by phases 2-3 (see PruneEmptyCategories); (6) backfill any
// category still missing a description (see FillBlankCategoryDescriptions).
// Reworks are looked up by guid, never name, since rw.oldGuids/mergedAwayGuids
// pin down the exact node even when several old effects share a name.
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
    std::vector<std::pair<json, std::vector<std::string>>> pendingMoves; //. (post-update snapshot, target category path)

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

        //_ Relocate only if the path differs -- 1a/2a fires too if moved
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

    //_ Phase 5: drop any subcategory branch a relocation left empty
    for (auto& cat : oldFile["categories"])
        PruneEmptyCategories(cat);

    //_ Phase 6: backfill descriptions last, after pruning deletes its targets
    for (auto& cat : oldFile["categories"])
    {
        if (!cat.contains("name") || !cat["name"].is_string())
            continue;
        std::vector<std::string> path{ cat["name"].get<std::string>() };
        FillBlankCategoryDescriptions(cat, path, plan.newCategoryDescriptions);
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