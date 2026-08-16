//################################################################################
// specialization_info.h
//--------------------------------------------------------------------------------
// Elite/core specialization ID -> {name, profession} lookup, in one table.
// GW2's specialization IDs are NOT enumerated in Mumble.h, RTAPI.hpp, or any
// other available header -- they're an open-ended set defined by ArenaNet's
// own /v2/specializations API, currently 81 entries (core + elite,
// per-profession) and growing with every new elite specialization.
//
// Table populated from https://api.guildwars2.com/v2/specializations?ids=all
// (public GW2 API, no key required). -- see effect_db.h's
// EffectDbSpecializationMask section for the bit layout that relies on the
// guarantee this table encodes: a nonzero specialization id always implies
// exactly one owning profession.
//
// Cross-source equivalence (RTAPI's EliteSpecialization and Mumble's
// Identity.Specialization sharing this id space) is confirmed by hand-testing:
// both resolve to the correct name for the same live character.
//--------------------------------------------------------------------------------

#pragma once

#include "game_state.h" // IWYU pragma: keep

//********************************************************************************
// SpecializationInfo
//--------------------------------------------------------------------------------
// name         display name, or nullptr if the id isn't in the table
// profession   owning profession, or None if the id isn't in the table
//--------------------------------------------------------------------------------
struct SpecializationInfo
{
    const char*         name       = nullptr;
    Mumble::EProfession profession = Mumble::EProfession::None;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetSpecializationInfo
//--------------------------------------------------------------------------------
// Returns the table row for specializationId, or a row with name ==
// nullptr if the id isn't in the table (e.g. a future elite spec released
// after this table was last updated -- never guesses). Check `.name`
// before trusting `.profession`: an unknown id still comes back with the
// struct's defaults, not a verified answer.
//--------------------------------------------------------------------------------
SpecializationInfo GetSpecializationInfo(unsigned int specializationId);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SpecializationName / SpecializationProfession
//--------------------------------------------------------------------------------
// Thin wrappers over GetSpecializationInfo() kept for existing call sites
// (and callers who just want one field): same "don't guess" contract as
// before -- SpecializationName returns nullptr and SpecializationProfession
// returns Mumble::EProfession::None for any id not in the table.
//--------------------------------------------------------------------------------
const char*         SpecializationName(unsigned int specializationId);
Mumble::EProfession SpecializationProfession(unsigned int specializationId);