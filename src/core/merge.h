#pragma once
#include "nlohmann_json.hpp"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Computes and (on request) applies a merge of a freshly-downloaded
// VfxDenoiser effect file (`newFile`) into the user's existing,
// already-configured one (`oldFile`), per the rules worked out for this
// project. For every effect in newFile, walking every category recursively:
//
//   0. If the effect has no guids at all (missing "guids" field, non-array,
//      or an empty array) -> ignore it entirely. It can never be tracked
//      reliably: there's nothing to match a future release's guids
//      against, and if it fell through to a name match instead, two
//      guid-less effects sharing a name upstream would silently collide.
//      Never appears in the plan, never shown in a diff view.
//
// Matching is guid-first, name-fallback. Guids are globally unique (a guid
// never belongs to more than one effect in oldFile), so any guid overlap
// unambiguously identifies the specific old effect(s) involved; name is only
// consulted once guid matching has found nothing (see case 2), or to decide
// whether an unambiguous single guid match (case 1) also counts as an
// unchanged upstream identity.
//
//   1. This new effect has at least one guid claimed somewhere in oldFile
//      (every one of its guids is checked, not just the first that hits
//      something -- a new effect's guid list can in principle straddle
//      more than one old effect, e.g. an upstream merge of two
//      previously-separate effects into one). However many distinct old
//      effects that guid set touches, they all get folded into one
//      resulting entry -- upstream's name/category always wins once guid
//      identity is certain, the only question is how many old effects are
//      being folded together:
//        a. Every matched guid points to the SAME old effect, and name
//           also matches that same old effect -> an ArenaNet skill-effect
//           rework with nothing upstream actually changed about its
//           identity. Keep oldFile's name, category location, and
//           behaviors (settings) exactly as they are -- name/category are
//           NOT touched in this specific case, since there's nothing to
//           reconcile. Guids are updated by comparing this effect's own
//           old guid list against its new one (never a whole-file index):
//             - nothing in the new list is actually new (old already
//               covers it) -> not recorded, nothing to do
//             - new list adds guids without dropping any old one, or the
//               change is mixed but the guid counts don't match -> old
//               guids kept, new ones appended (never silently drop a guid
//               unless the next case's signal is unambiguous)
//             - new list has the same guid count as old but neither fully
//               contains the other -> reads as a clean upstream renumber;
//               recorded as a full guid-list replacement
//        b. Every matched guid points to the same single old effect, but
//           name differs -> reworked same as 1a (guid-diff logic
//           unchanged), but this time name and category ARE overwritten
//           from the update: guid identity says this is the same
//           underlying effect under upstream's new name/location, and
//           upstream is now treated as authoritative rather than assuming
//           the user renamed it. Marked as a rework in the plan either
//           way; only whether name/category also come along for the ride
//           differs from 1a.
//        c. Matched guids are split across MORE THAN ONE old effect (an
//           upstream merge of previously-separate effects into one) ->
//           always merged now, regardless of name: the first-matched old
//           effect (in the order this new effect's own guid list matches
//           them) is the survivor and gets 1b's treatment -- guid diff run
//           against the union of every matched candidate's guids (so a
//           guid unique to a losing candidate still counts as already-
//           known, never silently dropped), name/category overwritten from
//           the update. Every other matched candidate is deleted outright
//           once the merge is applied -- their guids now live on the
//           survivor. If the matched candidates' behaviors (settings)
//           don't all agree, the result is flagged as a conflict --
//           display-only, never blocks applying -- so the user knows to
//           double check the surviving settings.
//   2. No guid overlap anywhere in oldFile -> fall back to name:
//        a. Exactly one old effect shares the name -> rework it, same
//           guid-comparison logic as 1a (this is the "full guid refresh
//           under an unchanged name" case -- no guid overlap by
//           definition, so the comparison always resolves to add-only or
//           full replace, never a same-list skip).
//        b. No old effect shares the name -> genuinely new. Recorded as a
//           NewEffect entry, to be inserted into whatever category newFile
//           puts it in (creating that category in oldFile if needed).
//        c. MULTIPLE old effects share the name -> ambiguous: no guid
//           signal is available to say which one this corresponds to, so
//           rather than guess (and silently overwrite or mislabel one of
//           several genuinely distinct same-named effects), this is
//           inserted as a new effect alongside the existing ones. Nothing
//           existing is touched.
//
// Any effect that exists in oldFile but was never matched by the above --
// i.e. newFile doesn't mention it under any shared guid or name -- is left
// exactly as-is. It may be an effect the user added themselves, or one
// ArenaNet removed; either way this is never touched or shown as a change.
// ---------------------------------------------------------------------------

// A brand-new effect (case 3) that would be inserted, and where.
// `effect` carries the full effect object from newFile (name, guids,
// default behaviors, everything) so ApplyMergePlan can insert it verbatim
// without needing newFile again; `name`/`categoryPath` are pulled out
// alongside it purely so a diff view can display them without reaching
// into `effect`'s json shape.
struct MergePlanNewEffect
{
    std::vector<std::string> categoryPath; // root -> immediate parent, in order
    std::string               name;
    nlohmann::ordered_json             effect;
};

// An existing effect (case 1a/1b/1c/2a) whose GUIDs would be updated, and
// -- for 1b/1c only -- whose name/category would also be overwritten from
// the update. `oldGuids` is the survivor's exact guid list *at resolve
// time* -- it doubles as this rework's identity key: since guids are
// globally unique, ApplyMergePlan (and any display code overlaying this
// onto a tree) looks up the target by checking which old effect owns one
// of these guids, not by name. Looking up by name instead would
// reintroduce the exact bug this design fixes -- multiple old effects can
// share a name, so a name-based lookup can't tell them apart, but a
// guid-based one always can. `newGuids` is the *final* guid list to write
// -- either newFile's raw list verbatim (a clean same-count renumber) or
// the old guids (unioned across every merged candidate, for 1c) plus
// whatever's newly added (an add-only change).
//
// `oldName`/`newName` and `oldCategoryPath`/`newCategoryPath` are always
// populated (even when they're equal -- 1a/2a set both sides to the same
// value rather than leaving either blank, since "no rename/move" is just
// as valid a fact to record as a change). `oldCategoryPath` is empty only
// when this rework's path couldn't be resolved (shouldn't happen against a
// well-formed oldFile); a legitimate path is never empty, since even a
// top-level category contributes at least its own name.
//
// `mergedAwayGuids` holds one representative guid per OTHER old effect
// this rework's guids matched (case 1c only -- empty for 1a/1b/2a): each
// entry is enough to find and delete that duplicate candidate outright
// when the plan is applied, since guids are globally unique. Their own
// guids are already folded into `newGuids` above, so nothing is lost by
// removing the now-redundant JSON objects themselves.
//
// `behaviorsConflict` is only ever true for a 1c merge (never a plain 1a/
// 1b/2a rework, which by definition has just the one candidate and
// nothing else to disagree with it): true means the matched candidates'
// behaviors (settings) didn't all agree, so the survivor keeps its own
// but the user should double-check them. Purely informational -- never
// blocks ApplyMergePlan.
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
};

// The full, human-displayable result of resolving newFile against oldFile.
// Deliberately contains nothing for case-1 (ignored) effects -- there is
// nothing to show for those, by design.
struct MergePlan
{
    std::vector<MergePlanNewEffect> inserts;
    std::vector<MergePlanRework>    reworks;

    bool IsEmpty() const { return inserts.empty() && reworks.empty(); }
};

// Read-only: walks oldFile and newFile and returns the plan described above.
// Neither argument is modified. Sets outOk to false (returned plan is empty)
// if either input isn't a well-formed VfxDenoiser effect file (missing/
// malformed top-level "categories" array).
MergePlan ResolveMergePlan(const nlohmann::ordered_json& oldFile, const nlohmann::ordered_json& newFile, bool& outOk);

// Applies a previously-resolved plan to oldFile in place: refreshes GUIDs
// for every rework (and, where the rework's name/category actually differ
// from the update -- 1b/1c, never 1a/2a -- overwrites those and physically
// relocates the effect to its new category, creating that category if it
// doesn't exist yet), deletes every merged-away duplicate a 1c rework
// folded away, then inserts every new effect (creating categories as
// needed). Must be called with the same oldFile the plan was resolved
// against -- see the note in merge.cpp on why interleaving resolve/apply
// across a stale oldFile is unsafe.
void ApplyMergePlan(nlohmann::ordered_json& oldFile, const MergePlan& plan);

// ---------------------------------------------------------------------------
// Every rule above -- guid-first matching in particular -- leans on one
// premise: a guid never belongs to more than one effect within a single
// file. That premise is confirmed for how ArenaNet/Xen0phy actually ship
// these files, but nothing stops a hand-edit (or a third-party tool) from
// violating it in a user's installed copy. IndexCategory's guidToEffect map
// doesn't defend against that -- building the index just lets the last
// effect indexed with a given guid win that map entry -- so FindAllByGuid
// (merge.cpp) then only ever sees that one winner for a violated guid, not
// both owners. A violated premise wouldn't crash anything, but could
// silently pick the "wrong" of two same-guid effects to rework.
//
// This is a separate, standalone check on a single file (installed or
// otherwise) rather than part of resolving a plan: it's a data-integrity
// question about that one file in isolation, unrelated to whether an
// update is even available. Read-only -- never mutates `file`.
// ---------------------------------------------------------------------------

// Returns every guid that appears on more than one effect anywhere in
// `file` (each such guid listed once, regardless of how many effects share
// it). An empty result means the guid-uniqueness premise holds for this
// file. Malformed input (missing/non-array "categories") is treated as "no
// duplicates found" rather than an error -- callers that care about a
// malformed file already have other checks for that (see ResolveMergePlan's
// outOk).
std::vector<std::string> FindDuplicateGuids(const nlohmann::ordered_json& file);