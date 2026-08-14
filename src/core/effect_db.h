//################################################################################
// effect_db.h
//--------------------------------------------------------------------------------
// The "for science" effect database -- a self-only, autonomous capture of
// every distinct (guid, block, type) identity this addon has seen, plus
// every distinct (duration, a4, a6, self_mask) combination each identity
// has shown up under, with a bitmask of every race (see EffectDbRaceMask)
// and every profession+specialization pairing (see
// EffectDbSpecializationMask) that combination has been seen on. Separate
// concern from live_log.h: that module folds one entry per guid for
// on-screen display and is cleared per-session; this module never folds
// and never clears -- it accumulates across every session, permanently,
// on disk.
//
// Deliberately does NOT store: installedBehavior (this db doesn't care
// what the user's own sin file does with a guid -- see live_log.h for
// that), mapID (self-only capture makes it redundant with
// profession/race/specialization, which already localize "where" in the
// sense that matters here), or free-text notes (decided against -- too
// much upkeep for a background capture tool).
//
// Backed by a single SQLite file (see EffectDb_Open), holding four
// tables: effects (one row per guid, first-seen-wins on
// guid/block/type -- a later sighting never overwrites them, since
// that would paper over a real finding about the identity rather than
// record one), effect_meta (one row per effect_id -- see below -- holding
// the name/category/description/behavior-default fields that used to
// live on effects directly; several guids of one merged effect share a
// single effect_meta row rather than each carrying their own copy),
// occurrences (one row per distinct tuple per guid, race and
// profession+specialization each folded into their own bitmask rather
// than one row per combination -- see EffectDbRaceMask and
// EffectDbSpecializationMask), and group_members (which type:1/11 "starter" guid
// each guid was seen alongside, keyed by the starter's own (guid,
// duration, a4) since a later cast of the same starter can open a
// differently-membered group). group_members exists because membership
// can't be recovered from occurrences alone: occurrences is deduplicated,
// so a repeat sighting says nothing about whether *that* firing followed
// a live type:1/11 run, and (duration, a4) pairs get reused across
// unrelated effects, so grouping occurrences by a shared (duration, a4)
// would silently merge data that was never actually together. See
// EffectDb_RecordEvent for how all four tables are written, and
// EffectDb_GetGroupsStarted / EffectDb_GetGroupsMemberOf for how
// group_members is read back as a raw membership browse -- not pattern
// detection, which occurrence data is deliberately kept out of (see this
// module's own design discussion).
//
// effect_id / effect_meta: a guid alone isn't the right identity to hang
// a name/category on -- a single curated effect can be backed by several
// guids at once (see EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md), and keying
// name/category storage by guid let those diverge even in a single-user
// local db. Every effects row carries an effect_id; every guid that's
// really "the same effect" shares one. This db's own capture never knows
// about that grouping on its own (a captured event only ever carries one
// guid) -- a brand-new guid always starts as its own singleton effect_id.
// Real multi-guid grouping is decided by whatever populated this db's
// effect_meta rows in the first place (see the handoff doc's "Where
// effect_id comes from" -- this is intentionally out of this module's
// scope).
//--------------------------------------------------------------------------------

#pragma once

#include "game_state.h" //. pulls in Nexus.h (AddonAPI_t) and Mumble.h (EProfession/ERace) together

#include <cassert>
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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDbRaceMask / EffectDb_RaceBit / EffectDb_RacesInMask
//--------------------------------------------------------------------------------
// One bit per Mumble::ERace value, stored on `occurrences` as race_mask
// instead of one full row per race (see the schema writeup up top). uint32_t
// gives headroom past today's 5 known values (Asura..Sylvari), so a future
// ERace addition is just a higher bit -- no schema change, no edit needed
// here.
//
// EffectDb_RaceBit is the single-race bit EffectDb_RecordEvent ORs in on each
// sighting. EffectDb_RacesInMask is the inverse -- every bit set in `mask`,
// unpacked back to ERace values for a caller rendering "races seen" (see
// live_log_ui.cpp / installed_tree_view.cpp). Loops the full 32-bit width, not
// just today's 5, so a future ERace value shows up here without an edit.
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
// applied to profession+specialization together rather than giving
// profession its own independent mask, which would silently lose which
// profession fired which spec on a multi-value row. A nonzero
// specialization id already uniquely implies its owning profession (see
// specialization_info.h), so one merged mask is enough; the only gap is
// specialization == 0 (core build), closed by EffectDb_SpecOrCoreId below
// with reserved pseudo-ids at the *top* of the bit range.
//
// Two uint64_t words rather than one: real GW2 spec ids alone already
// exceed 64 (81 today), and SQLite's bitwise `|` only works cleanly on
// 64-bit ints -- stored as specialization_mask_lo/_hi, each OR'd
// independently on conflict, same shape as race_mask split across two words.
//--------------------------------------------------------------------------------
struct EffectDbSpecializationMask
{
    uint64_t lo = 0;
    uint64_t hi = 0;
};

//_ Ids >= this are reserved profession-only pseudo-ids (see
// EffectDb_SpecOrCoreId above), not real spec ids to look up in
// specialization_info.h.
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
// Inverse of EffectDb_SpecBit -- every bit set in mask, unpacked back to
// raw ids (1..127). Deliberately NOT resolved to a profession/name pair
// here -- that's a display concern (see live_log_ui.cpp /
// installed_tree_view.cpp), not this module's; a caller resolves each id
// via SpecializationProfession()/SpecializationName() for a real id, or
// EffectDb_ProfessionFromCoreOnlyId() for a reserved one.
//--------------------------------------------------------------------------------
std::vector<unsigned int> EffectDb_SpecOrCoreIdsInMask(const EffectDbSpecializationMask& mask);

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

    //_ Resolved to a single EffectDb_SpecBit before storage (see
    // EffectDbSpecializationMask above); kept separate here since that's
    // how GameState_GetProfession/GameState_GetSpecialization produce them.
    Mumble::EProfession  profession{};
    Mumble::ERace         race{};
    unsigned int          specialization = 0;
};

//********************************************************************************
// EffectDbEffect / EffectDbOccurrence
//--------------------------------------------------------------------------------
// Read-side mirrors of the tables above, for the tree overlay and the
// "for science" detail view to consume without touching SQL directly.
// name/categoryPath/description/behavior* all come from effect_meta via
// effect_id -- a join, from this struct's perspective, not a second call.
//--------------------------------------------------------------------------------
struct EffectDbEffect
{
    std::string guid_b64;
    int64_t     effect_id = 0;   //. shared by every guid of one effect -- see file header
    std::string name;
    std::string blockGroup;
    std::string blockMember;
    int         type = -1;       //. -1 = not yet captured, see effect_db.cpp's schema note
    bool        in_json = false; //. presence-only; never written by this module, see effect_db.cpp

    std::vector<std::string> categoryPath;  //. empty = not yet placed anywhere
    std::string description;

    //_ '' = no default set. Only behaviorType/behaviorCaster are
    // meaningful text values; behaviorDuration only means anything when
    // behaviorType == "SetDuration" (same sentinel-by-emptiness
    // convention as everywhere else in this struct, not a separate
    // has-value flag).
    std::string behaviorType;
    std::string behaviorCaster;
    int         behaviorDuration = 0;

    //_ Externally seeded only (see EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md's
    // TODO_B.md item 7) -- a curated Greed-file position for generation
    // to walk in. A capture-discovered guid gets MAX(sort_order)+1 at
    // insert time (see effect_db.cpp's kInsertEffectMeta) so it sorts
    // last rather than jumping ahead of curated content.
    int sortOrder = 0;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
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
    std::vector<std::string> categoryPath;  //. full path, split on the category delimiter
    std::string description;
    int sortOrder = 0;
};

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
// EffectDb_EnsureOpenForBrowsing
//--------------------------------------------------------------------------------
// The DB tab's own entry point -- opens the database if it isn't already,
// WITHOUT going through EffectDb_SetEnabled's gates. Deliberately doesn't
// require EffectDb_GreedFileExists (a db can be real and populated with
// capture toggled off, or even with no Greed.json at all -- see
// EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md: the db is meant to be authoritative
// on its own, independent of "for science" being on), and doesn't touch
// s_enabled either way -- browsing the DB tab must never silently turn
// capture on, and toggling capture off later must not close a connection
// the DB tab still needs open. A no-op if already open (from either this
// or EffectDb_SetEnabled -- whichever got there first owns the connection,
// both read the same file). Returns false (outError filled) only on a
// real open/schema failure, same as EffectDb_Open.
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
// occurrences (see the file-level comment for what each table stores),
// and into group_members too when ev.groupStarterGuid is non-empty. Each
// insert is a silent no-op on a tuple that's already there, so the
// caller never needs to pre-check for duplicates. All three join
// whatever transaction is currently open (see EffectDb_FlushPendingWrites
// for who commits it and when) rather than each call opening and
// committing its own -- a group's starter still always has its own
// effects row written before any member row that references it, since
// they're the same call's writes either way.
//
// Caller must only ever pass self-involved events -- ev.selfMask is
// trusted as-is, never re-derived here. It should always be
// kSelfMaskCaster for now; kSelfMaskTarget/kSelfMaskBoth are reserved
// for once target-watching is added.
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
// Bumped on any write that could change what the DB tab's tree should
// look like: a genuinely new effect row (not a dedup no-op -- see
// EffectDb_RecordEvent), a successful EffectDb_SetName or
// EffectDb_SetCategoryPath, or a real occurrence/group_members write.
//
// That last one used to be deliberately excluded (occurrences seemed too
// high-frequency, and nothing rendered them yet) -- but the DB tab now
// displays occurrence/group detail directly (RenderEffectDbDetail), and
// this is the only remaining reader of this counter (the JSON tab's old
// overlay, which this comment used to also serve, is retired -- see
// EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md). Excluding occurrences meant an
// already-known guid's fresh capture data -- the common case, since most
// real effects arrive pre-seeded -- never showed up until something
// unrelated happened to bump this counter. Bumping on every real write
// is correct now that there's exactly one, cheap-to-rebuild consumer.
//
// Same purpose as GetInstalledTreeGeneration in installed_tree_store.h --
// lets a cache built over this data (the DB tab's own tree) tell "this
// changed" apart from "same generation I already built from", without
// its own invalidation hook.
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
// Every distinct guid that has ever opened a type:1/11 group (i.e. every
// distinct `group_members.starter_guid_b64`), returned as a full
// EffectDbEffect each -- the DB tab's own group-review view lists these
// so a curator can browse straight to "every unnamed starter" without
// already knowing a guid to look one up by (EffectDb_GetGroupsStarted
// requires the starter's guid as input; this is what enumerates them in
// the first place).
//
// A starter guid always has its own `effects` row by construction --
// EffectDb_RecordEvent writes the effects row and the group_members row
// for one event together, never one without the other -- so a lookup
// failure here would mean a real data inconsistency, not an expected
// case; such a guid is silently skipped rather than surfaced specially,
// since there's nothing more useful a browsing view could do with it.
//
// Not on the hot path (called once per DB-tab-generation change, same
// gating as EffectDb_GetAllEffects's own caller), so an ad-hoc two-step
// query (list distinct starters, then EffectDb_GetEffect each) is fine --
// reuses that already-tested lookup rather than duplicating its column
// list in a second hand-written JOIN.
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
// Both read group_members (see the file-level comment for why it exists
// and what it does/doesn't let you reconstruct).
//
//  - GetGroupsStarted(guid_b64): every (duration, a4) instance where
//    guid_b64 was the *starter*, with the full member set recorded
//    under that instance -- including guid_b64 itself.
//  - GetGroupsMemberOf(guid_b64): every (starter guid, duration, a4)
//    instance where guid_b64 was a member of a group *someone else*
//    started; a guid's own group is already covered above, so it's
//    excluded here rather than listed in both.
//
// Both empty if guid_b64 has no group_members rows. Ordered by
// (duration, a4) [/ starter guid] for stable, deterministic iteration.
//--------------------------------------------------------------------------------
std::vector<EffectDbGroupInstance>   EffectDb_GetGroupsStarted(const std::string& guid_b64);
std::vector<EffectDbGroupMembership> EffectDb_GetGroupsMemberOf(const std::string& guid_b64);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_SetName
//--------------------------------------------------------------------------------
// SQL is the sole source of truth for an effect's name -- this never
// touches, and is never touched by, any installed sin's JSON (see
// EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md: the JSON tab(s) and the db's own
// view are fully independent, neither reads nor writes the other). Takes
// a guid_b64 for caller convenience (that's what a tree click/quick-edit
// naturally has on hand), but resolves it to that guid's effect_id first
// and writes effect_meta -- so renaming any one guid of a multi-guid
// effect renames the whole effect, every sibling guid included, not just
// the one clicked. No-op (returns false) if guid_b64 isn't known yet.
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