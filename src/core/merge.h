//################################################################################
// merge.h
//--------------------------------------------------------------------------------
// MergePlanNewEffect      a new effect (case 2b) to insert, and where
// MergePlanMergeCandidate a losing candidate folded into a case-1c rework
// MergePlanRework         an existing effect whose guids/name/category update
// MergePlan               the full resolve result: inserts + reworks
// ResolveMergePlan()      walks oldFile/newFile, returns the plan (read-only)
// ApplyMergePlan()        applies a resolved plan to oldFile in place
// FindDuplicateGuids()    data-integrity check: guids reused across effects
//--------------------------------------------------------------------------------
// Declares the result of matching a freshly-downloaded VfxDenoiser effect
// file (newFile) against the user's existing one (oldFile), and the calls
// that produce and apply that match. For every effect in newFile, walking
// every category recursively:
//
//   0. No guids at all -> ignore entirely. Nothing to match reliably
//      against a future release, and falling back to name would let two
//      guid-less same-named effects collide. Never shown in a diff.
//
// Matching is guid-first, name-fallback: guids are globally unique, so any
// guid overlap unambiguously identifies the old effect(s) involved.
//
//   1. At least one guid is claimed in oldFile (every guid on the new
//      effect is checked, since its list can straddle more than one old
//      effect -- an upstream merge of two effects into one). All matched
//      old effects fold into one resulting entry; upstream's name/category
//      always wins once guid identity is certain:
//        a. All matched guids -> same old effect, name also matches ->
//           rework: guids refreshed, and relocated if upstream also moved
//           its category (name is unchanged by definition here).
//        b. All matched guids -> same old effect, name differs -> rework,
//           but name/category ARE overwritten from the update.
//        c. Matched guids split across MORE THAN ONE old effect -> merged
//           into one entry regardless of name (1b's treatment against the
//           union of every candidate's guids); every other candidate is
//           deleted, and a behaviors mismatch between them is flagged as
//           a conflict (display-only, never blocks applying).
//   2. No guid overlap anywhere -> fall back to name, skipping any old
//      effect a case-1 match already claimed elsewhere (guards a vacated
//      name/category slot from colliding with an unrelated new effect):
//        a. Exactly one unclaimed old effect shares the name -> rework it
//           (a full guid refresh under an unchanged name, relocated if
//           upstream also moved its category).
//        b. No unclaimed old effect shares the name -> genuinely new;
//           inserted into whatever category newFile puts it in.
//        c. MULTIPLE unclaimed old effects share the name -> ambiguous, no
//           guid signal to pick between them -> inserted as a new effect
//           alongside the existing ones; nothing existing is touched.
//
// An old effect newFile never mentions under any shared guid or name is
// left exactly as-is -- user-added or ArenaNet-removed, either way untouched.
//--------------------------------------------------------------------------------

#pragma once

#include "nlohmann_json.hpp"

#include <string>
#include <unordered_map>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// kCategoryPathKeySep
//--------------------------------------------------------------------------------
// Joins a category path into a single map key for
// MergePlan::newCategoryDescriptions. \x1f (ASCII "unit separator") rather
// than "/" or " / ": those are valid, observed characters inside real
// category names (see e.g. "Combos/AoEs" in Skill Effects), so joining with
// either risks two different paths colliding on the same key. \x1f never
// appears in a category name in practice and isn't typable through the
// tree-view rename UI, so it's safe as a delimiter here without needing a
// custom vector<string> hasher.
//--------------------------------------------------------------------------------
inline const char kCategoryPathKeySep = '\x1f';

inline std::string JoinCategoryPathKey(const std::vector<std::string>& path)
{
    std::string out;
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (i) out += kCategoryPathKeySep;
        out += path[i];
    }
    return out;
}

//********************************************************************************
// MergePlanNewEffect
//--------------------------------------------------------------------------------
// categoryPath   root -> immediate parent, in order
// name           pulled out of `effect` so a diff view can display it directly
// effect         full effect object from newFile, inserted verbatim
//--------------------------------------------------------------------------------
// A brand-new effect (case 2b) to insert. Carries the whole effect body so
// ApplyMergePlan can insert it without needing newFile again.
//--------------------------------------------------------------------------------
struct MergePlanNewEffect
{
    std::vector<std::string> categoryPath;
    std::string               name;
    nlohmann::ordered_json             effect;
};

//********************************************************************************
// MergePlanMergeCandidate
//--------------------------------------------------------------------------------
// name          the losing candidate's own name at resolve time
// categoryPath  where it lived, doubles as "go look here" if it survives
//               under its own separate rework instead of being deleted
// behaviors     its behaviors array at resolve time (empty if none)
//--------------------------------------------------------------------------------
// One matched candidate other than the survivor from a case-1c merge,
// captured only when MergePlanRework::behaviorsConflict ends up true --
// this is what "review before applying" asks the user to check.
//--------------------------------------------------------------------------------
struct MergePlanMergeCandidate
{
    std::string               name;
    std::vector<std::string>  categoryPath;
    nlohmann::ordered_json    behaviors;
};

//********************************************************************************
// MergePlanRework
//--------------------------------------------------------------------------------
// oldName/newName    always populated, even when equal (1a/2a)
// oldGuids           survivor's guid list at resolve time; doubles as the
//                    identity key ApplyMergePlan looks up by
// newGuids           final guid list to write
// oldCategoryPath/   always populated, even when equal; oldCategoryPath is
// newCategoryPath    empty only if resolving it failed
// mergedAwayGuids    one representative guid per other old effect folded
//                    away (case 1c only, else empty)
// behaviorsConflict  true only for a 1c merge with disagreeing settings
// otherCandidates    every matched candidate but the survivor; empty
//                    unless behaviorsConflict is true
//--------------------------------------------------------------------------------
// An existing effect (case 1a/1b/1c/2a) whose guids -- and for 1b/1c, name/
// category -- would be updated. Looked up by guid rather than name, since
// guids are globally unique and names are not -- the exact ambiguity this
// design exists to avoid.
//--------------------------------------------------------------------------------
struct MergePlanRework
{
    std::string              oldName;
    std::string              newName;
    std::vector<std::string> oldGuids;
    std::vector<std::string> newGuids;
    std::vector<std::string> oldCategoryPath;
    std::vector<std::string> newCategoryPath;
    std::vector<std::string> mergedAwayGuids;
    bool                      behaviorsConflict = false;
    std::vector<MergePlanMergeCandidate> otherCandidates;
};

//********************************************************************************
// MergePlan
//--------------------------------------------------------------------------------
// inserts                  every case-2b new effect to add
// reworks                  every case-1/2a existing effect to update
// newCategoryDescriptions  every category in newFile that has a non-empty
//                          "description", keyed by JoinCategoryPathKey(path)
//--------------------------------------------------------------------------------
// The full, human-displayable result of resolving newFile against oldFile.
// Contains nothing for case-0 (guid-less, ignored) effects by design.
//
// newCategoryDescriptions exists purely so a category ApplyMergePlan/
// BuildDiffOverlayTree has to freshly create (an insert or a relocation
// landing somewhere that doesn't exist in oldFile/installed yet) can be
// seeded with upstream's own description instead of coming out
// name-only. It is intentionally NOT consulted for a category that
// already exists on the old/installed side -- same "don't clobber a
// locally-touched node just because upstream also set something" rule
// BuildRework/BuildMergedRework already apply to effect fields; an
// already-existing category's description is left exactly as the user
// has it, blank or not. See FindOrCreateCategory (merge.cpp) and
// FindOrCreateDiffCategory (installed_tree_overlay.cpp).
//--------------------------------------------------------------------------------
struct MergePlan
{
    std::vector<MergePlanNewEffect> inserts;
    std::vector<MergePlanRework>    reworks;
    std::unordered_map<std::string, std::string> newCategoryDescriptions;

    bool IsEmpty() const { return inserts.empty() && reworks.empty(); }
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ResolveMergePlan
//--------------------------------------------------------------------------------
// Read-only: walks oldFile/newFile per the rules above and returns the
// plan. Sets outOk to false (plan returned empty) if either file is
// missing/malformed a top-level "categories" array.
//--------------------------------------------------------------------------------
MergePlan ResolveMergePlan(const nlohmann::ordered_json& oldFile, const nlohmann::ordered_json& newFile, bool& outOk);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ApplyMergePlan
//--------------------------------------------------------------------------------
// Applies a previously-resolved plan to oldFile in place: refreshes every
// rework's guids (and, for 1a/1b/1c/2a, its category, relocating it if
// needed; 1b/1c also update its name), deletes every merged-away
// duplicate, inserts every new effect, then prunes any subcategory branch
// left fully empty by a relocation. Must run against the same oldFile the
// plan was resolved against -- see merge.cpp for why a stale oldFile is
// unsafe.
//--------------------------------------------------------------------------------
void ApplyMergePlan(nlohmann::ordered_json& oldFile, const MergePlan& plan);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindDuplicateGuids
//--------------------------------------------------------------------------------
// Standalone data-integrity check on a single file, unrelated to whether
// an update is available: every rule above leans on guids never repeating
// within a file, a premise confirmed for how ArenaNet ships these files but
// not enforced against a hand-edited or third-party-modified one. Returns
// every guid appearing on more than one effect (each listed once); an
// empty result means the premise holds. Malformed input (missing/non-array
// "categories") reads as "no duplicates found," not an error -- callers
// needing to know a file is malformed already have other checks for that.
// Read-only -- never mutates `file`.
//--------------------------------------------------------------------------------
std::vector<std::string> FindDuplicateGuids(const nlohmann::ordered_json& file);