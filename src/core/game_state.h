//################################################################################
// game_state.h
//--------------------------------------------------------------------------------
// Standalone Mumble Link / RTAPI reader, not specific to the live log -- any
// consumer (live log enrichment now, the report feature later) reads game state
// through one place, with the RTAPI-preferred/Mumble-fallback rule defined
// exactly once. Mirrors the DataLink access pattern already used by Nexus-based
// addons; caches the DataLink pointers once instead of re-fetching them every
// call. Has no knowledge of live_log.cpp or any other consumer; callers decide
// when to read state, this module only answers what it is right now.
//
// Source selection, per field:
//   MapID/Profession/Specialization -- RTAPI if live, else Mumble. Profession's
//   numeric mapping and Specialization's cross-source equivalence are both
//   confirmed to agree between the two sources.
//   Race -- always Mumble; RTAPI has no race field at all.
//
// Identity fields (Profession/Specialization/Race) come from Nexus's
// DL_MUMBLE_LINK_IDENTITY, which Nexus itself parses into a real
// Mumble::Identity struct -- not JSON-parsed here. MapID is a plain struct
// field on Data::Context, no parsing needed either way.
//--------------------------------------------------------------------------------

#pragma once
//_ Two Mumble.h members share a name with their type -- a GCC hard error.
#include "Mumble.h"
#include "Nexus.h"
#include "RTAPI.hpp"

#include <string>

//_ Shorthand for Mumble::EProfession -- keeps profession tables terse.
using EProfession = Mumble::EProfession;

constexpr EProfession ENone         = EProfession::None;
constexpr EProfession EGuardian     = EProfession::Guardian;
constexpr EProfession EWarrior      = EProfession::Warrior;
constexpr EProfession EEngineer     = EProfession::Engineer;
constexpr EProfession ERanger       = EProfession::Ranger;
constexpr EProfession EThief        = EProfession::Thief;
constexpr EProfession EElementalist = EProfession::Elementalist;
constexpr EProfession EMesmer       = EProfession::Mesmer;
constexpr EProfession ENecromancer  = EProfession::Necromancer;
constexpr EProfession ERevenant     = EProfession::Revenant;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_Init / GameState_Shutdown
//--------------------------------------------------------------------------------
// Lifecycle pair. Init caches the DataLink pointers -- call once from
// Addon_Load after aApi is available, same convention as LiveLog_Init. Safe
// even if Mumble/RTAPI aren't present yet. Shutdown clears the cached pointers
// and is safe even if Init was never reached.
//--------------------------------------------------------------------------------
void GameState_Init(AddonAPI_t* aApi);
void GameState_Shutdown();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_IsRTAPILive
//--------------------------------------------------------------------------------
// True if RTAPI's shared block is present and actually live -- GameBuild is
// documented as set to 0 when RTAPI is unloaded, so presence of the block alone
// isn't enough.
//--------------------------------------------------------------------------------
bool GameState_IsRTAPILive();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_HasMumbleIdentity
//--------------------------------------------------------------------------------
// True if Nexus's Mumble Link identity block is present at all -- lets callers
// distinguish "no data available" from "value is genuinely the zero/first enum
// entry".
//--------------------------------------------------------------------------------
bool GameState_HasMumbleIdentity();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_GetMapID / GameState_GetProfession / GameState_GetSpecialization /
// GameState_GetRace
//--------------------------------------------------------------------------------
unsigned int         GameState_GetMapID();
Mumble::EProfession   GameState_GetProfession();
unsigned int         GameState_GetSpecialization();
Mumble::ERace         GameState_GetRace();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_ProfessionName / GameState_RaceName
//--------------------------------------------------------------------------------
// Human-readable names for the two enum fields Mumble.h itself already names
// every value of -- no external ID table needed, unlike Specialization (see
// specialization_info.h). Always returns a valid non-null string, falling back
// to "Unknown" outside the enum's range.
//--------------------------------------------------------------------------------
const char* GameState_ProfessionName(Mumble::EProfession profession);
const char* GameState_RaceName(Mumble::ERace race);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GameState_GetAccountName / GameState_GetCharacterName
//--------------------------------------------------------------------------------
// Report-time-only reads: read fresh when the report form composes its reporter
// line, never cached or stored on a LiveLogEntry. Account name is RTAPI-only
// (no Mumble equivalent); character name is RTAPI-preferred/Mumble-fallback
// like the accessors above, reading Mumble's Identity.Name -- not Data.Name,
// which is the Mumble-Link application identifier (e.g. "Guild Wars 2"), not a
// character name. Both are empty if their source isn't available.
//--------------------------------------------------------------------------------
std::string GameState_GetAccountName();
std::string GameState_GetCharacterName();