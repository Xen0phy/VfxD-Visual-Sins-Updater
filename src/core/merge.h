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
// Declares the result of matching a freshly-downloaded VfxDenoiser effect file
// (newFile) against the user's existing one (oldFile), and the calls that
// produce and apply that match. Matching is guid-first, name-fallback; a guid-
// less effect is ignored outright, since name-fallback risks colliding two
// guid-less same-named effects. A guid claimed in oldFile settles identity and
// upstream's name/category always win: one matched old effect keeps its name
// and gets a guid refresh; a differently-named match also has its name/category
// overwritten; guids split across several old effects merge into one entry,
// deleting the rest and flagging a behaviors disagreement for review. With no
// guid overlap, matching falls back to name among old effects case 1 hasn't
// claimed: one unclaimed match is reworked under its unchanged name; none, or
// more than one with no guid signal to disambiguate, inserts the effect as new.
// An untouched old effect newFile never mentions is left as-is.
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
// MergePlan::newCategoryDescriptions, using \x1f (ASCII "unit separator")
// instead of "/" or " / ": those are valid, observed characters inside real
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
// One matched candidate other than the survivor from a case-1c merge, captured
// only when MergePlanRework::behaviorsConflict ends up true -- this is what
// "review before applying" asks the user to check.
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
// oldGuids           survivor's guids at resolve time; the identity key
// newGuids           final guid list to write
// oldCategoryPath/   always populated, even when equal; oldCategoryPath
// newCategoryPath    empty only if resolving it failed
// mergedAwayGuids    one guid per other old effect folded away (1c only)
// behaviorsConflict  true only for a 1c merge with disagreeing settings
// otherCandidates    every matched candidate but survivor (conflict only)
//--------------------------------------------------------------------------------
// An existing effect (1a/1b/1c/2a) whose guids update, and for 1b/1c
// name/category too. Looked up by guid, not name -- guids are unique.
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
// The full, human-displayable result of resolving newFile against oldFile;
// nothing is recorded for case-0 (guid-less) effects. newCategoryDescriptions
// seeds a freshly-created category -- an insert, or a relocation into a new
// path -- with upstream's own description instead of leaving it name-only; an
// already-existing category's description is left untouched. See
// FindOrCreateCategory (merge.cpp).
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
// Read-only: walks oldFile/newFile per the rules above and returns the plan.
// Sets outOk to false (plan returned empty) if either file is missing/malformed
// a top-level "categories" array.
//--------------------------------------------------------------------------------
MergePlan ResolveMergePlan(const nlohmann::ordered_json& oldFile, const nlohmann::ordered_json& newFile, bool& outOk);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ApplyMergePlan
//--------------------------------------------------------------------------------
// Applies a previously-resolved plan to oldFile in place: refreshes every
// rework's guids (and, for 1a/1b/1c/2a, its category, relocating it if needed;
// 1b/1c also update its name), deletes every merged-away duplicate, inserts
// every new effect, then prunes any subcategory branch left fully empty by a
// relocation. Must run against the same oldFile the plan was resolved against
// -- see merge.cpp for why a stale oldFile is unsafe.
//--------------------------------------------------------------------------------
void ApplyMergePlan(nlohmann::ordered_json& oldFile, const MergePlan& plan);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindDuplicateGuids
//--------------------------------------------------------------------------------
// Standalone data-integrity check on a single file, unrelated to whether an
// update is available: every rule above leans on guids never repeating within a
// file, a premise confirmed for how ArenaNet ships these files but not enforced
// against a hand-edited or third-party-modified one. Returns every guid
// appearing on more than one effect (each listed once); an empty result means
// the premise holds. Malformed input (missing/non-array "categories") reads as
// "no duplicates found," not an error -- callers needing to know a file is
// malformed already have other checks for that. Read-only -- never mutates
// `file`.
//--------------------------------------------------------------------------------
std::vector<std::string> FindDuplicateGuids(const nlohmann::ordered_json& file);