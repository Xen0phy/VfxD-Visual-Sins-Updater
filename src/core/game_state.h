#pragma once
#include "Nexus.h"
#include <string>

// Mumble.h declares two members that share a name with their own type
// (Context::Compass and Data::Context), which GCC (incl. the project's
// x86_64-w64-mingw32-g++) rejects as a hard error by default
// (-Wchanges-meaning), not merely a style warning -- confirmed against the
// actual header, not assumed. Scoped to just this include rather than
// edited into the provided header (kept byte-identical for future upstream
// updates) or loosened project-wide via -fpermissive, which would also
// mask unrelated real errors elsewhere in the build.
#include "Mumble.h"

#include "RTAPI.hpp"

// ---------------------------------------------------------------------------
// Standalone Mumble Link / RTAPI reader. Not specific to the live log --
// this exists so any consumer (live log enrichment now, the report feature
// later) reads game state through one place, with the RTAPI-preferred /
// Mumble-fallback rule defined exactly once rather than duplicated per
// caller. Mirrors the DataLink access pattern already used by Nexus-based
// addons; caches the DataLink pointers once rather than re-fetching them
// every call.
//
// Source selection, per field:
//   MapID          -- RTAPI if live, else Mumble.
//   Profession     -- RTAPI if live, else Mumble. Numeric mapping confirmed
//                     equivalent (both 0=None/Unknown, 1-9 same order).
//   Specialization -- RTAPI if live, else Mumble. Cross-source equivalence
//                     CONFIRMED by hand-testing (both resolve to the same
//                     specialization name via specialization_names.cpp's
//                     table for the same live character). No per-value
//                     source tag is kept (explicitly decided against).
//   Race           -- ALWAYS Mumble. Not a fallback -- RTAPI has no race
//                     field at all, same shape as the reference project's
//                     IsMapOpen (always-Mumble because RTAPI doesn't expose
//                     it).
//
// Identity fields (Profession/Specialization/Race) are read from Nexus's
// DL_MUMBLE_LINK_IDENTITY, which Nexus itself parses out of Data::Identity
// (a wchar_t JSON blob) into a real Mumble::Identity struct -- NOT by
// JSON-parsing Data::Identity ourselves. MapID is a real struct field on
// Data::Context, no parsing needed either way.
// ---------------------------------------------------------------------------

// Caches the DataLink pointers. Call once from Addon_Load, after aApi is
// available -- same calling convention as LiveLog_Init. Safe to call even
// if Mumble/RTAPI aren't present yet; DataLink_Get returning nullptr just
// means the corresponding Get* functions below fall back or return
// zero-valued defaults.
void GameState_Init(AddonAPI_t* aApi);

// Clears the cached pointers. Call from Addon_Unload. Safe to call even if
// GameState_Init was never reached.
void GameState_Shutdown();

// True if RTAPI's shared block is present and actually live -- GameBuild is
// documented as set to 0 when RTAPI is unloaded, so presence of the block
// alone isn't enough.
bool GameState_IsRTAPILive();

// True if Nexus's Mumble Link identity block is present at all. Useful for
// callers that need to distinguish "no data available" from "value is
// genuinely the zero/first enum entry."
bool GameState_HasMumbleIdentity();

unsigned int        GameState_GetMapID();
Mumble::EProfession  GameState_GetProfession();
unsigned int        GameState_GetSpecialization();
Mumble::ERace        GameState_GetRace(); // always Mumble -- see note above

// Human-readable names for the two enum fields that Mumble.h itself already
// names every value of (EProfession: None..Revenant, ERace: Asura..Sylvari)
// -- no external ID table needed for these two, unlike Specialization (see
// specialization_names.h). Always returns a valid non-null string, even for
// a value outside the enum's defined range (falls back to "Unknown").
const char* GameState_ProfessionName(Mumble::EProfession profession);
const char* GameState_RaceName(Mumble::ERace race);

// Report-time-only reads -- both are read fresh only when the report form
// composes its reporter line, never cached or stored on a LiveLogEntry.
//
// Account name: RTAPI-only, no Mumble equivalent exists at all. Empty if
// RTAPI isn't currently live.
//
// Character name: Mumble's Identity.Name (the field actually sized/
// positioned for a character name -- NOT Data.Name, which is the
// Mumble-Link *application* identifier, e.g. "Guild Wars 2") if RTAPI
// isn't live, else RTAPI's RealTimeData::CharacterName -- same
// RTAPI-preferred/Mumble-fallback rule used by every other field in this
// module. Empty if neither source is available.
std::string GameState_GetAccountName();
std::string GameState_GetCharacterName();