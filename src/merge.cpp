#include "merge.h"
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
    std::unordered_multimap<std::string, json*> effectsByName;
};

void IndexCategory(json& category, OldIndex& idx)
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
                        idx.guidToEffect[g.get<std::string>()] = &effect;
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
            IndexCategory(sub, idx);
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

    outRework.name     = name;
    outRework.oldGuids = oldGuids;
    outRework.newGuids = finalGuids;
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
// matching has ruled out anything more precise:
//
//   1. This new effect has at least one guid claimed by oldFile
//      (FindAllByGuid checks every one of its guids, not just the first
//      that hits, since a new effect's guid list can in principle straddle
//      more than one old effect -- see FindAllByGuid's own comment):
//        a. Every matched guid points to the SAME old effect:
//             - name also matches that same old effect -> rework it
//               (BuildRework decides skip / add-only / replace).
//             - name differs -> skip entirely. Guid identity says this is
//               the same underlying effect, just under upstream's own new
//               name -- but the user may have renamed it themselves, and
//               their naming is left alone rather than guessed at.
//        b. Matched guids point to MORE THAN ONE old effect (e.g. an
//           upstream merge of two previously-separate effects into one) --
//           name is the tie-breaker: exactly one of the matched candidates
//           sharing this new effect's name -> rework that one (the other
//           candidate's guid(s) are simply "added" from its perspective,
//           same as any other guid it didn't have before). Zero or more
//           than one matched candidate sharing the name -> no way to pick
//           without guessing, so touch nothing.
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

                if (!nameMatches)
                    continue; // guid known, name differs -- assume a user rename, leave it alone

                MergePlanRework rework;
                if (BuildRework(*guidMatch, newEffect, name, rework))
                    plan.reworks.push_back(std::move(rework));
                continue;
            }

            if (guidMatches.size() > 1)
            {
                // Step 1, split case: this new effect's guids are spread
                // across more than one old effect (e.g. an upstream merge
                // of two previously-separate effects into one). Guid
                // overlap alone can no longer say which of these is "the"
                // continuation -- so use the name as the tie-breaker,
                // exactly like the ordinary guid match above: if exactly
                // one of the matched old effects shares this new effect's
                // name, that's the one being continued. Its own guid list
                // vs. newEffect's is compared by BuildRework as always, so
                // whichever guid(s) belonged to the *other* matched
                // effect(s) are simply added-not-removed, same as any other
                // guid this effect didn't have before -- the merge lands
                // correctly even though only some of newEffect's guids
                // were already on the confirmed target.
                //
                // If no matched candidate shares the name, or more than one
                // does, there's no way to pick without guessing -- leave
                // every matched old effect untouched rather than risk
                // reworking (or duplicating) the wrong one.
                json* nameConfirmed = nullptr;
                int nameMatchCount = 0;
                for (json* candidate : guidMatches)
                {
                    if (candidate->contains("name") && candidate->value("name", std::string()) == name)
                    {
                        nameConfirmed = candidate;
                        ++nameMatchCount;
                    }
                }

                if (nameMatchCount == 1)
                {
                    MergePlanRework rework;
                    if (BuildRework(*nameConfirmed, newEffect, name, rework))
                        plan.reworks.push_back(std::move(rework));
                }
                continue; // nameMatchCount == 0 or > 1: ambiguous, touch nothing
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

// Finds, or creates and appends, the child category of oldParent whose name
// matches `name`. Only used while applying inserts, after every guid
// refresh has already been applied.
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
        IndexCategory(cat, idx);

    for (const auto& newTop : newFile["categories"])
    {
        if (!newTop.contains("name") || !newTop["name"].is_string())
            continue;
        std::vector<std::string> path{ newTop["name"].get<std::string>() };
        ResolvePlan(newTop, path, idx, plan);
    }

    outOk = true;
    return plan;
}

void ApplyMergePlan(json& oldFile, const MergePlan& plan)
{
    if (!oldFile.contains("categories") || !oldFile["categories"].is_array())
        return;

    // Every rework target is looked up fresh, right here, rather than
    // carrying a pointer inside MergePlan -- MergePlan needs to be safely
    // copyable and long-lived (it sits in a cache between "user viewed the
    // diff" and "user clicked Apply"), and a raw json* would go stale the
    // moment anything reallocates oldFile in between.
    //
    // Looked up by GUID, not name: rework.oldGuids is the exact guid list
    // the matched old effect had at resolve time, and guids are globally
    // unique, so any one of them pins down the specific node unambiguously
    // -- even when several old effects share the rework's name. Looking
    // this up by name instead (as before) would hit the same collision
    // TagReworkEffect had on the display side: every same-named effect
    // looking like a valid target instead of just the one actually meant.
    OldIndex idx;
    for (auto& cat : oldFile["categories"])
        IndexCategory(cat, idx);

    // --- Phase 1: apply every GUID refresh. In-place writes to existing
    //     map entries -- never grows a vector, so every pointer in `idx`
    //     stays valid no matter how many of these run. ---
    for (const auto& rework : plan.reworks)
    {
        json* target = nullptr;
        for (const auto& g : rework.oldGuids)
        {
            auto it = idx.guidToEffect.find(g);
            if (it != idx.guidToEffect.end()) { target = it->second; break; }
        }
        if (!target)
            continue; // oldFile changed since the plan was resolved -- skip rather than guess

        json guids = json::array();
        for (const auto& g : rework.newGuids)
            guids.push_back(g);
        (*target)["guids"] = std::move(guids);
    }

    // --- Phase 2: apply every new-effect insertion. Only now, since these
    //     push_backs can reallocate category "effects"/"categories" arrays
    //     and would have invalidated any pointer taken before this point. ---
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
