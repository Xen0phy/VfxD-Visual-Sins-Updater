//################################################################################
// game_state.h
//--------------------------------------------------------------------------------
// Standalone Mumble Link / RTAPI reader, not specific to the live log --
// any consumer (live log enrichment now, the report feature later) reads
// game state through one place, with the RTAPI-preferred/Mumble-fallback
// rule defined exactly once. Mirrors the DataLink access pattern already
// used by Nexus-based addons; caches the DataLink pointers once rather
// than re-fetching them every call.
//
// Source selection, per field:
//   MapID/Profession/Specialization -- RTAPI if live, else Mumble.
//     Profession's numeric mapping and Specialization's cross-source
//     equivalence are both confirmed to agree between the two sources.
//   Race -- always Mumble; RTAPI has no race field at all.
//
// Identity fields (Profession/Specialization/Race) come from Nexus's
// DL_MUMBLE_LINK_IDENTITY, which Nexus itself parses into a real
// Mumble::Identity struct -- not JSON-parsed here. MapID is a plain
// struct field on Data::Context, no parsing needed either way.
//--------------------------------------------------------------------------------

#pragma once
//_ Mumble.h has two members that share a name with their own type, which
// GCC rejects as a hard error (-Wchanges-meaning) -- scoped to this
// include rather than edited into the vendored header or loosened project-wide.
#include "Mumble.h"
#include "Nexus.h"
#include "RTAPI.hpp"

#include <string>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_Init / GameState_Shutdown
//--------------------------------------------------------------------------------
// Lifecycle pair. Init caches the DataLink pointers -- call once from
// Addon_Load after aApi is available, same convention as LiveLog_Init.
// Safe even if Mumble/RTAPI aren't present yet. Shutdown clears the
// cached pointers and is safe even if Init was never reached.
//--------------------------------------------------------------------------------
void GameState_Init(AddonAPI_t* aApi);
void GameState_Shutdown();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_IsRTAPILive
//--------------------------------------------------------------------------------
// True if RTAPI's shared block is present and actually live -- GameBuild
// is documented as set to 0 when RTAPI is unloaded, so presence of the
// block alone isn't enough.
//--------------------------------------------------------------------------------
bool GameState_IsRTAPILive();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_HasMumbleIdentity
//--------------------------------------------------------------------------------
// True if Nexus's Mumble Link identity block is present at all -- lets
// callers distinguish "no data available" from "value is genuinely the
// zero/first enum entry".
//--------------------------------------------------------------------------------
bool GameState_HasMumbleIdentity();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_GetMapID / GameState_GetProfession / GameState_GetSpecialization /
// GameState_GetRace
//--------------------------------------------------------------------------------
// Per-field accessors. MapID/Profession/Specialization are RTAPI-if-live,
// else Mumble. Race is always Mumble -- not a fallback, RTAPI has no race
// field at all.
//--------------------------------------------------------------------------------
unsigned int         GameState_GetMapID();
Mumble::EProfession   GameState_GetProfession();
unsigned int         GameState_GetSpecialization();
Mumble::ERace         GameState_GetRace();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_ProfessionName / GameState_RaceName
//--------------------------------------------------------------------------------
// Human-readable names for the two enum fields Mumble.h itself already
// names every value of -- no external ID table needed, unlike
// Specialization (see specialization_names.h). Always returns a valid
// non-null string, falling back to "Unknown" outside the enum's range.
//--------------------------------------------------------------------------------
const char* GameState_ProfessionName(Mumble::EProfession profession);
const char* GameState_RaceName(Mumble::ERace race);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_GetAccountName / GameState_GetCharacterName
//--------------------------------------------------------------------------------
// Report-time-only reads: read fresh when the report form composes its
// reporter line, never cached or stored on a LiveLogEntry. Account name
// is RTAPI-only (no Mumble equivalent); character name is
// RTAPI-preferred/Mumble-fallback like the accessors above, reading
// Mumble's Identity.Name -- not Data.Name, which is the Mumble-Link
// application identifier (e.g. "Guild Wars 2"), not a character name.
// Both are empty if their source isn't available.
//--------------------------------------------------------------------------------
std::string GameState_GetAccountName();
std::string GameState_GetCharacterName();