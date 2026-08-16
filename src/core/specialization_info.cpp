//################################################################################
// specialization_info.cpp   (see: specialization_info.h)
//--------------------------------------------------------------------------------

#include "specialization_info.h"

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetSpecializationInfo
//--------------------------------------------------------------------------------
// Generated from https://api.guildwars2.com/v2/specializations?ids=all;
// regenerate from that endpoint if this table is ever lost.
//--------------------------------------------------------------------------------
SpecializationInfo GetSpecializationInfo(unsigned int specializationId)
{
    switch (specializationId)
    {
        //_ Mesmer
        case 1:  return {"Dueling",             EMesmer};
        case 10: return {"Domination",          EMesmer};
        case 23: return {"Inspiration",         EMesmer};
        case 24: return {"Illusions",           EMesmer};
        case 40: return {"Chronomancer",        EMesmer};
        case 45: return {"Chaos",               EMesmer};
        case 59: return {"Mirage",              EMesmer};
        case 66: return {"Virtuoso",            EMesmer};
        case 73: return {"Troubadour",          EMesmer};

        //_ Necromancer
        case 2:  return {"Death Magic",         ENecromancer};
        case 19: return {"Blood Magic",         ENecromancer};
        case 34: return {"Reaper",              ENecromancer};
        case 39: return {"Curses",              ENecromancer};
        case 50: return {"Soul Reaping",        ENecromancer};
        case 53: return {"Spite",               ENecromancer};
        case 60: return {"Scourge",             ENecromancer};
        case 64: return {"Harbinger",           ENecromancer};
        case 76: return {"Ritualist",           ENecromancer};

        //_ Revenant
        case 3:  return {"Invocation",          ERevenant};
        case 9:  return {"Retribution",         ERevenant};
        case 12: return {"Salvation",           ERevenant};
        case 14: return {"Corruption",          ERevenant};
        case 15: return {"Devastation",         ERevenant};
        case 52: return {"Herald",              ERevenant};
        case 63: return {"Renegade",            ERevenant};
        case 69: return {"Vindicator",          ERevenant};
        case 79: return {"Conduit",             ERevenant};

        //_ Warrior
        case 4:  return {"Strength",            EWarrior};
        case 11: return {"Tactics",             EWarrior};
        case 18: return {"Berserker",           EWarrior};
        case 22: return {"Defense",             EWarrior};
        case 36: return {"Arms",                EWarrior};
        case 51: return {"Discipline",          EWarrior};
        case 61: return {"Spellbreaker",        EWarrior};
        case 68: return {"Bladesworn",          EWarrior};
        case 74: return {"Paragon",             EWarrior};

        //_ Ranger
        case 5:  return {"Druid",               ERanger};
        case 8:  return {"Marksmanship",        ERanger};
        case 25: return {"Nature Magic",        ERanger};
        case 30: return {"Skirmishing",         ERanger};
        case 32: return {"Beastmastery",        ERanger};
        case 33: return {"Wilderness Survival", ERanger};
        case 55: return {"Soulbeast",           ERanger};
        case 72: return {"Untamed",             ERanger};
        case 78: return {"Galeshot",            ERanger};

        //_ Engineer
        case 6:  return {"Explosives",          EEngineer};
        case 21: return {"Tools",               EEngineer};
        case 29: return {"Alchemy",             EEngineer};
        case 38: return {"Firearms",            EEngineer};
        case 43: return {"Scrapper",            EEngineer};
        case 47: return {"Inventions",          EEngineer};
        case 57: return {"Holosmith",           EEngineer};
        case 70: return {"Mechanist",           EEngineer};
        case 75: return {"Amalgam",             EEngineer};

        //_ Thief
        case 7:  return {"Daredevil",           EThief};
        case 20: return {"Shadow Arts",         EThief};
        case 28: return {"Deadly Arts",         EThief};
        case 35: return {"Critical Strikes",    EThief};
        case 44: return {"Trickery",            EThief};
        case 54: return {"Acrobatics",          EThief};
        case 58: return {"Deadeye",             EThief};
        case 71: return {"Specter",             EThief};
        case 77: return {"Antiquary",           EThief};

        //_ Guardian
        case 13: return {"Valor",               EGuardian};
        case 16: return {"Radiance",            EGuardian};
        case 27: return {"Dragonhunter",        EGuardian};
        case 42: return {"Zeal",                EGuardian};
        case 46: return {"Virtues",             EGuardian};
        case 49: return {"Honor",               EGuardian};
        case 62: return {"Firebrand",           EGuardian};
        case 65: return {"Willbender",          EGuardian};
        case 81: return {"Luminary",            EGuardian};

        //_ Elementalist
        case 17: return {"Water",               EElementalist};
        case 26: return {"Earth",               EElementalist};
        case 31: return {"Fire",                EElementalist};
        case 37: return {"Arcane",              EElementalist};
        case 41: return {"Air",                 EElementalist};
        case 48: return {"Tempest",             EElementalist};
        case 56: return {"Weaver",              EElementalist};
        case 67: return {"Catalyst",            EElementalist};
        case 80: return {"Evoker",              EElementalist};

        default:
            return {};   //. falls back to raw id
    }
}

const char* SpecializationName(unsigned int specializationId)
{
    return GetSpecializationInfo(specializationId).name;
}

Mumble::EProfession SpecializationProfession(unsigned int specializationId)
{
    return GetSpecializationInfo(specializationId).profession;
}