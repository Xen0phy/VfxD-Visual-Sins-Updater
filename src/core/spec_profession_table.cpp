//################################################################################
// spec_profession_table.cpp
//--------------------------------------------------------------------------------
// See spec_profession_table.h for the module contract and sourcing note.
// Owns: the flat SpecOwner[] table (id, profession, elite) and the two
// linear-scan accessors built over it. 81 entries -- a linear scan per call
// is fine at this size and this call frequency (render-time tree grouping,
// not a hot per-event path).
//--------------------------------------------------------------------------------

#include "spec_profession_table.h"

namespace {

struct SpecOwner
{
    unsigned int         id;
    Mumble::EProfession   profession;
    bool                  isElite;
};

//_ Grouped by profession, core (5) then elite (4) within each, matching
// class-spec-bitmask-handoff.md §4's table -- regenerate from that table
// (source of truth) if this file is ever lost.
constexpr SpecOwner kSpecOwners[] = {
    //, Guardian
    { 42, Mumble::EProfession::Guardian, false }, //. Zeal
    { 16, Mumble::EProfession::Guardian, false }, //. Radiance
    { 13, Mumble::EProfession::Guardian, false }, //. Valor
    { 49, Mumble::EProfession::Guardian, false }, //. Honor
    { 46, Mumble::EProfession::Guardian, false }, //. Virtues
    { 27, Mumble::EProfession::Guardian, true  }, //. Dragonhunter
    { 62, Mumble::EProfession::Guardian, true  }, //. Firebrand
    { 65, Mumble::EProfession::Guardian, true  }, //. Willbender
    { 81, Mumble::EProfession::Guardian, true  }, //. Luminary

    //. Warrior
    { 4,  Mumble::EProfession::Warrior, false }, //. Strength
    { 36, Mumble::EProfession::Warrior, false }, //. Arms
    { 22, Mumble::EProfession::Warrior, false }, //. Defense
    { 11, Mumble::EProfession::Warrior, false }, //. Tactics
    { 51, Mumble::EProfession::Warrior, false }, //. Discipline
    { 18, Mumble::EProfession::Warrior, true  }, //. Berserker
    { 61, Mumble::EProfession::Warrior, true  }, //. Spellbreaker
    { 68, Mumble::EProfession::Warrior, true  }, //. Bladesworn
    { 74, Mumble::EProfession::Warrior, true  }, //. Paragon

    //. Engineer
    { 6,  Mumble::EProfession::Engineer, false }, //. Explosives
    { 38, Mumble::EProfession::Engineer, false }, //. Firearms
    { 47, Mumble::EProfession::Engineer, false }, //. Inventions
    { 29, Mumble::EProfession::Engineer, false }, //. Alchemy
    { 21, Mumble::EProfession::Engineer, false }, //. Tools
    { 43, Mumble::EProfession::Engineer, true  }, //. Scrapper
    { 57, Mumble::EProfession::Engineer, true  }, //. Holosmith
    { 70, Mumble::EProfession::Engineer, true  }, //. Mechanist
    { 75, Mumble::EProfession::Engineer, true  }, //. Amalgam

    //. Ranger
    { 8,  Mumble::EProfession::Ranger, false }, //. Marksmanship
    { 30, Mumble::EProfession::Ranger, false }, //. Skirmishing
    { 33, Mumble::EProfession::Ranger, false }, //. Wilderness Survival
    { 32, Mumble::EProfession::Ranger, false }, //. Beastmastery
    { 25, Mumble::EProfession::Ranger, false }, //. Nature Magic
    { 5,  Mumble::EProfession::Ranger, true  }, //. Druid
    { 55, Mumble::EProfession::Ranger, true  }, //. Soulbeast
    { 72, Mumble::EProfession::Ranger, true  }, //. Untamed
    { 78, Mumble::EProfession::Ranger, true  }, //. Galeshot

    //. Thief
    { 28, Mumble::EProfession::Thief, false }, //. Deadly Arts
    { 35, Mumble::EProfession::Thief, false }, //. Critical Strikes
    { 20, Mumble::EProfession::Thief, false }, //. Shadow Arts
    { 54, Mumble::EProfession::Thief, false }, //. Acrobatics
    { 44, Mumble::EProfession::Thief, false }, //. Trickery
    { 7,  Mumble::EProfession::Thief, true  }, //. Daredevil
    { 58, Mumble::EProfession::Thief, true  }, //. Deadeye
    { 71, Mumble::EProfession::Thief, true  }, //. Specter
    { 77, Mumble::EProfession::Thief, true  }, //. Antiquary

    //. Elementalist
    { 31, Mumble::EProfession::Elementalist, false }, //. Fire
    { 41, Mumble::EProfession::Elementalist, false }, //. Air
    { 26, Mumble::EProfession::Elementalist, false }, //. Earth
    { 17, Mumble::EProfession::Elementalist, false }, //. Water
    { 37, Mumble::EProfession::Elementalist, false }, //. Arcane
    { 48, Mumble::EProfession::Elementalist, true  }, //. Tempest
    { 56, Mumble::EProfession::Elementalist, true  }, //. Weaver
    { 67, Mumble::EProfession::Elementalist, true  }, //. Catalyst
    { 80, Mumble::EProfession::Elementalist, true  }, //. Evoker

    //. Mesmer
    { 1,  Mumble::EProfession::Mesmer, false }, //. Dueling
    { 10, Mumble::EProfession::Mesmer, false }, //. Domination
    { 23, Mumble::EProfession::Mesmer, false }, //. Inspiration
    { 24, Mumble::EProfession::Mesmer, false }, //. Illusions
    { 45, Mumble::EProfession::Mesmer, false }, //. Chaos
    { 40, Mumble::EProfession::Mesmer, true  }, //. Chronomancer
    { 59, Mumble::EProfession::Mesmer, true  }, //. Mirage
    { 66, Mumble::EProfession::Mesmer, true  }, //. Virtuoso
    { 73, Mumble::EProfession::Mesmer, true  }, //. Troubadour

    //. Necromancer
    { 53, Mumble::EProfession::Necromancer, false }, //. Spite
    { 39, Mumble::EProfession::Necromancer, false }, //. Curses
    { 2,  Mumble::EProfession::Necromancer, false }, //. Death Magic
    { 19, Mumble::EProfession::Necromancer, false }, //. Blood Magic
    { 50, Mumble::EProfession::Necromancer, false }, //. Soul Reaping
    { 34, Mumble::EProfession::Necromancer, true  }, //. Reaper
    { 60, Mumble::EProfession::Necromancer, true  }, //. Scourge
    { 64, Mumble::EProfession::Necromancer, true  }, //. Harbinger
    { 76, Mumble::EProfession::Necromancer, true  }, //. Ritualist

    //. Revenant
    { 3,  Mumble::EProfession::Revenant, false }, //. Invocation
    { 9,  Mumble::EProfession::Revenant, false }, //. Retribution
    { 14, Mumble::EProfession::Revenant, false }, //. Corruption
    { 12, Mumble::EProfession::Revenant, false }, //. Salvation
    { 15, Mumble::EProfession::Revenant, false }, //. Devastation
    { 52, Mumble::EProfession::Revenant, true  }, //. Herald
    { 63, Mumble::EProfession::Revenant, true  }, //. Renegade
    { 69, Mumble::EProfession::Revenant, true  }, //. Vindicator
    { 79, Mumble::EProfession::Revenant, true  }, //. Conduit
};

const SpecOwner* FindSpecOwner(unsigned int specializationId)
{
    for (const auto& owner : kSpecOwners)
        if (owner.id == specializationId)
            return &owner;
    return nullptr;
}

} //. namespace

Mumble::EProfession SpecializationProfession(unsigned int specializationId)
{
    const SpecOwner* owner = FindSpecOwner(specializationId);
    return owner ? owner->profession : Mumble::EProfession::None;
}

bool SpecializationIsElite(unsigned int specializationId)
{
    const SpecOwner* owner = FindSpecOwner(specializationId);
    return owner && owner->isElite;
}
