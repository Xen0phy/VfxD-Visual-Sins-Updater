//################################################################################
// vfxd_sins_bridge.h
//--------------------------------------------------------------------------------
// EV_VFXD_SINS_LISTEN_START/STOP   live-capture toggle notifications (no payload)
// EV_VFXD_SINS_LOG                 raised once per logged effect while listening
// kVfxSinsLogEventVersion          current EV_VFXD_SINS_LOG payload version
// VfxSinsLogEvent                  the payload struct itself
//--------------------------------------------------------------------------------
// Wire contract between VfxDenoiser and VfxDSinsUpdater for live effect-log
// capture, carried over Nexus's Events_Raise/Events_Subscribe instead of a
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
//--------------------------------------------------------------------------------

#pragma once

#include <cstdint>

//_ Raised when live-capture toggles on/off; log_effect only emits EV_VFXD_SINS_LOG while listening, so logged_effects stays bounded
inline constexpr const char* EV_VFXD_SINS_LISTEN_START = "EV_VFXD_SINS_LISTEN_START";
inline constexpr const char* EV_VFXD_SINS_LISTEN_STOP  = "EV_VFXD_SINS_LISTEN_STOP";

//_ One per logged effect while listening; payload is valid only for the callback -- copy strings out immediately
inline constexpr const char* EV_VFXD_SINS_LOG = "EV_VFXD_SINS_LOG";

//_ Bump on struct layout change; readers should ignore an unrecognized struct_version instead of guessing at layout
inline constexpr uint32_t kVfxSinsLogEventVersion = 1;

extern "C" {

//********************************************************************************
// VfxSinsLogEvent
//--------------------------------------------------------------------------------
// struct_version   = kVfxSinsLogEventVersion for this shape
// guid_b64         guid_to_base64(effectDef->guid) -- same encoding as
//                   VfxDenoiser's own log.txt
// info             the already-built infostr, verbatim, from before
//                   log_file's own found_effect/behavior double-append
//--------------------------------------------------------------------------------
// The EV_VFXD_SINS_LOG payload -- see the file header above for its
// lifetime and cross-DLL layout rules.
//--------------------------------------------------------------------------------
struct VfxSinsLogEvent
{
    uint32_t    struct_version;
    const char* guid_b64;
    const char* info;
};

} //. extern "C"