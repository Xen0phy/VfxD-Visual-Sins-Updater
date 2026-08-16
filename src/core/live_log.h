//################################################################################
// live_log.h
//--------------------------------------------------------------------------------
// Live effect-log capture over the Nexus event bridge (vfxd_sins_bridge.h),
// consumed from VfxDenoiser's log_effect once its side of the patch exists. Until
// then, the standalone stub addon raises the same EV_VFXD_SINS_LOG events this
// listens for, so this module and its UI can be built and tested with zero
// dependency on VfxDenoiser's repo.
//
// Same overall shape as report.h: the logic here doesn't reach into addon.cpp's
// statics directly -- the guid-to-name lookup incoming events are checked against
// is passed in via LiveLog_SetKnownGuidNames, same "passed in, not read from
// statics" shape used elsewhere in this addon.
//--------------------------------------------------------------------------------

#pragma once

#include "Mumble.h"
#include "Nexus.h"

#include <string>
#include <unordered_map>
#include <vector>

//_ Number of distinct infostr "type:" values seen so far (0-11 inclusive)
inline constexpr int kLiveLogTypeCount = 12;

//_ Cap on LiveLogEntry::recentGroupIds -- length of recent-group history kept
inline constexpr size_t kLiveLogGroupHistoryCap = 3;

//********************************************************************************
// LiveLogEntry
//--------------------------------------------------------------------------------
// guid_b64/knownInSin/displayName   identity: guid, known-in-sin flag, name
// type/duration/a4/caster/a6/target   raw infostr fields, target may be self
// installedBehavior                 this user's own configured behavior
// firstSeenSeq/seenCount            bookkeeping: first-seen order, event count
// groupId/recentGroupIds            type:1/11 grouping state, see fields below
// mapID/race/profession/specialization/hasSelfContext   self-context snapshot
//--------------------------------------------------------------------------------
// One entry per guid; VfxDenoiser's own trailing " -> X" resolution is fully
// removed -- installedBehavior, checked against this user's installed sin JSON,
// is the only behavior source shown.
//--------------------------------------------------------------------------------
struct LiveLogEntry
{
    std::string guid_b64;
    bool        knownInSin = false;
    std::string displayName;

    int          type     = 0;   //. infostr "type:" value, 0-11
    int          duration = 0;   //. signed, can be negative
    unsigned int a4       = 0;   //. infostr "a4:" value
    std::string  caster;         //. self/null/stringified agent ID
    std::string  a6;
    std::string  target;         //. self/null/stringified agent ID

    std::string installedBehavior;   //. this user's own configured behavior

    int firstSeenSeq = 0;   //. assigned on first sighting
    int seenCount = 0;      //. events folded into this entry

    int groupId = -1;   //. -1 = no group
    std::vector<int> recentGroupIds;   //. recent groups, oldest first, capped

    //_ mapID/profession/specialization prefer RTAPI when live, else Mumble
    unsigned int        mapID          = 0;
    Mumble::ERace        race{};   //. always from Mumble
    Mumble::EProfession  profession{};
    unsigned int        specialization = 0;

    bool hasSelfContext = false;   //. sticky once true
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_Init / LiveLog_Shutdown
//--------------------------------------------------------------------------------
// Lifecycle pair. Init subscribes to EV_VFXD_SINS_LOG -- call once from
// Addon_Load after aApi is available; safe even if nothing ever raises that event
// yet. Shutdown unsubscribes and raises EV_VFXD_SINS_LISTEN_STOP if listening was
// still active, so an addon unload/crash while capturing doesn't leave the sender
// thinking someone's still listening. aApi may be null if Addon_Load was never
// reached.
//--------------------------------------------------------------------------------
void LiveLog_Init(AddonAPI_t* aApi);
void LiveLog_Shutdown(AddonAPI_t* aApi);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_SetListening / LiveLog_IsListening
//--------------------------------------------------------------------------------
// Flips the listen state and raises EV_VFXD_SINS_LISTEN_START/STOP accordingly --
// only on a real state change, so a UI checkbox re-rendering every frame doesn't
// spam the event bus.
//--------------------------------------------------------------------------------
void LiveLog_SetListening(AddonAPI_t* aApi, bool listening);
bool LiveLog_IsListening();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_SetHideKnown / LiveLog_GetHideKnown
//--------------------------------------------------------------------------------
// While true, an incoming event whose guid already resolves to a known sin effect
// is dropped before it ever becomes/updates an entry -- not filtered at render
// time. Turning it off doesn't retroactively repopulate what was already dropped;
// only new events are affected.
//--------------------------------------------------------------------------------
void LiveLog_SetHideKnown(bool hide);
bool LiveLog_GetHideKnown();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_GetTypeEnabled / LiveLog_SetTypeEnabled
//--------------------------------------------------------------------------------
// Whether events whose infostr "type:" equals `type` (0-11) are logged at all,
// checked at ingestion -- same drop-on-arrival shape as SetHideKnown. Defaults
// come from characterizing captured data (see live_log.cpp) and are not persisted
// across a reload -- exploratory filters, not settings meant to stick. `type`
// outside [0, kLiveLogTypeCount) is a no-op for the setter and reads back enabled
// (fail open, in case a 12th type ever shows up).
//--------------------------------------------------------------------------------
bool LiveLog_GetTypeEnabled(int type);
void LiveLog_SetTypeEnabled(int type, bool enabled);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_SetKnownGuidNames / LiveLog_SetKnownGuidBehaviors
//--------------------------------------------------------------------------------
// Refresh the guid -> sin-effect-name and guid -> this-user's-own-configured-
// behavior lookups new events are checked against. Cheap to call every frame the
// Live Log section is open; the caller is responsible for having loaded the
// installed effects tree at least once first (same lazy-load-on-first-open
// pattern used elsewhere in this addon).
//--------------------------------------------------------------------------------
void LiveLog_SetKnownGuidNames(const std::unordered_map<std::string, std::string>& guidToName);
void LiveLog_SetKnownGuidBehaviors(const std::unordered_map<std::string, std::string>& guidToBehavior);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_GetEntries / LiveLog_GetForScienceEntries / LiveLog_Clear
//--------------------------------------------------------------------------------
// LiveLog_GetEntries is the ordinary display fold, filtered by s_typeEnabled
// ("Types logged") and hideKnown -- what the Live Log panel normally shows.
// LiveLog_GetForScienceEntries mirrors the effect db's own capture stream: self
// (caster) events only, every type, only while EffectDb_IsEnabled(), not filtered
// by hideKnown since that filter targets this user's own sin JSON, an unrelated
// concern. Every guid here already has an EFFECTS row (see EffectDb_IsKnownGuid),
// fed by the same event via EffectDb_RecordEvent. Clear drops both stores and
// both firstSeenSeq counters without touching the listen toggle or the effect db
// -- it only clears the on-screen lists, never anything on disk.
//--------------------------------------------------------------------------------
const std::unordered_map<std::string, LiveLogEntry>& LiveLog_GetEntries();
const std::unordered_map<std::string, LiveLogEntry>& LiveLog_GetForScienceEntries();
void LiveLog_Clear();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LiveLog_SetForScienceOnly / LiveLog_GetForScienceOnly
//--------------------------------------------------------------------------------
// While true, IngestLogLine skips its own ordinary-display fold (the
// LiveLog_GetEntries() store) entirely, before the "Types logged"/hideKnown
// checks run, so that store neither grows nor gets touched. FeedEffectDb and
// UpdateForScienceEntry (the "for science" db write and its own display twin,
// LiveLog_GetForScienceEntries()) run unconditionally either way, ahead of this
// check. This lets "for science" run as pure background capture without the
// ordinary live log panel accumulating and rendering entries nobody is viewing.
// Independent of EffectDb_IsEnabled()/LiveLog_IsListening() -- only changes what
// IngestLogLine does once an event arrives, not whether events arrive or reach
// the db; live_log_ui.cpp ties the two into one convenience toggle.
//--------------------------------------------------------------------------------
void LiveLog_SetForScienceOnly(bool forScienceOnly);
bool LiveLog_GetForScienceOnly();