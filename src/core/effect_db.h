//################################################################################
// effect_db.h
//--------------------------------------------------------------------------------
// Self-only "for science" capture: every distinct (guid, block, type) identity
// this addon has seen, every distinct (duration, a4, a6, self_mask)
// combination under it, and which races (EffectDbRaceMask) and
// profession+specializations (EffectDbSpecializationMask) it's been seen on.
// Backed by a single SQLite file (see EffectDb_Open); never folds and never
// clears, accumulating across every session permanently on disk, unlike
// live_log.h, which folds one entry per guid for on-screen display and clears
// every session.
//
// Does not store: installedBehavior (see live_log.h for what the user's own
// sin file does with a guid), mapID (redundant with
// profession/race/specialization, which already localize "where" here), or
// free-text notes (too much upkeep for a background capture tool).
//--------------------------------------------------------------------------------

#pragma once

#include "Mumble.h"
#include "Nexus.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDbSelfMask
//--------------------------------------------------------------------------------
// Caster/target-relative-to-self, as a 2-bit flag instead of a bool, so a
// later phase that also watches target==self doesn't need a schema change --
// see EffectDb_RecordEvent's doc comment on why kSelfMaskNone can't occur yet.
//--------------------------------------------------------------------------------
enum EffectDbSelfMask : uint8_t
{
    kSelfMaskNone   = 0b00,
    kSelfMaskTarget = 0b01,
    kSelfMaskCaster = 0b10,
    kSelfMaskBoth   = 0b11,
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDbRaceMask / EffectDb_RaceBit / EffectDb_RacesInMask
//--------------------------------------------------------------------------------
// One bit per Mumble::ERace value, stored on `occurrences` as race_mask
// instead of one full row per race (see CreateSchemaIfNeeded in
// effect_db.cpp). uint32_t gives headroom past today's 5 known values
// (Asura..Sylvari), so a future ERace addition is just a higher bit, no schema
// change needed. EffectDb_RaceBit is the single-race bit EffectDb_RecordEvent
// ORs in on each sighting; EffectDb_RacesInMask is the inverse, unpacking
// every bit set in `mask` back to ERace values for a caller rendering "races
// seen" (see live_log_ui.cpp / installed_tree_view.cpp), looping the full
// 32-bit width so a future value shows up without an edit.
//--------------------------------------------------------------------------------
using EffectDbRaceMask = uint32_t;

inline EffectDbRaceMask EffectDb_RaceBit(Mumble::ERace race)
{
    return EffectDbRaceMask{1} << static_cast<unsigned char>(race);
}

std::vector<Mumble::ERace> EffectDb_RacesInMask(EffectDbRaceMask mask);

//********************************************************************************
// EffectDbSpecializationMask
//--------------------------------------------------------------------------------
// lo   real spec ids 1..63 as bits (bit 0 unused -- ids start at 1)
// hi   real spec ids 64..81, plus reserved core-only pseudo-ids 118..127
//--------------------------------------------------------------------------------
// Same "fold instead of a new row" reasoning as EffectDbRaceMask above,
// applied to profession+specialization together. A nonzero specialization id
// already implies its owning profession (see specialization_info.h), so one
// merged mask suffices; the only gap is specialization == 0 (core build),
// closed by EffectDb_SpecOrCoreId below with reserved pseudo-ids at the top of
// the bit range. Two uint64_t words stored as specialization_mask_lo/_hi, each
// OR'd independently on conflict, same shape as race_mask split across two
// words.
//--------------------------------------------------------------------------------
struct EffectDbSpecializationMask
{
    uint64_t lo = 0;
    uint64_t hi = 0;
};

//_ Ids >= this are reserved pseudo-ids, not real spec ids (see SpecOrCoreId).
constexpr unsigned int kEffectDbCoreOnlyIdFloor = 118;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SpecOrCoreId / EffectDb_SpecBit
//--------------------------------------------------------------------------------
// Resolve an event's (profession, specId) pair down to the bit index (or
// full mask) to OR into a row -- specId == 0 (core build, see
// GameState_GetSpecialization) falls through to a reserved profession-only
// pseudo-id, 127 - static_cast<unsigned char>(profession), counting down
// from 127 so real spec ids (currently 1..81) can keep growing upward
// without colliding. specId is asserted < 82 to catch a caller
// accidentally passing a pseudo-id back in.
//--------------------------------------------------------------------------------
inline unsigned int EffectDb_SpecOrCoreId(Mumble::EProfession prof, unsigned int specId)
{
    assert(specId == 0 || specId < kEffectDbCoreOnlyIdFloor); //. guard against reserved-id collision
    return specId != 0
        ? specId
        : 127 - static_cast<unsigned char>(prof);
}

inline EffectDbSpecializationMask EffectDb_SpecBit(Mumble::EProfession prof, unsigned int specId)
{
    unsigned int bit = EffectDb_SpecOrCoreId(prof, specId);
    EffectDbSpecializationMask mask;
    if (bit < 64) mask.lo = uint64_t{1} << bit;
    else          mask.hi = uint64_t{1} << (bit - 64);
    return mask;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_ProfessionFromCoreOnlyId
//--------------------------------------------------------------------------------
// Inverse of EffectDb_SpecOrCoreId's pseudo-id branch -- given a reserved
// id (>= kEffectDbCoreOnlyIdFloor) from EffectDb_SpecOrCoreIdsInMask,
// returns which profession it stands for (a core build, no elite spec
// active). Passing a real spec id here is a caller bug; callers are
// expected to branch on kEffectDbCoreOnlyIdFloor first.
//--------------------------------------------------------------------------------
inline Mumble::EProfession EffectDb_ProfessionFromCoreOnlyId(unsigned int id)
{
    assert(id >= kEffectDbCoreOnlyIdFloor && id <= 127);
    return static_cast<Mumble::EProfession>(127 - id);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SpecOrCoreIdsInMask
//--------------------------------------------------------------------------------
// Inverse of EffectDb_SpecBit -- every bit set in mask, unpacked back to raw
// ids (1..127). Not resolved to a profession/name pair here -- that's a
// display concern (see live_log_ui.cpp / installed_tree_view.cpp), not this
// module's; a caller resolves each id via
// SpecializationProfession()/SpecializationName() for a real id, or
// EffectDb_ProfessionFromCoreOnlyId() for a reserved one.
//--------------------------------------------------------------------------------
std::vector<unsigned int> EffectDb_SpecOrCoreIdsInMask(const EffectDbSpecializationMask& mask);

//********************************************************************************
// EffectDbRawEvent
//--------------------------------------------------------------------------------
// One infostr line, unparsed further -- the input to EffectDb_RecordEvent.
// Distinct from LiveLogEntry (live_log.h): that struct folds to one-per-guid
// and carries display/behavior fields this module has no use for; this is the
// raw per-sighting shape the fold would otherwise be built from.
// groupStarterGuid is resolved by the caller's live group state, never
// re-derived here (see AdvanceGroupState). profession/race/specialization
// resolve to a single EffectDb_SpecBit before storage (see
// EffectDbSpecializationMask above), kept separate here since that matches how
// GameState_GetProfession/GameState_GetSpecialization produce them.
//--------------------------------------------------------------------------------
struct EffectDbRawEvent
{
    std::string guid_b64;
    std::string name;           //. "" for a typeless marker line
    int         type     = 0;
    int         duration = 0;
    unsigned int a4      = 0;
    std::string a6;
    std::string blockGroup;     //. "" if no dotted block
    std::string blockMember;    //. "" likewise

    //_ "" outside an open group; else the starter guid, see struct header
    std::string groupStarterGuid;

    EffectDbSelfMask selfMask = kSelfMaskNone;

    //_ Resolved to EffectDb_SpecBit before storage, see struct header
    Mumble::EProfession  profession{};
    Mumble::ERace         race{};
    unsigned int          specialization = 0;
};

//********************************************************************************
// EffectDbEffect   (pairs with: EffectDbOccurrence)
//--------------------------------------------------------------------------------
// Read-side mirror of effects/effect_meta, for the tree overlay and "for
// science" detail view to consume without touching SQL directly.
// name/categoryPath/description/behavior* all come from effect_meta via
// effect_id -- a join, from this struct's perspective, not a second call.
// behaviorType/behaviorCaster/behaviorDuration use ''/0 as "not set";
// behaviorDuration only means something when behaviorType == "SetDuration".
// sortOrder is externally seeded only, a curated Greed-file position; a
// capture-discovered guid gets MAX(sort_order)+1 at insert time (see
// kInsertEffectMeta in effect_db.cpp), sorting last instead of ahead of
// curated content.
//--------------------------------------------------------------------------------
struct EffectDbEffect
{
    std::string guid_b64;
    int64_t     effect_id = 0;   //. shared across one effect's guids
    std::string name;
    std::string blockGroup;
    std::string blockMember;
    int         type = -1;       //. -1 = not yet captured (see effect_db.cpp)
    bool        in_json = false; //. presence-only, never written here

    std::vector<std::string> categoryPath;  //. empty = not yet placed anywhere
    std::string description;

    //_ '' = no default set, see struct header
    std::string behaviorType;
    std::string behaviorCaster;
    int         behaviorDuration = 0;

    //_ Externally seeded only, see struct header
    int sortOrder = 0;
};

//********************************************************************************
// EffectDbCategory
//--------------------------------------------------------------------------------
// One row of the `categories` table -- a category's own description and
// sort position, since neither can be recovered from effect_meta.category_path
// alone (that's just a string on each effect row, not an entity with its
// own identity). Externally seeded only, same as EffectDbEffect's curated
// fields -- see EffectDb_GetAllCategories.
//--------------------------------------------------------------------------------
struct EffectDbCategory
{
    std::vector<std::string> categoryPath;  //. full path, split on delimiter
    std::string description;
    int sortOrder = 0;
};

//********************************************************************************
// EffectDbOccurrence   (pairs with: EffectDbEffect)
//--------------------------------------------------------------------------------
// Read-side mirror of one `occurrences` row -- a distinct (duration, a4,
// a6, self_mask) signature for a guid, with race/spec folded into masks
// (see EffectDbRaceMask/EffectDbSpecializationMask above).
//--------------------------------------------------------------------------------
struct EffectDbOccurrence
{
    int          duration = 0;
    unsigned int a4       = 0;
    std::string  a6;
    EffectDbSelfMask self_mask = kSelfMaskNone;

    EffectDbRaceMask            raceMask = 0;  //. see EffectDb_RacesInMask to unpack
    EffectDbSpecializationMask  specializationMask{};  //. see EffectDb_SpecOrCoreIdsInMask to unpack
};

//********************************************************************************
// EffectDbGroupInstance / EffectDbGroupMembership
//--------------------------------------------------------------------------------
// Read-side mirrors of group_members, split by which end of the FK the query
// is keyed from -- see EffectDb_GetGroupsStarted/EffectDb_GetGroupsMemberOf
// just below for which is which. EffectDbGroupInstance::memberGuids includes
// the starter's own guid (group_members' own convention: the starter's row has
// member_guid_b64 == starter_guid_b64), so a group with no other member ever
// recorded still shows up as a one-member instance, not an empty one.
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
// EffectDb_EnsureOpenForBrowsing
//--------------------------------------------------------------------------------
// The DB tab's own entry point -- opens the database if it isn't
// already, WITHOUT going through EffectDb_SetEnabled's gates or
// requiring EffectDb_GreedFileExists (a db can be real and populated
// with capture toggled off, or with no Greed.json at all -- the db is
// meant to be authoritative on its own). Doesn't touch s_enabled either
// way -- browsing must never silently turn capture on, and toggling
// capture off later must not close a connection this still needs open.
// A no-op if already open (from either this or EffectDb_SetEnabled --
// whichever got there first owns the connection). Returns false
// (outError filled) only on a real open/schema failure.
//--------------------------------------------------------------------------------
bool EffectDb_EnsureOpenForBrowsing(const std::string& denoiserAddonDir, std::string& outError);

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
// The "for science" toggle. Turning on requires EffectDb_GreedFileExists to
// already be true -- if it isn't, this returns false and enabled state is left
// unchanged; the caller's UI surfaces why (tooltip pointing at the filename,
// not an auto-create). Turning off never fails. While enabled,
// EffectDb_RecordEvent actually writes, and the caller blocks the GitHub
// check/apply-update flow entirely (see github_update.cpp) so a bulk JSON
// rewrite never lands mid-capture. Manual tree edits (rename, promote,
// drag-to-category) stay live regardless.
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
// No-op if EffectDb_IsEnabled() is false. Upserts `ev` into effects,
// occurrences, and (when ev.groupStarterGuid is non-empty) group_members (see
// CreateSchemaIfNeeded in effect_db.cpp for what each stores) -- each insert
// is a silent no-op on a duplicate tuple, so callers never pre-check. All
// three join whatever transaction is currently open instead of each committing
// its own (see EffectDb_FlushPendingWrites for who commits).
//
// Caller must only ever pass self-involved events -- ev.selfMask is trusted
// as-is, never re-derived here. Currently always kSelfMaskCaster; Target/Both
// are reserved for target-watching.
//--------------------------------------------------------------------------------
void EffectDb_RecordEvent(const EffectDbRawEvent& ev);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_FlushPendingWrites
//--------------------------------------------------------------------------------
// Commits the transaction EffectDb_RecordEvent left open, if one is
// currently open; a cheap no-op otherwise. Meant to be called once per
// real frame (see entry.cpp's RT_PostRender registration, which runs
// regardless of whether the options panel is open, unlike RT_OptionsRender)
// so a burst of same-frame RecordEvent calls collapses into a single WAL
// commit instead of stalling the render thread with one commit per event.
// Also called from EffectDb_Close() itself -- see that function.
//--------------------------------------------------------------------------------
void EffectDb_FlushPendingWrites();

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
// Bumped on any write that could change what the DB tab's tree should look
// like: a genuinely new effect row (not a dedup no-op, see
// EffectDb_RecordEvent), a successful EffectDb_SetName/SetCategoryPath, or a
// real occurrence/group_members write -- RenderEffectDbDetail renders
// occurrence/group detail directly and is the only reader of this counter.
// Same purpose as GetInstalledTreeGeneration in installed_tree_store.h: lets a
// cache built over this data tell "this changed" apart from "same generation
// already built from".
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
// EffectDb_GetAllGroupStarters
//--------------------------------------------------------------------------------
// Every distinct guid that has ever started a type:1/11 group (every distinct
// group_members.starter_guid_b64), as a full EffectDbEffect each -- lets the
// DB tab's group-review view browse "every unnamed starter" without already
// knowing a guid (unlike EffectDb_GetGroupsStarted, which requires one as
// input). A starter always has its own effects row by construction
// (EffectDb_RecordEvent writes both together), so a lookup failure here is
// skipped, not surfaced -- it would mean a data inconsistency, not an expected
// case. Not hot-path, so the ad-hoc two-step query (list starters, then
// EffectDb_GetEffect each) is fine.
//--------------------------------------------------------------------------------
std::vector<EffectDbEffect> EffectDb_GetAllGroupStarters();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetAllCategories
//--------------------------------------------------------------------------------
// Full `categories` table, for SinGenerator's category-node materialization
// (description + sort_order) -- see EffectDbCategory above.
//--------------------------------------------------------------------------------
std::vector<EffectDbCategory> EffectDb_GetAllCategories();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetOccurrences
//--------------------------------------------------------------------------------
// Every distinct (duration, a4, a6, self_mask) row recorded for guid_b64,
// for the "for science" expanded tree view (duration/a4/a6 -> class ->
// spec, with specializationMask unpacked into per-profession spec buckets
// and raceMask unpacked into a "races seen" annotation on the signature --
// see EffectDb_SpecOrCoreIdsInMask/EffectDb_RacesInMask and this module's
// callers for how that's grouped; this just returns the flat rows). Empty
// if guid_b64 is unknown.
//--------------------------------------------------------------------------------
std::vector<EffectDbOccurrence> EffectDb_GetOccurrences(const std::string& guid_b64);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetGroupsStarted / EffectDb_GetGroupsMemberOf
//--------------------------------------------------------------------------------
// Both read group_members -- see CreateSchemaIfNeeded in effect_db.cpp for
// what it stores and why membership can't be recovered from occurrences alone.
// GetGroupsStarted(guid_b64): every (duration, a4) instance where guid_b64 was
// the starter, with the full member set recorded under that instance,
// including guid_b64 itself. GetGroupsMemberOf(guid_b64): every (starter guid,
// duration, a4) instance where guid_b64 was a member of a group someone else
// started -- a guid's own group is already covered above, so it's excluded
// here instead of listed in both. Both empty if guid_b64 has no group_members
// rows, ordered by (duration, a4) for stable, deterministic iteration.
//--------------------------------------------------------------------------------
std::vector<EffectDbGroupInstance>   EffectDb_GetGroupsStarted(const std::string& guid_b64);
std::vector<EffectDbGroupMembership> EffectDb_GetGroupsMemberOf(const std::string& guid_b64);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SetName
//--------------------------------------------------------------------------------
// SQL is the sole source of truth for an effect's name -- this never
// touches, and is never touched by, any installed sin's JSON (the JSON
// tab(s) and the db's own view are fully independent, neither reads nor
// writes the other). Takes a guid_b64 for caller convenience (that's
// what a tree click/quick-edit naturally has on hand), but resolves it
// to that guid's effect_id first and writes effect_meta -- so renaming
// any one guid of a multi-guid effect renames the whole effect, every
// sibling guid included, not just the one clicked. No-op (returns
// false) if guid_b64 isn't known yet.
//--------------------------------------------------------------------------------
bool EffectDb_SetName(const std::string& guid_b64, const std::string& name);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SetCategoryPath
//--------------------------------------------------------------------------------
// SQL-only placement -- never touches, and is never touched by, any
// installed sin's JSON (see EffectDb_SetName's comment just above; same
// independence applies here). Resolves guid_b64 to its effect_id first
// and writes effect_meta, so this moves the whole effect (every sibling
// guid), not just the one clicked. Does not require guid_b64 to have
// ever appeared in JSON at all. No-op (returns false) if guid_b64 isn't
// known yet.
//--------------------------------------------------------------------------------
bool EffectDb_SetCategoryPath(const std::string& guid_b64, const std::vector<std::string>& categoryPath);