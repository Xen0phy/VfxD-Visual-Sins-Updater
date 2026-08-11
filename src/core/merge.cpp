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
// can be tested and read in isolation. ApplyMergePlan mutates oldFile in
// several strict phases so every pointer captured from its own OldIndex
// stays valid until it's no longer needed -- see its own comment for the
// phase breakdown and why the order matters.
//
// Resolving is itself two passes -- ResolveGuidPass then ResolveNamePass --
// so every case-1 (guid) match across the whole newFile is locked in and
// recorded as claimed before any case-2 (name-fallback) match runs. A
// single interleaved pass let a guid match on one new effect and a name
// match on an unrelated new effect resolve against the very same old
// effect (its guid got reassigned upstream while its vacated name/slot
// was independently reused), producing two reworks with the same identity
// key; ApplyMergePlan would apply both against the same node and silently
// drop whichever ran first. See ResolveGuidPass/ResolveNamePass.
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
// True if `effect` has at least one string guid. A guid-less effect can
// never be tracked reliably across releases -- see the case-0 skip in
// ResolveGuidPass below.
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
// candidate and miss the other. See ResolveGuidPass for how the full set
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
// BuildRework
//--------------------------------------------------------------------------------
// Decides skip/add-only/replace for a matched old/new effect pair (guid or
// unambiguous name match) by diffing this effect's own guid list -- never
// a whole-file set, which would let one already-known guid mask other,
// genuinely new ones on the same effect:
//   added empty                      -> guids untouched
//   added, nothing removed           -> add-only, old guids kept
//   added and removed, counts differ -> add-only (too ambiguous to drop)
//   added and removed, counts match  -> replace (clean upstream renumber)
// Also compares oldEffect's actual category (via idx) against
// newCategoryPath: guid/name matching (1a/2a) says nothing about whether
// upstream also reorganized the category tree, so that can't be assumed
// unchanged -- see NEXUS_REVIEW.md. Returns false (no outRework) only when
// neither the guids nor the category actually changed.
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

    //_ 1a/2a: name already matches, so oldName == newName; category is
    // still recorded both sides so ApplyMergePlan can relocate when it
    // actually moved (see merge.h field doc for MergePlanRework)
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

//********************************************************************************
// PendingByName
//--------------------------------------------------------------------------------
// newEffect      the new-file effect deferred to Step 2 (owned by newFile,
//                which outlives both passes -- see ResolveMergePlan)
// name           pulled out once so ResolveNamePass never re-checks it
// categoryPath   root -> immediate parent, same convention as pathSoFar
//--------------------------------------------------------------------------------
// A new effect ResolveGuidPass found no guid overlap for. Held until every
// guid match across the whole newFile has run, so ResolveNamePass can tell
// a genuinely unclaimed same-named old effect from one that's already
// spoken for -- see the file header for why that ordering matters.
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
// Walks newCategory (read-only), resolving every Step-1 guid match on the
// spot exactly as before, and deferring anything guid-less or with no guid
// overlap into `pending` instead of touching idx.effectsByName itself.
// Every old effect any guid match touches -- survivor or merged-away
// candidate alike -- goes into `claimed`, even when BuildRework/
// BuildMergedRework end up returning false (nothing to rework): the guid
// identity is still spoken for, so ResolveNamePass must never pick it up
// as an unrelated same-named candidate.
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

            //_ Case 0, guid-less: can't be matched reliably; skip so it
            // never collides with another guid-less same-named effect
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
                claimed.insert(m); //. spoken for, regardless of outcome below

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
                    //_ 1b: same guid-diff logic as 1a, but name/category are
                    // overwritten from the update
                    MergePlanRework rework;
                    if (BuildMergedRework({ guidMatch }, newEffect, name, pathSoFar, idx, rework))
                        plan.reworks.push_back(std::move(rework));
                }
            }
            else
            {
                //_ 1c: guids split across multiple old effects -- always
                // folded into one entry now (see BuildMergedRework)
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
// Runs Step 2 over every effect ResolveGuidPass deferred, once guid
// matching for the entire newFile is done and `claimed` is final. Filters
// idx.effectsByName's candidates against `claimed` before counting, so an
// old effect a Step-1 match already consumed can't also be matched here by
// coincidence of name. A candidate this pass itself resolves (2a) is added
// to `claimed` too, so two pending effects that happen to share a name
// still can't both land on the same single unclaimed candidate.
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
            //_ 2a: exactly one unclaimed same-named old effect -- full guid
            // refresh under an unchanged name
            MergePlanRework rework;
            if (BuildRework(*candidates[0], *item.newEffect, item.name, idx, item.categoryPath, rework))
                plan.reworks.push_back(std::move(rework));
            claimed.insert(candidates[0]);
        }
        else
        {
            //_ candidates.empty() (2b: genuinely new, or every same-named
            // old effect was already claimed elsewhere) or > 1 (2c:
            // ambiguous, no guid signal to pick) -- insert as new either way
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
// CollectCategoryDescriptions
//--------------------------------------------------------------------------------
// Recursively walks newFile's category tree, recording every non-empty
// "description" into `out`, keyed by JoinCategoryPathKey(pathSoFar).
// Deliberately independent of effect matching -- a category's description
// is upstream metadata about the category itself, not something tied to
// any one effect inside it, so this doesn't piggyback on
// ResolveGuidPass/ResolveNamePass at all.
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
// Walks EVERY category already sitting in `category` -- brand-new ones
// FindOrCreateCategory just created moments ago, categories a relocation
// landed effects into, and categories that were already there on disk
// completely untouched by this update -- and fills in `descriptions[path]`
// for any of them whose own "description" is currently missing or empty.
// A category with ANY non-empty description, however it got there
// (hand-written locally, or filled by an earlier update), is never
// touched -- same "don't clobber something already there" rule
// BuildRework/BuildMergedRework apply to effect fields, just checked
// directly against blankness instead of inferred from match provenance.
//
// Deliberately a single top-to-bottom pass over the WHOLE tree rather than
// scoped to what the merge plan touched: a category can have sat blank for
// releases with nothing ever relocating through it, so there's no rework/
// insert entry to hang this off of. Run once, at the very end of
// ApplyMergePlan (after inserts/relocations/pruning), so it only ever
// looks at the tree's final shape -- see BuildDiffOverlayTree's own mirror
// of this same pass for the preview side.
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
// matches `name`. Used while applying inserts (after every guid refresh
// has already been applied) and while relocating any rework whose category
// changed (1a/1b/1c/2a alike -- see BuildRework/BuildMergedRework).
//
// Deliberately description-agnostic -- a freshly-created category comes out
// name-only here; FillBlankCategoryDescriptions (run once, at the very end
// of ApplyMergePlan) is what seeds it, in the exact same pass that also
// backfills a pre-existing-but-blank category elsewhere in the tree. See
// that function's own doc for why centralizing it there, rather than here,
// covers both cases with one rule instead of two.
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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// PruneEmptyCategories
//--------------------------------------------------------------------------------
// Recursively strips any subcategory left with no effects and no
// non-empty subcategories of its own, post-order so a parent that's only
// empty because its last surviving child was just pruned in this same
// pass is caught too. Runs at the end of ApplyMergePlan so a relocation
// that fully vacates an old branch (e.g. a category ArenaNet reorganized
// upstream) doesn't leave a dead, effect-less shell of the old tree
// sitting alongside the new one. Only ever prunes *subcategories* --
// top-level categories are left alone even if empty, since those are
// stable named groups the UI expects to always find; `category` itself is
// mutated in place, and the return value tells the caller whether
// `category` itself is now empty (so it, in turn, can be pruned by ITS
// parent -- irrelevant for top-level callers, which ignore it).
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

    //_ Shared across every top-level category so a guid match anywhere in
    // newFile is claimed before ResolveNamePass runs anywhere -- see the
    // file header and ResolveGuidPass/ResolveNamePass for why that matters
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

    //_ Independent of guid/name matching above -- see the function's own
    // comment. Populated unconditionally, whether or not this newFile
    // category ends up needing to be freshly created; FindOrCreateCategory/
    // FindOrCreateDiffCategory are what decide whether it's actually used.
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
// then mutated in five strict phases so every captured pointer stays valid
// throughout: (1) field updates only, never resizing an effects array; (2)
// a single removal pass over the whole tree, using addresses from phase 1
// while they're still valid; (3) re-insert every moved survivor at its new
// location, only now that resizes are safe; (4) apply new-effect
// insertions; (5) prune any subcategory branch left fully empty by phases
// 2-3 (see PruneEmptyCategories). Reworks are looked up by guid, never
// name, since rw.oldGuids/mergedAwayGuids pin down the exact node even
// when several old effects share a name.
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

        //_ Relocate only if the update's path actually differs -- for
        // 1a/2a this now fires too whenever upstream moved the effect's
        // category (see BuildRework)
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

    //_ Phase 5: drop any subcategory branch a relocation left fully empty
    // (e.g. the old side of a category ArenaNet reorganized upstream)
    for (auto& cat : oldFile["categories"])
        PruneEmptyCategories(cat);

    //_ Phase 6: backfill any category -- new or pre-existing -- still
    // missing a description upstream has one for. Run last, after pruning,
    // so a category phase 5 is about to delete never gets written to first.
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