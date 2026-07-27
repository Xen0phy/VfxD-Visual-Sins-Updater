#pragma once

// ---------------------------------------------------------------------------
// Elite/core specialization ID -> display name lookup.
//
// Unlike Mumble::EProfession/ERace (see game_state.h), which Mumble.h names
// every value of directly, Guild Wars 2's specialization IDs are NOT
// enumerated anywhere in Mumble.h, RTAPI.hpp, or any other header
// available. They're an open-ended set defined by ArenaNet's own
// /v2/specializations API -- currently 81 entries (core + elite,
// per-profession) and growing with every new elite specialization release,
// not something baked into this addon's dependencies.
//
// Table populated from https://api.guildwars2.com/v2/specializations?ids=all
// (public GW2 API endpoint, no API key required) -- see
// specialization_names.cpp. Cross-source equivalence (RTAPI's
// EliteSpecialization and Mumble's Identity.Specialization sharing this
// same id space) is now CONFIRMED by hand-testing: both resolve to the
// correct name for the same live character.
// ---------------------------------------------------------------------------

// Returns the specialization's display name, or nullptr if `specializationId`
// isn't in the table (e.g. a future elite spec released after this table was
// last updated). Never guesses a name for an id it doesn't have a verified
// entry for -- the caller falls back to showing the raw numeric id instead.
const char* SpecializationName(unsigned int specializationId);
