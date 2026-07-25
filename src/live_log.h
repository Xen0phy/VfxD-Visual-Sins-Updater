#pragma once
#include "game_state.h" // pulls in Nexus.h (AddonAPI_t) and Mumble.h (EProfession/ERace) together
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Live effect-log capture over the Nexus event bridge (vfxd_sins_bridge.h),
// consumed from VfxDenoiser's log_effect once its side of the patch exists.
// Until then, the standalone stub addon (see HANDOFF's build order) raises
// the same EV_VFXD_SINS_LOG events this listens for, so this module and its
// UI can be built and tested with zero dependency on VfxDenoiser's repo.
//
// One entry per GUID, not one per event -- repeated events for the same
// effect overwrite that entry's fields ("latest wins") rather than
// appending, which is what keeps this bounded regardless of how often an
// effect re-fires. See HANDOFF_VfxSins.md's "Live log display" section for
// the full design writeup this implements.
//
// Same overall shape as report.h: the logic here doesn't reach into
// addon.cpp's statics directly (s_installedJson etc.) -- the guid-to-name
// lookup it checks incoming events against is passed in via
// LiveLog_SetKnownGuidNames, same "passed in, not read from statics"
// shape used elsewhere in this addon.
// ---------------------------------------------------------------------------

// Number of distinct numeric "type:" values seen in infostr so far (0-11
// inclusive). Not necessarily exhaustive -- see LiveLog_GetTypeEnabled's
// fail-open behavior below for a type outside this range.
inline constexpr int kLiveLogTypeCount = 12;

struct LiveLogEntry
{
    std::string guid_b64;
    bool        knownInSin = false;
    std::string displayName;    // sin effect name if knownInSin, else guid_b64

    // Properly typed per log_effect's real signature (see HANDOFF's
    // "Ground truth" section) -- duration is signed (can be negative), a4
    // is always numeric (never "self"/"null"), and every one of these six
    // is a single whitespace-delimited token in infostr with no embedded
    // spaces, ever.
    int          type     = 0;
    int          duration = 0;
    unsigned int a4       = 0;
    std::string  caster;   // "self" / "null" / stringified agent ID
    std::string  a6;       // "self" / "null" / stringified agent ID
    std::string  target;   // "self" / "null" / stringified agent ID

    // `behavior` (VfxDenoiser's OWN trailing " -> X" resolution against ITS
    // OWN installed sin file) is fully removed -- not parsed, not stored,
    // not rendered. installedBehavior below, checked against this user's
    // own installed sin file(s), is the only behavior source shown.

    // This user's own currently-configured behavior for this guid, looked
    // up independently against the receiver's installed sin JSON (same
    // guid/effect entry displayName already comes from) -- never derived
    // from the incoming event. Empty if knownInSin is false, or if the
    // matching effect has no "behaviors" entries of its own.
    std::string installedBehavior;

    // Assigned once, the first time this guid is ever seen -- never
    // touched again on subsequent "latest wins" updates. Used to render
    // entries in the order they were first received rather than any
    // re-derived ordering (alphabetical, etc.), so the list doesn't
    // reshuffle every time an existing entry's fields update.
    int firstSeenSeq = 0;

    int seenCount = 0;

    // Self-only enrichment, snapshotted at ingestion time (not re-read live
    // at render time) whenever the incoming event has caster=="self" or
    // target=="self" -- matching VfxDenoiser's own pointer-identity
    // self-check exactly. A non-self event for a guid that already has
    // these populated leaves them completely untouched: not cleared, not
    // overwritten. mapID/profession/specialization prefer RTAPI (if live)
    // and fall back to Mumble; race is always Mumble, since RTAPI has no
    // race field at all. No source tag is kept per-field or per-entry
    // (explicitly decided against).
    unsigned int        mapID          = 0;
    Mumble::ERace        race{};
    Mumble::EProfession  profession{};
    unsigned int        specialization = 0;

    // Set true the first time this guid ever has a self-event, then never
    // cleared again -- distinguishes "genuinely never observed" from a
    // zero-initialized/stale snapshot. Needed because the fields above
    // stay untouched (not reset) on a later non-self event for the same
    // guid, e.g. the same effect logged by/against someone else -- the
    // "Self (last seen)" UI section stays visible once this is true, even
    // if the entry's *current* caster/target is no longer "self".
    bool hasSelfContext = false;
};

// Subscribes to EV_VFXD_SINS_LOG. Call once from Addon_Load, after aApi is
// available. Safe to call even if nothing ever raises that event yet (e.g.
// before the stub addon or VfxDenoiser's patch exist) -- this only
// registers a callback, it doesn't require a sender to be present.
void LiveLog_Init(AddonAPI_t* aApi);

// Unsubscribes, and raises EV_VFXD_SINS_LISTEN_STOP if listening was still
// active -- so an addon unload/crash while capturing doesn't leave the
// sender thinking someone's still listening. Call from Addon_Unload. `aApi`
// may be null if Addon_Load was never reached; this is then a no-op.
void LiveLog_Shutdown(AddonAPI_t* aApi);

// Flips the listen state and raises EV_VFXD_SINS_LISTEN_START/STOP
// accordingly. Only actually raises the notification on a real state
// change -- calling this with the value it's already at is a no-op past
// the first time, so a UI checkbox re-rendering every frame doesn't spam
// the event bus.
void LiveLog_SetListening(AddonAPI_t* aApi, bool listening);
bool LiveLog_IsListening();

// While true, an incoming event whose guid already resolves to a known sin
// effect (per the map last passed to LiveLog_SetKnownGuidNames) is dropped
// before it ever becomes/updates an entry in LiveLog_GetEntries() -- not
// filtered at render time, nothing written in the background for it.
// Turning this off does not retroactively repopulate what was dropped
// while it was on; only new incoming events are affected from that point
// forward.
void LiveLog_SetHideKnown(bool hide);
bool LiveLog_GetHideKnown();

// Whether events whose infostr "type:" field equals `type` (0-11,
// VfxDenoiser's own numeric effect-type) are logged at all. Checked at
// ingestion -- disabling a type drops matching events before they ever
// become/update an entry, same "drop on arrival, not filter on display"
// shape as the hideKnown toggle above. Defaults are the built-in ones from
// characterizing captured data (see live_log.cpp) and are deliberately NOT
// persisted across an addon reload -- these are exploratory filters for
// figuring out what each type actually is, not settings meant to stick.
// `type` outside [0, kLiveLogTypeCount) is a no-op for the setter and
// reads back as enabled (fail open, in case a 12th type ever shows up).
bool LiveLog_GetTypeEnabled(int type);
void LiveLog_SetTypeEnabled(int type, bool enabled);

// Refreshes the guid -> sin-effect-name lookup new events are checked
// against. Cheap to call every frame the Live Log section is open; the
// caller is responsible for having actually loaded the installed effects
// tree at least once first (same lazy-load-on-first-open pattern
// RenderInstalledEffects/RenderReportSection already use).
void LiveLog_SetKnownGuidNames(const std::unordered_map<std::string, std::string>& guidToName);

// Refreshes the guid -> this-user's-own-configured-behavior lookup, the
// same independent-of-the-event resolution LiveLog_SetKnownGuidNames does
// for names. Same calling convention: cheap to call every frame the Live
// Log section is open, caller is responsible for the installed tree having
// been loaded at least once first.
void LiveLog_SetKnownGuidBehaviors(const std::unordered_map<std::string, std::string>& guidToBehavior);

const std::unordered_map<std::string, LiveLogEntry>& LiveLog_GetEntries();

// Drops everything captured so far. Does not touch the listen toggle.
void LiveLog_Clear();