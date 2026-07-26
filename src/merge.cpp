#include "merge.h"
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

namespace {

// A flat, built-once snapshot of oldFile:
//   - guidToEffect: guid -> owning old effect. Guids are globally unique
//     (an assumption confirmed for this project), so this is a safe 1:1
//     index -- no guid can ever point at two different effects.
//   - guidToPath: guid -> the category path (root -> immediate parent, in
//     order, same convention as MergePlanNewEffect::categoryPath) the
//     owning old effect currently lives under. Only used at resolve time,
//     to record MergePlanRework::oldCategoryPath so a 1b/1c rework can
//     later tell whether the update actually moves it.
//   - effectsByName: name -> every old effect with that name. Deliberately
//     a multimap, not a single pointer -- names are NOT guaranteed unique
//     (GW2 reuses display names across genuinely distinct effects), so
//     collapsing to one pointer per name was itself a bug: it silently
//     picked one arbitrary same-named effect as "the" match and both wrote
//     to it and displayed it as if every same-named effect were involved.
// Built once up front and never mutated afterward while in use -- see the
// notes below on why that matters in each of the two callers.
struct OldIndex
{
    std::unordered_map<std::string, json*> guidToEffect;
    std::unordered_map<std::string, std::vector<std::string>> guidToPath;
    std::unordered_multimap<std::string, json*> effectsByName;
};

// `pathSoFar` already includes `category`'s own name (same convention as
// ResolvePlan's own pathSoFar) -- the caller pushes it on before recursing,
// mirroring exactly how ResolvePlan builds newFile's path on the way down.
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
                continue; // malformed sub-category -- no name to extend the path with, skip it
            std::vector<std::string> subPath = pathSoFar;
            subPath.push_back(sub["name"].get<std::string>());
            IndexCategory(sub, subPath, idx);
        }
    }
}

// True if `effect` has at least one string guid. An effect with no guids
// field, a non-array guids field, or an empty guids array can never be
// tracked reliably across releases (nothing to match against, and nothing
// to distinguish it from any other guid-less effect sharing its name) --
// see the guid-less skip in ResolvePlan below.
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

// Returns every distinct old effect owning at least one of `newEffect`'s
// guids (each such effect listed once, even if more than one of
// `newEffect`'s own guids happens to match it), or an empty vector if none
// of them are claimed anywhere in oldFile.
//
// Deliberately checks every guid in `newEffect` rather than stopping at the
// first hit. A single shared guid does pin down one specific old effect
// (guids are globally unique -- confirmed by FindDuplicateGuids), but that
// only means each *individual* guid can't be ambiguous on its own. It says
// nothing about a case where a new effect lists several guids that
// individually belong to two *different* old effects -- e.g. upstream
// merging two previously-separate effects into one, which would ship as a
// single new-file entry whose guid list straddles both. Stopping at the
// first match would silently pick one of the two candidates and never even
// look at the guid that pointed at the other -- see ResolvePlan below for
// how the full set returned here gets resolved (unambiguous single match,
// name-confirmed split match, or give up rather than guess).
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

// Compares a matched effect's old guid list `o` against its new guid list
// `n` and returns the guids present in `n` but not in `o` (order preserved
// from `n`, de-duplicated), while reporting via `outHasRemoved` whether `o`
// contains anything `n` doesn't. Together these two facts are enough to
// decide skip / add-only / replace -- see the table in ResolvePlan below;
// raw counts alone are not (same count can mean identical, disjoint, or
// partially-overlapping sets, and none of those need the same handling).
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

// Given a matched old effect (found either by guid or by an unambiguous
// name match) and the new effect being merged, decides skip / add-only /
// replace by comparing this effect's own old guid list against its new
// one -- never a whole-file index. That distinction matters as soon as an
// effect carries more than one guid: checking against a *global* set (as a
// single combined test) means a single already-known guid makes the whole
// effect look "already known," silently swallowing any genuinely new guids
// sitting alongside it. Comparing against just the matched effect's own
// list avoids that:
//
//   added   = guids in the new list not already in the old list
//   removed = true if the old list has anything the new list doesn't
//
//   added empty                        -> skip (nothing upstream adds)
//   added non-empty, !removed          -> add-only (old fully retained,
//                                          new ones appended)
//   added & removed, counts differ     -> add-only (ambiguous shape --
//                                          never drop a guid we're not
//                                          sure is a clean 1:1 swap)
//   added & removed, counts match      -> replace (reads as an upstream
//                                          renumber of every guid)
//
// Returns true and fills `outRework` if there's something to record; false
// if the old list already covers everything (nothing to do).
bool BuildRework(const json& oldEffect, const json& newEffect,
                  const std::string& name, MergePlanRework& outRework)
{
    std::vector<std::string> oldGuids = ExtractGuids(oldEffect);
    std::vector<std::string> newGuidsRaw = ExtractGuids(newEffect);

    bool hasRemoved = false;
    std::vector<std::string> added = GuidDiff(oldGuids, newGuidsRaw, hasRemoved);

    if (added.empty())
        return false; // old already covers everything upstream lists

    std::vector<std::string> finalGuids;
    if (!hasRemoved || newGuidsRaw.size() != oldGuids.size())
    {
        // Superset (old fully retained, just append what's new) or a
        // mixed change whose counts don't even line up (too ambiguous to
        // treat as a clean swap) -- either way, keep the old guids and
        // only add the new ones.
        finalGuids = oldGuids;
        finalGuids.insert(finalGuids.end(), added.begin(), added.end());
    }
    else
    {
        // Same count, neither list contains the other: reads as an
        // unambiguous upstream renumber of every guid.
        finalGuids = newGuidsRaw;
    }

    // 1a/2a: name is confirmed to already match (that's how these two
    // cases get here in the first place -- see ResolvePlan), so oldName/
    // newName are always equal here. Category path is deliberately left
    // empty on both sides -- these two cases never move a category (see
    // merge.h's case-1a/2a doc comment) -- which BuildMergedRework's
    // callers rely on: an empty newCategoryPath is the signal that means
    // "nothing to relocate" everywhere else in this file.
    outRework.oldName         = name;
    outRework.newName         = name;
    outRework.oldGuids        = oldGuids;
    outRework.newGuids        = finalGuids;
    return true;
}

// The category path (root -> immediate parent, in order) `effect` -- an
// old effect known to `idx` -- currently lives under, found via whichever
// of its own guids `idx.guidToPath` recognizes. Every guid on a given old
// effect was indexed against the same path (see IndexCategory), so the
// first one found is as good as any; returns an empty path only if none of
// `effect`'s guids are in `idx` at all, which shouldn't happen for an
// effect that was itself found *through* `idx` in the first place.
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

// True if `candidates`' behaviors (settings) don't all agree -- used only
// for a 1c merge (2+ candidates), to decide whether the survivor's kept
// settings need a second look. An empty behaviors list doesn't count as a
// disagreement (an effect with no configured behaviors yet isn't "in
// conflict" with one that has some) -- only candidates that actually have
// something configured are compared against each other, and 0 or 1 such
// candidates can't disagree with themselves.
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

// Builds the merge/rework plan entry for however many old effects
// `candidates` (in FindAllByGuid's own match order) a new effect's guids
// touch. `candidates[0]` is always the survivor -- see
// MergePlanRework::mergedAwayGuids's doc comment for why picking the
// first-matched one (rather than, say, the one with the most guids) is
// good enough: it just needs to be *a* consistent, deterministic choice,
// since every other matched candidate is being deleted regardless of
// which one survives. With exactly one candidate this is 1b (guid known,
// name differs); with more than one it's 1c (an upstream merge of
// previously-separate effects). Both get the same treatment here --
// upstream's name/category win, guids are unioned across every candidate
// rather than just the survivor's own (so a guid unique to a
// soon-to-be-deleted candidate isn't silently lost) -- 1c additionally
// records the other candidates for deletion and checks their behaviors
// for a conflict. Returns false (nothing to record) only when there's
// truly nothing to do: a single candidate whose guids/name/category
// already match the update exactly -- which BuildRework's caller (1a)
// already routes around before this function is ever called for a single
// candidate, but is checked here too since it's a real possibility for the
// "1b" call site when the only actual difference already resolved down to
// nothing.
bool BuildMergedRework(const std::vector<json*>& candidates,
                        const json& newEffect,
                        const std::string& newName,
                        const std::vector<std::string>& newCategoryPath,
                        const OldIndex& idx,
                        MergePlanRework& outRework)
{
    json* survivor = candidates[0];
    const bool isMerge = candidates.size() > 1;

    // Union every matched candidate's own guid list -- never just the
    // survivor's -- so a guid living only on a soon-to-be-deleted
    // candidate still counts as "already known" (rather than looking
    // newly-added) and isn't silently lost if the update's own list
    // doesn't happen to repeat it.
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

    // A single candidate (1b's call site) whose guids/name/category all
    // already match the update exactly has nothing left to record --
    // shouldn't normally happen (1b is only reached when name differs),
    // but checked for the same reason BuildRework checks it: never record
    // a no-op.
    if (!isMerge && !guidsChanged && !nameChanged && !categoryChanged)
        return false;

    outRework.oldName         = oldName;
    outRework.newName         = newName;
    outRework.oldGuids        = ExtractGuids(*survivor); // identity key -- see struct comment
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

    return true;
}

// Walks newCategory (read-only) and appends every effect's disposition to
// `plan`, without touching oldFile at all. idx reflects oldFile's state
// from BEFORE this resolve started; a new effect never gets compared
// against another new effect from the same pass, which keeps this step
// order-independent and side-effect-free.
//
// Matching is guid-first, name-fallback -- reversed from this file's
// original design, where name was checked first and guid only used to
// refine a same-name match. That original order silently mismatched
// whenever multiple old effects shared a name (idx.effectsByName used to
// be a single pointer per name, so only one arbitrary same-named effect
// ever got refreshed or displayed as changing, even when several existed).
// Guids are globally unique, so checking them first removes all ambiguity
// wherever a guid actually matches; name is only consulted once guid
// matching has ruled out anything more precise (case 2):
//
//   1. This new effect has at least one guid claimed by oldFile
//      (FindAllByGuid checks every one of its guids, not just the first
//      that hits, since a new effect's guid list can in principle straddle
//      more than one old effect -- see FindAllByGuid's own comment).
//      However many distinct old effects that touches, they all get
//      folded into one resulting entry -- upstream's name/category always
//      wins once guid identity is certain:
//        a. Every matched guid points to the SAME old effect, and name
//           also matches -> plain rework, guid diff only (BuildRework
//           decides skip / add-only / replace); name/category untouched,
//           since there's nothing upstream actually changed here.
//        b. Every matched guid points to the SAME old effect, but name
//           differs -> same guid-diff logic, but name/category ARE
//           overwritten from the update (BuildMergedRework, single
//           candidate) -- guid identity says this is the same underlying
//           effect, and upstream's new name/location is now treated as
//           authoritative.
//        c. Matched guids point to MORE THAN ONE old effect (e.g. an
//           upstream merge of two previously-separate effects into one) --
//           always folded together now, regardless of name
//           (BuildMergedRework, multiple candidates): the first-matched
//           candidate survives (gets 1b's treatment, guid diff run against
//           the union of every matched candidate's guids), every other
//           matched candidate is recorded for deletion, and a behaviors
//           disagreement between them is flagged as a conflict.
//   2. No guid overlap at all -> fall back to name:
//        - exactly one old effect shares the name -> rework it (this is
//          the "full guid refresh under an unchanged name" case -- there's
//          no guid overlap by definition, so BuildRework's comparison
//          against that one candidate's old list always reads as "added
//          everything, removed everything," landing on add-only or
//          replace per its own size check).
//        - no old effect shares the name -> genuinely new, insert as-is.
//        - MULTIPLE old effects share the name -> ambiguous: no guid
//          signal is available to say which one this corresponds to, so
//          rather than guess (and silently overwrite/mislabel one of
//          several genuinely distinct same-named effects), insert this as
//          a new effect alongside the existing ones. Nothing existing is
//          touched.
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
                continue; // malformed entry in the new file -- skip rather than guess

            // Guid-less effect (no guids field, non-array, or empty array):
            // skip entirely. It can't be matched reliably against oldFile,
            // and if the name-match fallback were allowed here instead,
            // two guid-less effects sharing a name upstream would silently
            // collide.
            if (!HasAnyGuid(newEffect))
                continue;

            const std::string name = newEffect["name"].get<std::string>();

            std::vector<json*> guidMatches = FindAllByGuid(newEffect, idx);

            if (guidMatches.size() == 1)
            {
                // Step 1: guid identifies exactly one old effect.
                json* guidMatch = guidMatches[0];
                const bool nameMatches =
                    guidMatch->contains("name") && guidMatch->value("name", std::string()) == name;

                if (nameMatches)
                {
                    // 1a: identity and name both already agree with the
                    // update -- nothing to reconcile, just refresh guids.
                    MergePlanRework rework;
                    if (BuildRework(*guidMatch, newEffect, name, rework))
                        plan.reworks.push_back(std::move(rework));
                }
                else
                {
                    // 1b: same guid-diff logic as 1a, but name/category
                    // are now overwritten from the update rather than
                    // left alone.
                    MergePlanRework rework;
                    if (BuildMergedRework({ guidMatch }, newEffect, name, pathSoFar, idx, rework))
                        plan.reworks.push_back(std::move(rework));
                }
                continue;
            }

            if (guidMatches.size() > 1)
            {
                // 1c: this new effect's guids are spread across more than
                // one old effect (e.g. an upstream merge of two
                // previously-separate effects into one) -- always folded
                // into a single entry now, regardless of name. See
                // BuildMergedRework for how the survivor is picked and how
                // the other matched candidates are recorded for deletion.
                MergePlanRework rework;
                if (BuildMergedRework(guidMatches, newEffect, name, pathSoFar, idx, rework))
                    plan.reworks.push_back(std::move(rework));
                continue;
            }

            // Step 2: no guid overlap anywhere in oldFile -- fall back to name.
            auto range = idx.effectsByName.equal_range(name);
            const size_t candidateCount = static_cast<size_t>(std::distance(range.first, range.second));

            if (candidateCount == 1)
            {
                // Exactly one same-named old effect: the "full guid
                // refresh under an unchanged name" case this fallback
                // exists for.
                MergePlanRework rework;
                if (BuildRework(*range.first->second, newEffect, name, rework))
                    plan.reworks.push_back(std::move(rework));
            }
            else
            {
                // candidateCount == 0 (genuinely new) or > 1 (ambiguous --
                // several old effects share this name and there's no guid
                // signal to pick between them). Either way: insert as a
                // new effect, verbatim, touching nothing that already
                // exists. Keep the full effect body so ApplyMergePlan can
                // insert it without needing newFile again.
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

// Recursively tallies how many times each guid string appears across every
// effect under `category`, accumulating into `counts`. A guid belonging to
// two effects increments the same key twice -- deliberately not de-duped
// per-effect, since a single effect listing the same guid twice in its own
// array is a different (and much less concerning) kind of oddity than two
// distinct effects sharing one; this function only needs to answer "does
// any guid appear more than once file-wide," so it doesn't need to
// distinguish those two shapes.
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

// Address-based removal, single pass: strips out any effect anywhere under
// `category` whose address appears in `toRemove`. Only ever called once,
// against addresses captured from a still-fully-valid `OldIndex` (see the
// phase-ordering comment in ApplyMergePlan below) -- an erase mid-array
// shifts every later element's address, so comparing against addresses
// captured after an earlier erase in the same array would silently match
// the wrong effect.
void RemoveEffectsRecursive(json& category, const std::unordered_set<const json*>& toRemove)
{
    if (category.contains("effects") && category["effects"].is_array())
    {
        // Back-to-front, by index, deliberately -- NOT the forward
        // begin()/erase(it)-returns-next pattern this used to be. Erasing
        // index i shifts every element AFTER i down one slot, reusing the
        // memory address that used to belong to the erased element. A
        // forward pass re-checks toRemove against `&(*it)` on that very
        // next iteration, right after the shift -- so the element that
        // just slid into the freed slot spuriously matches the STALE
        // address of the effect that used to live there, gets erased too,
        // and the next one slides into that same slot and repeats: one
        // legitimate removal cascades into wiping out every effect after
        // it in the array. Going back-to-front avoids this entirely --
        // erasing index i only ever shifts indices > i, all of which this
        // loop has already finished with, so every not-yet-visited index
        // (< i) keeps its original address for the rest of the pass.
        auto& effects = category["effects"];
        for (size_t i = effects.size(); i-- > 0; )
            if (toRemove.count(&effects[i]))
                effects.erase(effects.begin() + i);
    }

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
            RemoveEffectsRecursive(sub, toRemove);
}

// Finds, or creates and appends, the child category of oldParent whose name
// matches `name`. Used while applying inserts (after every guid refresh
// has already been applied) and while relocating a 1b/1c rework to its
// new category.
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

// Second pass over the fully-resolved plan, run once ResolvePlan has
// finished walking every new effect. A rework's `oldGuids` is the identity
// key of an old effect that has its OWN independent, legitimate fate in
// this plan (a rename/renumber, cases 1a/1b/2a, or the survivor side of a
// 1c). A DIFFERENT rework's `mergedAwayGuids` can end up naming that same
// guid when that old effect's guid gets pulled into some unrelated new
// effect's 1c merge elsewhere (e.g. a guid that used to uniquely identify
// one effect gets folded into another effect's guid list upstream, while
// the original effect separately picks up a brand-new guid of its own and
// is only found again via the name fallback). Both outcomes can't be
// right at once: the old effect isn't a redundant duplicate to delete,
// it's alive under its own rework -- so any mergedAwayGuids entry that
// collides with another rework's identity key is dropped here, before
// ApplyMergePlan ever sees it, rather than let deletion silently destroy
// an object another rework in the very same plan just updated.
//
// Deliberately guid-based, not pointer-based: this runs entirely over
// MergePlan (plain strings), never touching oldFile/newFile json objects,
// so there's no address to go stale -- unlike ApplyMergePlan's own
// OldIndex, which is built fresh right before use specifically to avoid
// that trap (see its own comment).
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

    // oldFile is untouched here -- IndexCategory only ever reads through
    // its json& parameter in this call path (the non-const overload exists
    // so the same function can also hand out mutable pointers in
    // ApplyMergePlan below; ResolveMergePlan just never dereferences those
    // pointers for writing).
    OldIndex idx;
    for (auto& cat : const_cast<json&>(oldFile)["categories"])
    {
        if (!cat.contains("name") || !cat["name"].is_string())
            continue; // malformed top-level category -- no name to build a path with, skip it
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

void ApplyMergePlan(json& oldFile, const MergePlan& plan)
{
    if (!oldFile.contains("categories") || !oldFile["categories"].is_array())
        return;

    // Built once, up front -- O(effects) -- rather than a fresh linear
    // scan per lookup (O(reworks x effects), the shape the original
    // per-rework FindEffectLocationInFile version had). At the file sizes
    // this addon deals with (thousands of effects) that only matters when
    // a single update reworks a large fraction of the file at once, but at
    // that point it was worth the fixed cost of doing this properly:
    //   - Phase 1 mutates every survivor's own "name"/"guids" fields IN
    //     PLACE and works out (without touching anything yet) which
    //     effects need to disappear from wherever they currently sit --
    //     every rework's merged-away effects, plus a moving survivor's own
    //     old slot. Field assignment never changes an "effects" array's
    //     size, so every pointer `idx` handed out stays valid through this
    //     entire phase no matter how many reworks run.
    //   - Phase 2 is a SINGLE removal pass over the whole tree, using the
    //     addresses captured in phase 1 -- still valid, since nothing has
    //     resized any "effects" array yet. One pass (rather than one erase
    //     per effect) means a moving survivor and a same-array merged-away
    //     effect can never shift each other's address out from under a
    //     still-pending removal.
    //   - Phase 3 re-inserts every moved survivor at its new location,
    //     creating categories as needed. Only now, since these push_backs
    //     can reallocate/resize arrays -- doing it any earlier would have
    //     invalidated pointers phases 1/2 still needed.
    //   - Phase 4 applies every new-effect insertion, same as always.
    //
    // Looked up by GUID, not name: rework.oldGuids/mergedAwayGuids are the
    // exact guid lists the matched old effects had at resolve time, and
    // guids are globally unique, so any one of them pins down the specific
    // node unambiguously -- even when several old effects share a name.
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
            continue; // oldFile changed since the plan was resolved -- skip rather than guess

        json guids = json::array();
        for (const auto& g : rw.newGuids)
            guids.push_back(g);
        (*survivor)["guids"] = std::move(guids);
        (*survivor)["name"]  = rw.newName; // no-op for 1a/2a, where newName always equals the current name

        for (const auto& loserGuid : rw.mergedAwayGuids)
        {
            auto it = idx.guidToEffect.find(loserGuid);
            if (it != idx.guidToEffect.end() && it->second != survivor)
                toRemove.insert(it->second);
        }

        // Relocate to the update's category, but only when it actually
        // differs from where the survivor currently sits -- 1a/2a leave
        // both sides empty (see BuildRework), so this is always skipped
        // for those.
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

    // --- Phase 4: apply every new-effect insertion, same as before. ---
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
        return out; // malformed file -- nothing to report here, see FindDuplicateGuids' own doc comment

    std::unordered_map<std::string, int> counts;
    for (const auto& cat : file["categories"])
        CountGuidsRecursive(cat, counts);

    for (const auto& [guid, count] : counts)
        if (count > 1)
            out.push_back(guid);

    return out;
}