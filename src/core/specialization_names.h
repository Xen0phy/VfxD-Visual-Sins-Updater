//################################################################################
// specialization_names.h
//--------------------------------------------------------------------------------
// Elite/core specialization ID -> display name lookup. Unlike
// Mumble::EProfession/ERace (see game_state.h), which Mumble.h names
// every value of directly, GW2's specialization IDs are NOT enumerated
// in Mumble.h, RTAPI.hpp, or any other available header -- they're an
// open-ended set defined by ArenaNet's own /v2/specializations API,
// currently 81 entries (core + elite, per-profession) and growing with
// every new elite specialization.
//
// Table populated from
// https://api.guildwars2.com/v2/specializations?ids=all (public GW2 API,
// no key required) -- see specialization_names.cpp. Cross-source
// equivalence (RTAPI's EliteSpecialization and Mumble's
// Identity.Specialization sharing this id space) is confirmed by
// hand-testing: both resolve to the correct name for the same live
// character.
//--------------------------------------------------------------------------------

#pragma once

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SpecializationName
//--------------------------------------------------------------------------------
// Returns the specialization's display name, or nullptr if
// specializationId isn't in the table (e.g. a future elite spec released
// after this table was last updated). Never guesses a name for an id it
// doesn't have a verified entry for -- the caller falls back to showing
// the raw numeric id instead.
//--------------------------------------------------------------------------------
const char* SpecializationName(unsigned int specializationId);