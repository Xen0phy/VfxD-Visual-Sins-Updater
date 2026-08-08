//################################################################################
// spec_profession_table.h
//--------------------------------------------------------------------------------
// Companion to specialization_names.h: same 81-id GW2 specialization space
// (see that header for the id->name table and its own sourcing note), this
// time keeping the `profession` and `elite` fields the original name-table
// generator discarded. Exists to let EffectDbSpecializationMask (effect_db.h)
// store a single merged profession+specialization value instead of two
// independent masks that would lose which profession fired which spec on a
// multi-value row -- see class-spec-bitmask-handoff.md for the full design
// writeup this table was produced for, and effect_db.h's
// EffectDbSpecializationMask section for the bit layout that relies on the
// guarantee this table encodes: a nonzero specialization id always implies
// exactly one owning profession.
//
// Table populated from https://api.guildwars2.com/v2/specializations?ids=all
// (public GW2 API, no key required), same source as specialization_names.cpp.
// Guardian, Warrior, Engineer, Ranger, Thief, Elementalist, Mesmer,
// Necromancer, and Revenant were fetched directly and cross-validated: the
// 81 ids partition into exactly nine 9-id groups (5 core + 4 elite each)
// with zero overlap or leftover.
//--------------------------------------------------------------------------------

#pragma once

#include "game_state.h" //. pulls in Mumble.h (EProfession)

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SpecializationProfession / SpecializationIsElite
//--------------------------------------------------------------------------------
// SpecializationProfession returns the profession that owns
// specializationId, or Mumble::EProfession::None if the id isn't in the
// table (e.g. a future elite spec released after this table was last
// updated -- mirrors SpecializationName's own "don't guess" contract).
// This is what lets a caller unpack an EffectDbSpecializationMask's real-id
// bits back into per-profession buckets without the mask itself needing to
// carry a redundant profession value alongside each spec id (see
// effect_db.h's EffectDbSpecializationMask doc comment).
//
// SpecializationIsElite returns true only for one of a profession's 4 elite
// specs; false for a core spec (one of the 5) or an id not in the table.
// Purely a display concern (e.g. grouping "Core" vs "Elite" under a
// profession in the tree UI) -- never consulted by effect_db.h/.cpp itself.
//--------------------------------------------------------------------------------
Mumble::EProfession SpecializationProfession(unsigned int specializationId);
bool                 SpecializationIsElite(unsigned int specializationId);
