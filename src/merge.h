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
// consulted once guid matching has found nothing, or to break a tie when a
// new effect's guids are split across more than one old effect (see 1c).
//
//   1. This new effect has at least one guid claimed somewhere in oldFile
//      (every one of its guids is checked, not just the first that hits
//      something -- a new effect's guid list can in principle straddle
//      more than one old effect, e.g. an upstream merge of two
//      previously-separate effects into one):
//        a. Every matched guid points to the SAME old effect, and name
//           also matches that same old effect -> an ArenaNet
//           skill-effect rework. Keep oldFile's name, category location,
//           and behaviors (settings) exactly as they are. Guids are
//           updated by comparing this effect's own old guid list against
//           its new one (never a whole-file index):
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
//           name differs -> skip entirely. Guid identity says this is the
//           same underlying effect under upstream's new name, but the user
//           may have renamed it themselves -- their naming is left alone
//           rather than guessed at. Never appears in the plan.
//        c. Matched guids are split across MORE THAN ONE old effect ->
//           name is the tie-breaker: exactly one of the matched candidates
//           sharing this new effect's name is reworked (1a's logic, run
//           against just that one candidate -- the other candidate's own
//           matched guid is simply treated as newly-added, same as any
//           other guid the reworked effect didn't have before). Zero, or
//           more than one, matched candidate sharing the name -> no way to
//           pick without guessing, so nothing is touched and this never
//           appears in the plan.
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

// An existing effect (case 1a/2a) whose GUIDs would be updated under its
// unchanged name/category/settings. `oldGuids` is the matched effect's
// exact guid list *at resolve time* -- it doubles as this rework's identity
// key: since guids are globally unique, ApplyMergePlan (and any display
// code overlaying this onto a tree) looks up the target by checking which
// old effect owns one of these guids, not by `name`. Looking up by name
// instead would reintroduce the exact bug this design fixes -- multiple
// old effects can share a name, so a name-based lookup can't tell them
// apart, but a guid-based one always can. `newGuids` is the *final* guid
// list to write -- either newFile's raw list verbatim (a clean same-count
// renumber) or oldGuids plus whatever's newly added (an add-only change).
struct MergePlanRework
{
    std::string              name;
    std::vector<std::string> oldGuids;
    std::vector<std::string> newGuids;
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
// for every rework, then inserts every new effect (creating categories as
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
