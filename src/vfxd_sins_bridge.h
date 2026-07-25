#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Wire contract between VfxDenoiser and VfxDSinsUpdater for live effect-log
// capture, carried over Nexus's Events_Raise/Events_Subscribe rather than a
// network endpoint -- both addons already load through Nexus and get an
// AddonAPI_t* with events built in, so there's no socket/port to stand up or
// secure.
//
// This header's *contents* are meant to end up duplicated verbatim on all
// three sides that need it (this addon, VfxDenoiser's eventual fork, and the
// standalone stub addon used to test this side before that fork exists) --
// not shared as an actual build/source dependency between two otherwise-
// unrelated projects. Keep it POD and pass it by pointer across the DLL
// boundary only for the duration of a synchronous callback; don't assume the
// two sides were compiled with the same toolchain beyond "this specific
// struct's layout matches."
//
// See HANDOFF_VfxSins.md, to-do item 1, for the full design writeup this
// implements.
// ---------------------------------------------------------------------------

// Notifications (no payload), raised by VfxDSinsUpdater whenever its live-
// capture UI toggle changes state. VfxDenoiser's patched log_effect only
// raises EV_VFXD_SINS_LOG while listening is active on our side -- this is
// what keeps its own logged_effects list from growing unbounded while we're
// capturing instead.
inline constexpr const char* EV_VFXD_SINS_LISTEN_START = "EV_VFXD_SINS_LISTEN_START";
inline constexpr const char* EV_VFXD_SINS_LISTEN_STOP  = "EV_VFXD_SINS_LISTEN_STOP";

// Raised by VfxDenoiser's log_effect (or, until that patch exists, by the
// stub addon), one per call, only while listening is active. Payload is a
// VfxSinsLogEvent* -- valid only for the duration of the subscriber
// callback. Copy the strings out immediately rather than holding the
// pointer: log_effect likely runs on the game's render/update thread, so
// Events_Raise (and this callback) fires synchronously there too.
inline constexpr const char* EV_VFXD_SINS_LOG = "EV_VFXD_SINS_LOG";

// Current payload shape. Bump this if the fields below ever change, and
// have readers on both sides ignore anything with a struct_version they
// don't recognize rather than guessing at a layout that may not match.
inline constexpr uint32_t kVfxSinsLogEventVersion = 1;

extern "C" {

struct VfxSinsLogEvent
{
    uint32_t    struct_version; // = kVfxSinsLogEventVersion for this shape
    const char* guid_b64;       // guid_to_base64(effectDef->guid) -- same encoding VfxDenoiser's own log.txt already uses
    const char* info;           // the already-built infostr, verbatim, from before log_file's own found_effect/behavior double-append
};

} // extern "C"
