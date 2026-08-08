//################################################################################
// live_log.h
//--------------------------------------------------------------------------------
// Live effect-log capture over the Nexus event bridge (vfxd_sins_bridge.h),
// consumed from VfxDenoiser's log_effect once its side of the patch
// exists. Until then, the standalone stub addon raises the same
// EV_VFXD_SINS_LOG events this listens for, so this module and its UI can
// be built and tested with zero dependency on VfxDenoiser's repo.
//
// Same overall shape as report.h: the logic here doesn't reach into
// addon.cpp's statics directly -- the guid-to-name lookup incoming events
// are checked against is passed in via LiveLog_SetKnownGuidNames, same
// "passed in, not read from statics" shape used elsewhere in this addon.
//--------------------------------------------------------------------------------

#pragma once

#include "game_state.h" //. pulls in Nexus.h (AddonAPI_t) and Mumble.h (EProfession/ERace) together

#include <string>
#include <unordered_map>
#include <vector>

//_ Number of distinct infostr "type:" values seen so far (0-11 inclusive)
// -- not necessarily exhaustive; see LiveLog_GetTypeEnabled's fail-open
// behavior below.
inline constexpr int kLiveLogTypeCount = 12;

//_ Cap on LiveLogEntry::recentGroupIds -- a guid that keeps drifting
// through new groups only needs to show "this has happened more than
// once, recently", not a full unbounded history.
inline constexpr size_t kLiveLogGroupHistoryCap = 3;

//********************************************************************************
// LiveLogEntry
//--------------------------------------------------------------------------------
// guid_b64             base64 GUID identifying the effect
// knownInSin            true if guid resolves to a known sin effect
// displayName           sin effect name if knownInSin, else guid_b64
// type                  numeric infostr "type:" value (see kLiveLogTypeCount)
// duration              signed; can be negative
// a4                    numeric infostr "a4:" value
// caster / a6 / target   "self" / "null" / stringified agent ID
// installedBehavior     this user's own configured behavior for this guid
// firstSeenSeq          assigned once, on this guid's first sighting
// seenCount             number of events folded into this entry
// groupId                which type:1/11 group this guid belongs to, -1
//                        if none; content-addressed, see AdvanceGroupState.
// recentGroupIds         distinct groups this guid has belonged to
//                        recently, oldest first, capped at kLiveLogGroupHistoryCap.
// mapID / race / profession / specialization   self-only, see below
// hasSelfContext        true once this guid has ever had a self-event
//--------------------------------------------------------------------------------
// One entry per guid; VfxDenoiser's OWN trailing " -> X" resolution
// (`behavior`) is fully removed -- not parsed, stored, or rendered.
// installedBehavior, looked up independently against this user's own
// installed sin JSON, is the only behavior source shown.
//
// mapID/race/profession/specialization/hasSelfContext are snapshotted
// once, the first time this guid has an event with caster or target ==
// "self" (matching VfxDenoiser's own pointer-identity self-check
// exactly), then left completely untouched on every later non-self event
// for the same guid -- hasSelfContext, once true, never clears again, so
// the "Self (last seen)" UI section stays visible even after a later
// non-self event. mapID/profession/specialization prefer RTAPI (if live),
// falling back to Mumble; race is always Mumble. No per-field/per-entry
// source tag is kept.
//--------------------------------------------------------------------------------
struct LiveLogEntry
{
    std::string guid_b64;
    bool        knownInSin = false;
    std::string displayName;

    int          type     = 0;
    int          duration = 0;
    unsigned int a4       = 0;
    std::string  caster;
    std::string  a6;
    std::string  target;

    std::string installedBehavior;

    int firstSeenSeq = 0;
    int seenCount = 0;

    int groupId = -1;   //. -1 = not currently part of a group; see live_log.cpp AdvanceGroupState
    std::vector<int> recentGroupIds;   //. see comment above

    unsigned int        mapID          = 0;
    Mumble::ERace        race{};
    Mumble::EProfession  profession{};
    unsigned int        specialization = 0;

    bool hasSelfContext = false;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_Init / LiveLog_Shutdown
//--------------------------------------------------------------------------------
// Lifecycle pair. Init subscribes to EV_VFXD_SINS_LOG -- call once from
// Addon_Load after aApi is available; safe even if nothing ever raises
// that event yet. Shutdown unsubscribes and raises
// EV_VFXD_SINS_LISTEN_STOP if listening was still active, so an addon
// unload/crash while capturing doesn't leave the sender thinking someone's
// still listening. aApi may be null if Addon_Load was never reached.
//--------------------------------------------------------------------------------
void LiveLog_Init(AddonAPI_t* aApi);
void LiveLog_Shutdown(AddonAPI_t* aApi);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_SetListening / LiveLog_IsListening
//--------------------------------------------------------------------------------
// Flips the listen state and raises EV_VFXD_SINS_LISTEN_START/STOP
// accordingly -- only on a real state change, so a UI checkbox
// re-rendering every frame doesn't spam the event bus.
//--------------------------------------------------------------------------------
void LiveLog_SetListening(AddonAPI_t* aApi, bool listening);
bool LiveLog_IsListening();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_SetHideKnown / LiveLog_GetHideKnown
//--------------------------------------------------------------------------------
// While true, an incoming event whose guid already resolves to a known
// sin effect is dropped before it ever becomes/updates an entry -- not
// filtered at render time. Turning it off doesn't retroactively
// repopulate what was already dropped; only new events are affected.
//--------------------------------------------------------------------------------
void LiveLog_SetHideKnown(bool hide);
bool LiveLog_GetHideKnown();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_GetTypeEnabled / LiveLog_SetTypeEnabled
//--------------------------------------------------------------------------------
// Whether events whose infostr "type:" equals `type` (0-11) are logged at
// all, checked at ingestion -- same drop-on-arrival shape as
// SetHideKnown. Defaults come from characterizing captured data (see
// live_log.cpp) and are deliberately not persisted across a reload --
// exploratory filters, not settings meant to stick. `type` outside
// [0, kLiveLogTypeCount) is a no-op for the setter and reads back enabled
// (fail open, in case a 12th type ever shows up).
//--------------------------------------------------------------------------------
bool LiveLog_GetTypeEnabled(int type);
void LiveLog_SetTypeEnabled(int type, bool enabled);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_SetKnownGuidNames / LiveLog_SetKnownGuidBehaviors
//--------------------------------------------------------------------------------
// Refresh the guid -> sin-effect-name and guid -> this-user's-own-
// configured-behavior lookups new events are checked against. Cheap to
// call every frame the Live Log section is open; the caller is
// responsible for having loaded the installed effects tree at least once
// first (same lazy-load-on-first-open pattern used elsewhere in this addon).
//--------------------------------------------------------------------------------
void LiveLog_SetKnownGuidNames(const std::unordered_map<std::string, std::string>& guidToName);
void LiveLog_SetKnownGuidBehaviors(const std::unordered_map<std::string, std::string>& guidToBehavior);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_GetEntries / LiveLog_GetForScienceEntries / LiveLog_Clear
//--------------------------------------------------------------------------------
// Two separate stores, both keyed by guid_b64:
//
//  - LiveLog_GetEntries: the ordinary display fold, subject to
//    s_typeEnabled ("Types logged") and hideKnown -- what the Live Log
//    panel normally shows.
//  - LiveLog_GetForScienceEntries: the effect db's own capture stream,
//    mirrored exactly -- self (caster) events only, every type
//    regardless of "Types logged", only while EffectDb_IsEnabled(). Not
//    filtered by hideKnown either, since that filter is about *this
//    user's own sin JSON*, an unrelated concern to what the db is
//    recording. Every guid that ever appears here already has an
//    EFFECTS row by construction (see EffectDb_IsKnownGuid) -- it got
//    here because the same event that updated this map also fed
//    EffectDb_RecordEvent.
//
// Clear drops both stores (and both firstSeenSeq counters) without
// touching the listen toggle or the effect db itself -- this only
// clears the on-screen lists, never anything on disk.
//--------------------------------------------------------------------------------
const std::unordered_map<std::string, LiveLogEntry>& LiveLog_GetEntries();
const std::unordered_map<std::string, LiveLogEntry>& LiveLog_GetForScienceEntries();
void LiveLog_Clear();