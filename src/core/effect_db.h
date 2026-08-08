//################################################################################
// effect_db.h
//--------------------------------------------------------------------------------
// The "for science" effect database -- a self-only, autonomous capture of
// every distinct (guid, block, type) identity this addon has seen, plus
// every distinct (duration, a4, a6, self_mask, profession, race,
// specialization) combination each of those identities has shown up
// under. Separate concern from live_log.h: that module folds one entry
// per guid for on-screen display and is cleared per-session; this module
// never folds and never clears -- it's meant to accumulate across every
// session, permanently, on disk.
//
// Deliberately does NOT store: installedBehavior (this db doesn't care
// what the user's own sin file does with a guid -- see live_log.h for
// that), mapID (self-only capture makes it redundant with
// profession/race/specialization, which already localize "where" in the
// sense that matters here), or free-text notes (decided against --
// too much upkeep for a background capture tool).
//--------------------------------------------------------------------------------
// Storage shape (SQLite, one file, see EffectDb_Open):
//
//   effects
//     guid_b64       PK
//     name           from the infostr line's trailing name, "" for a
//                    type 1/11 marker row that has none. Refreshed by
//                    EffectDb_SetName only -- never overwritten by
//                    ingestion once a row exists, so a user rename
//                    survives later sightings of the same guid.
//     block_group    5 chars, "" if this guid's infostr had no dotted
//                    block at all
//     block_member   5 chars, "" likewise
//     type
//     category_path  '\x1f'-joined path segments (see
//                    EffectDb_SetCategoryPath), "" if never placed
//
//   occurrences
//     guid_b64       FK -> effects.guid_b64
//     duration
//     a4
//     a6
//     self_mask      see EffectDbSelfMask
//     profession
//     race
//     specialization
//     UNIQUE(guid_b64, duration, a4, a6, self_mask, profession, race,
//            specialization) -- a repeat of an already-seen combination
//            is a silent no-op insert, not a new row. This is what makes
//            the table safe to write to on every single matching event
//            without the caller pre-checking for duplicates itself.
//
//   group_members
//     starter_guid_b64  guid of the type:1/11 line that opened the group
//     duration          the starter's duration
//     a4                the starter's a4
//     member_guid_b64   FK -> effects.guid_b64; a distinct guid seen while
//                       that group was open, including the starter itself
//                       (its own row has member_guid_b64 == starter_guid_b64)
//     UNIQUE(starter_guid_b64, duration, a4, member_guid_b64) -- same
//            "record the fact once" convention as occurrences.
//
//     This table exists because group membership can NOT be reconstructed
//     later from `occurrences` alone, for two separate reasons, both
//     confirmed against real log data rather than hypothetical:
//       1. `occurrences` is deduplicated (see its own UNIQUE constraint
//          above) -- a repeat sighting of an already-seen tuple writes
//          nothing, so whether a *particular* firing happened to be
//          preceded by a live, unbroken type:1/11 run is exactly the
//          information that write throws away.
//       2. (duration, a4) pairs get reused by unrelated effects (an open
//          hypothesis below already documents a real example) -- so
//          querying "everything that shares this starter's (duration,
//          a4)" is not the same question as "everything that was actually
//          in this group," and would silently merge unrelated data.
//     The starter's own (duration, a4) is included in this table's key
//     (not just starter_guid_b64) for the same reason: the same starter
//     guid can open the group with different numbers on a later cast
//     (see the "signature stamp" hypothesis below) -- whether that
//     later firing has the same member set as an earlier one is itself
//     an open question this table is meant to let someone actually check,
//     not something to assume by merging them together at write time.
//     Read side: EffectDb_GetGroupsStarted / EffectDb_GetGroupsMemberOf
//     (below) expose this table for the tree/live-log "group info"
//     display -- a raw membership browse, not pattern detection, so it
//     doesn't run afoul of "Occurrence data was deliberately not wired
//     into any correlation-detection UI" in the project handoff doc. That
//     caution is about inferring/asserting a hypothesis (e.g. "a4
//     determines group membership"); this is just showing the rows that
//     are already there.
//
// guid_b64/block/type on `effects` are first-seen-wins: EffectDb_RecordEvent
// never updates them on a guid that already has a row. If that
// assumption -- block/type as a fixed identity -- ever turns out to be
// wrong for some guid, that's a real finding, not a bug to code around
// here; see the occurrences table for what already does vary.
//--------------------------------------------------------------------------------

#pragma once

#include "game_state.h" //. pulls in Nexus.h (AddonAPI_t) and Mumble.h (EProfession/ERace) together

#include <cstdint>
#include <string>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDbSelfMask
//--------------------------------------------------------------------------------
// Caster/target-relative-to-self, as a 2-bit flag rather than a bool, so a
// later phase that also watches target==self doesn't need a schema
// change -- see EffectDb_RecordEvent's doc comment on why kSelfMaskNone
// can't actually occur yet.
//--------------------------------------------------------------------------------
enum EffectDbSelfMask : uint8_t
{
    kSelfMaskNone   = 0b00,
    kSelfMaskTarget = 0b01,
    kSelfMaskCaster = 0b10,
    kSelfMaskBoth   = 0b11,
};

//********************************************************************************
// EffectDbRawEvent
//--------------------------------------------------------------------------------
// One infostr line, essentially unparsed-further -- the input to
// EffectDb_RecordEvent. Distinct from LiveLogEntry (live_log.h): that
// struct folds to one-per-guid and carries display/behavior fields this
// module has no use for; this is the raw per-sighting shape the fold
// would otherwise be built from.
//--------------------------------------------------------------------------------
struct EffectDbRawEvent
{
    std::string guid_b64;
    std::string name;           //. "" for a typeless marker line
    int         type     = 0;
    int         duration = 0;
    unsigned int a4      = 0;
    std::string a6;
    std::string blockGroup;     //. "" if this line had no dotted block
    std::string blockMember;    //. "" likewise

    //_ "" if not part of an open type:1/11 group; otherwise the starter
    // guid (equal to ev.guid_b64 on the starter's own event). Resolved
    // by the caller's live group state -- never re-derived here (see AdvanceGroupState).
    std::string groupStarterGuid;

    EffectDbSelfMask selfMask = kSelfMaskNone;

    Mumble::EProfession  profession{};
    Mumble::ERace         race{};
    unsigned int          specialization = 0;
};

//********************************************************************************
// EffectDbEffect / EffectDbOccurrence
//--------------------------------------------------------------------------------
// Read-side mirrors of the two tables above, for the tree overlay and the
// "for science" detail view to consume without touching SQL directly.
//--------------------------------------------------------------------------------
struct EffectDbEffect
{
    std::string guid_b64;
    std::string name;
    std::string blockGroup;
    std::string blockMember;
    int         type = 0;
    std::vector<std::string> categoryPath;  //. empty = not yet placed anywhere
};

struct EffectDbOccurrence
{
    int          duration = 0;
    unsigned int a4       = 0;
    std::string  a6;
    EffectDbSelfMask self_mask = kSelfMaskNone;

    Mumble::EProfession  profession{};
    Mumble::ERace         race{};
    unsigned int          specialization = 0;
};

//********************************************************************************
// EffectDbGroupInstance / EffectDbGroupMembership
//--------------------------------------------------------------------------------
// Read-side mirrors of group_members, split by which end of the FK the
// query is keyed from -- see EffectDb_GetGroupsStarted/
// EffectDb_GetGroupsMemberOf just below for which is which.
//
// EffectDbGroupInstance::memberGuids includes the starter's own guid
// (group_members' own convention -- the starter's row has
// member_guid_b64 == starter_guid_b64), so a group with no other member
// ever recorded still shows up as a one-member instance rather than an
// empty one.
//--------------------------------------------------------------------------------
struct EffectDbGroupInstance
{
    int          duration = 0;
    unsigned int a4       = 0;
    std::vector<std::string> memberGuids;
};

struct EffectDbGroupMembership
{
    std::string  starterGuid_b64;
    int          duration = 0;
    unsigned int a4       = 0;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SetApi
//--------------------------------------------------------------------------------
// Same shape as InstalledTreeStore_SetApi -- used only for aApi->Log on a
// failure path (open/write errors). Call once from Addon_Load.
//--------------------------------------------------------------------------------
void EffectDb_SetApi(AddonAPI_t* aApi);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_Open / EffectDb_Close
//--------------------------------------------------------------------------------
// Opens (creating if absent) the database file inside denoiserAddonDir --
// a plain sibling of the VfxD_*.json files, not related to any of them on
// disk. Safe to call Open more than once (e.g. on a denoiserAddonDir
// change); a prior connection is closed first. outError is filled and
// false returned on any failure to open; the addon should treat this the
// same as "for science" being unavailable, not crash.
//--------------------------------------------------------------------------------
bool EffectDb_Open(const std::string& denoiserAddonDir, std::string& outError);
void EffectDb_Close();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GreedFileExists
//--------------------------------------------------------------------------------
// Cheap stat()-only check for VfxD_Greed.json inside denoiserAddonDir --
// no parse, matching ScanInstalledSinFiles's own filename-only match for
// this kind of file. Never creates it. Used both as the hard gate on
// EffectDb_SetEnabled(true) and by EffectDb_Poll's periodic recheck.
//--------------------------------------------------------------------------------
bool EffectDb_GreedFileExists(const std::string& denoiserAddonDir);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SetEnabled / EffectDb_IsEnabled
//--------------------------------------------------------------------------------
// The "for science" toggle. Turning on requires EffectDb_GreedFileExists
// to already be true -- if it isn't, this returns false and enabled
// state is left unchanged; the caller's UI is responsible for surfacing
// why (tooltip pointing at the filename, not an auto-create). Turning
// off never fails.
//
// While enabled: EffectDb_RecordEvent actually writes, and the caller is
// expected to block the GitHub check/apply-update flow entirely (not
// this module's concern to enforce -- see github_update.cpp) so a bulk
// JSON rewrite never lands mid-capture. Manual tree edits (rename,
// promote, drag-to-category) stay live the whole time regardless.
//--------------------------------------------------------------------------------
bool EffectDb_SetEnabled(bool enabled, const std::string& denoiserAddonDir);
bool EffectDb_IsEnabled();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_Poll
//--------------------------------------------------------------------------------
// Call once per frame from wherever the Live Log panel already ticks --
// internally rate-limits its own stat() call to roughly once a second,
// so this is cheap to call unconditionally. If "for science" is enabled
// and VfxD_Greed.json has disappeared out from under it, this stops
// capture, flips EffectDb_IsEnabled() back to false, and returns a
// non-empty message describing what happened for the caller to surface
// (e.g. a toast/status line). Returns "" every other frame. Whatever was
// already written to the db before the file vanished is left as-is --
// nothing to roll back, since presence of Greed.json was only ever a
// gate on writing, never a foreign key any row depends on.
//--------------------------------------------------------------------------------
std::string EffectDb_Poll(const std::string& denoiserAddonDir);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_RecordEvent
//--------------------------------------------------------------------------------
// No-op if EffectDb_IsEnabled() is false. Upserts `ev` into effects and
// occurrences, and -- when ev.groupStarterGuid is non-empty -- into
// group_members too: an EFFECTS row is inserted only if guid_b64 isn't
// already known (first seen wins on name/block/type -- see the file-level
// comment on why this is deliberate, not a shortcut); an OCCURRENCES row
// is inserted only if this exact tuple hasn't been seen before for this
// guid (silent no-op otherwise, per the UNIQUE constraint); a
// GROUP_MEMBERS row records (ev.groupStarterGuid, ev.duration, ev.a4,
// ev.guid_b64), again a silent no-op on repeat. All three inserts happen
// in the one transaction below, so a group's starter guid always has its
// own EFFECTS row committed before any member row that references it
// (the starter's own event is always recorded first in real capture
// order).
//
// Caller is responsible for only ever passing self-involved events --
// this module trusts ev.selfMask rather than re-deriving it, same
// "passed in, not read from statics" convention live_log.h/report_ui.h
// already use. In the current (caster-only) phase, ev.selfMask should
// always be kSelfMaskCaster; kSelfMaskNone is never valid to pass here
// (an event involving self in neither role has nothing to record against
// profession/race/specialization and shouldn't have reached this call at
// all) -- kSelfMaskTarget/kSelfMaskBoth are reserved for once
// target-watching is added, not yet produced by anything.
//--------------------------------------------------------------------------------
void EffectDb_RecordEvent(const EffectDbRawEvent& ev);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_IsKnownGuid
//--------------------------------------------------------------------------------
// True if guid_b64 already has an EFFECTS row. This is what the Live Log
// UI's inline "unknown -> quick edit" affordance keys off of -- an
// unknown guid gets the edit control, a known one doesn't need it every
// time it's re-sighted.
//--------------------------------------------------------------------------------
bool EffectDb_IsKnownGuid(const std::string& guid_b64);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetGeneration
//--------------------------------------------------------------------------------
// Bumped on any write that could change what a tree overlay built from
// this module's data should look like: a genuinely new effect row (a
// dedup no-op does NOT bump this -- see EffectDb_RecordEvent, which is
// called at high frequency and would otherwise force a full tree-overlay
// rebuild on every repeat sighting), a successful EffectDb_SetName, or a
// successful EffectDb_SetCategoryPath. Deliberately NOT bumped on a new
// occurrence row alone -- nothing the tree currently renders depends on
// occurrences (that's the "for science" detail expansion, not yet
// built), and duration/a4/class combinations vary often enough on an
// already-known guid that bumping for those too would rebuild the
// overlay far more than the tree actually needs.
//
// Same purpose as GetInstalledTreeGeneration in installed_tree_store.h --
// lets a cache built over this data (see installed_tree_overlay.h's
// BuildEffectDbOverlayTree and its caller in installed_tree_view.cpp)
// tell "this actually changed" apart from "same generation I already
// built my cache from", without every consumer needing its own
// invalidation hook.
//--------------------------------------------------------------------------------
int EffectDb_GetGeneration();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetEffect / EffectDb_GetAllEffects
//--------------------------------------------------------------------------------
// Read-only lookups for the tree overlay builder. GetEffect returns
// false if guid_b64 isn't known. GetAllEffects is the full table --
// cheap enough to call whenever the overlay is (re)built, same "not a
// per-frame call" expectation as GetInstalledJson.
//--------------------------------------------------------------------------------
bool EffectDb_GetEffect(const std::string& guid_b64, EffectDbEffect& out);
std::vector<EffectDbEffect> EffectDb_GetAllEffects();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetOccurrences
//--------------------------------------------------------------------------------
// Every distinct (duration, a4, a6, self_mask, profession, race,
// specialization) row recorded for guid_b64, for the "for science"
// expanded tree view (duration/a4/a6 -> class -> spec, race folded in as
// an annotation -- see this module's callers for how that's grouped;
// this just returns the flat rows). Empty if guid_b64 is unknown.
//--------------------------------------------------------------------------------
std::vector<EffectDbOccurrence> EffectDb_GetOccurrences(const std::string& guid_b64);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetGroupsStarted / EffectDb_GetGroupsMemberOf
//--------------------------------------------------------------------------------
// Both read group_members (see the file-level comment on that table for
// why it exists and what it does/doesn't let you reconstruct).
//
//  - GetGroupsStarted(guid_b64): every distinct (duration, a4) instance
//    where guid_b64 was the *starter* (the type:1/11 line that opened
//    the group), each with the full set of member guids recorded under
//    that specific instance -- including guid_b64 itself, per
//    EffectDbGroupInstance's own doc comment. Empty if this guid has
//    never opened a group.
//  - GetGroupsMemberOf(guid_b64): every (starter guid, duration, a4)
//    instance where guid_b64 showed up as a member of a group *someone
//    else* started (starter_guid_b64 != guid_b64 -- a guid's own
//    membership in a group it started itself is already covered by
//    GetGroupsStarted, and would otherwise show up redundantly in both).
//    Empty if this guid has never been swept into another guid's group.
//
// Both empty if guid_b64 has no group_members rows at all -- most guids,
// since only type:1/11 lines and whatever fired while one was open ever
// get one. Ordered by (duration, a4) [/ starter guid] for stable,
// deterministic iteration, same reason the occurrences grouping map
// elsewhere in this codebase uses std::map rather than an unordered one.
//--------------------------------------------------------------------------------
std::vector<EffectDbGroupInstance>   EffectDb_GetGroupsStarted(const std::string& guid_b64);
std::vector<EffectDbGroupMembership> EffectDb_GetGroupsMemberOf(const std::string& guid_b64);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SetName
//--------------------------------------------------------------------------------
// Db-side half of a rename. No-op (returns false) if guid_b64 isn't
// known yet. This does NOT touch any installed sin's JSON -- a rename of
// a guid that also exists in an installed sin file is the one case
// that's meant to write both places (see this module's own design
// discussion), and the JSON half of that is the caller's job via the
// existing FindInstalledJsonMutable/SaveInstalledSinFile path, same as
// every other JSON edit in this addon. Calling only this function is
// correct and sufficient for a guid that has no JSON entry at all.
//--------------------------------------------------------------------------------
bool EffectDb_SetName(const std::string& guid_b64, const std::string& name);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SetCategoryPath
//--------------------------------------------------------------------------------
// Db-only placement, e.g. from a drag-and-drop in the tree UI. Does NOT
// write any JSON and does NOT require guid_b64 to ever be promoted --
// this is what lets a db-only guid render in the correct spot in the
// tree (materializing a virtual category header if categoryPath doesn't
// exist in any installed sin yet, same shape as
// installed_tree_overlay.cpp's FindOrCreateDiffCategory) well before, or
// entirely without, an "add to JSON" action ever happening. No-op
// (returns false) if guid_b64 isn't known yet.
//--------------------------------------------------------------------------------
bool EffectDb_SetCategoryPath(const std::string& guid_b64, const std::vector<std::string>& categoryPath);