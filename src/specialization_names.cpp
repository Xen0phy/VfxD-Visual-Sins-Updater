// specialization_names.cpp
//
// Elite/core specialization id -> display name table, populated from
// https://api.guildwars2.com/v2/specializations?ids=all (public GW2 API,
// no key required). See specialization_names.h for why this couldn't be
// filled in from any header shared this session, and why it was left
// empty until now.
//
// Cross-source equivalence (RTAPI's EliteSpecialization vs Mumble's
// Identity.Specialization sharing this same id space) is CONFIRMED by
// hand-testing with this exact table -- both sources resolve to the
// correct specialization name for the same live character. See
// HANDOFF_LiveLogEnrichment.md's "Explicitly closed" section.
#include "specialization_names.h"

const char* SpecializationName(unsigned int specializationId)
{
    switch (specializationId)
    {
        case 1:  return "Dueling";
        case 2:  return "Death Magic";
        case 3:  return "Invocation";
        case 4:  return "Strength";
        case 5:  return "Druid";
        case 6:  return "Explosives";
        case 7:  return "Daredevil";
        case 8:  return "Marksmanship";
        case 9:  return "Retribution";
        case 10: return "Domination";
        case 11: return "Tactics";
        case 12: return "Salvation";
        case 13: return "Valor";
        case 14: return "Corruption";
        case 15: return "Devastation";
        case 16: return "Radiance";
        case 17: return "Water";
        case 18: return "Berserker";
        case 19: return "Blood Magic";
        case 20: return "Shadow Arts";
        case 21: return "Tools";
        case 22: return "Defense";
        case 23: return "Inspiration";
        case 24: return "Illusions";
        case 25: return "Nature Magic";
        case 26: return "Earth";
        case 27: return "Dragonhunter";
        case 28: return "Deadly Arts";
        case 29: return "Alchemy";
        case 30: return "Skirmishing";
        case 31: return "Fire";
        case 32: return "Beastmastery";
        case 33: return "Wilderness Survival";
        case 34: return "Reaper";
        case 35: return "Critical Strikes";
        case 36: return "Arms";
        case 37: return "Arcane";
        case 38: return "Firearms";
        case 39: return "Curses";
        case 40: return "Chronomancer";
        case 41: return "Air";
        case 42: return "Zeal";
        case 43: return "Scrapper";
        case 44: return "Trickery";
        case 45: return "Chaos";
        case 46: return "Virtues";
        case 47: return "Inventions";
        case 48: return "Tempest";
        case 49: return "Honor";
        case 50: return "Soul Reaping";
        case 51: return "Discipline";
        case 52: return "Herald";
        case 53: return "Spite";
        case 54: return "Acrobatics";
        case 55: return "Soulbeast";
        case 56: return "Weaver";
        case 57: return "Holosmith";
        case 58: return "Deadeye";
        case 59: return "Mirage";
        case 60: return "Scourge";
        case 61: return "Spellbreaker";
        case 62: return "Firebrand";
        case 63: return "Renegade";
        case 64: return "Harbinger";
        case 65: return "Willbender";
        case 66: return "Virtuoso";
        case 67: return "Catalyst";
        case 68: return "Bladesworn";
        case 69: return "Vindicator";
        case 70: return "Mechanist";
        case 71: return "Specter";
        case 72: return "Untamed";
        case 73: return "Troubadour";
        case 74: return "Paragon";
        case 75: return "Amalgam";
        case 76: return "Ritualist";
        case 77: return "Antiquary";
        case 78: return "Galeshot";
        case 79: return "Conduit";
        case 80: return "Evoker";
        case 81: return "Luminary";

        default:
            return nullptr; // not in the table yet -- caller falls back to the raw numeric id
    }
}
