//################################################################################
// game_state.cpp
//--------------------------------------------------------------------------------
// See game_state.h for the module contract and source-selection rules.
// Owns: caching the two DataLink pointers, and the per-field RTAPI/Mumble
// selection logic. Deliberately has no knowledge of live_log.cpp or any
// other consumer -- callers decide when to call these, this module just
// answers "what is it right now."
//--------------------------------------------------------------------------------

#include "game_state.h"

#include <cstring>

namespace {

//_ Cached once in GameState_Init -- DataLink_Get's pointers stay valid
// for the addon's session, so these aren't re-queried every call.
Mumble::Identity*    s_mumbleIdentity = nullptr;
Mumble::Data*        s_mumbleLink     = nullptr;
RTAPI::RealTimeData* s_rtapiData      = nullptr;

} // namespace

void GameState_Init(AddonAPI_t* aApi)
{
    if (!aApi)
        return;

    s_mumbleLink     = static_cast<Mumble::Data*>(aApi->DataLink_Get(DL_MUMBLE_LINK));
    s_mumbleIdentity = static_cast<Mumble::Identity*>(aApi->DataLink_Get(DL_MUMBLE_LINK_IDENTITY));
    s_rtapiData      = static_cast<RTAPI::RealTimeData*>(aApi->DataLink_Get(DL_RTAPI));
}

void GameState_Shutdown()
{
    s_mumbleLink     = nullptr;
    s_mumbleIdentity = nullptr;
    s_rtapiData      = nullptr;
}

bool GameState_IsRTAPILive()
{
    return s_rtapiData != nullptr && s_rtapiData->GameBuild != 0;
}

bool GameState_HasMumbleIdentity()
{
    return s_mumbleIdentity != nullptr;
}

unsigned int GameState_GetMapID()
{
    if (GameState_IsRTAPILive())
        return s_rtapiData->MapID;
    if (s_mumbleLink)
        return s_mumbleLink->Context.MapID;
    return 0;
}

Mumble::EProfession GameState_GetProfession()
{
    if (GameState_IsRTAPILive())
        return static_cast<Mumble::EProfession>(s_rtapiData->Profession);
    if (s_mumbleIdentity)
        return s_mumbleIdentity->Profession;
    return Mumble::EProfession::None;
}

unsigned int GameState_GetSpecialization()
{
    //_ Cross-source equivalence confirmed by hand-testing (see
    // game_state.h). No reconciliation beyond the fallback order itself.
    if (GameState_IsRTAPILive())
        return s_rtapiData->EliteSpecialization;
    if (s_mumbleIdentity)
        return s_mumbleIdentity->Specialization;
    return 0;
}

Mumble::ERace GameState_GetRace()
{
    //_ Falls back to Asura (0) when identity isn't available -- callers
    // needing to tell that apart from a genuine Asura should check
    // GameState_HasMumbleIdentity() first.
    if (s_mumbleIdentity)
        return s_mumbleIdentity->Race;
    return Mumble::ERace::Asura;
}

const char* GameState_ProfessionName(Mumble::EProfession profession)
{
    //_ Matched directly against Mumble.h's EProfession, not guessed from
    // the enumerator spelling.
    switch (profession)
    {
        case Mumble::EProfession::None:         return "None";
        case Mumble::EProfession::Guardian:      return "Guardian";
        case Mumble::EProfession::Warrior:       return "Warrior";
        case Mumble::EProfession::Engineer:      return "Engineer";
        case Mumble::EProfession::Ranger:        return "Ranger";
        case Mumble::EProfession::Thief:         return "Thief";
        case Mumble::EProfession::Elementalist:  return "Elementalist";
        case Mumble::EProfession::Mesmer:        return "Mesmer";
        case Mumble::EProfession::Necromancer:   return "Necromancer";
        case Mumble::EProfession::Revenant:      return "Revenant";
        default:                                 return "Unknown";
    }
}

const char* GameState_RaceName(Mumble::ERace race)
{
    switch (race)
    {
        case Mumble::ERace::Asura:   return "Asura";
        case Mumble::ERace::Charr:   return "Charr";
        case Mumble::ERace::Human:   return "Human";
        case Mumble::ERace::Norn:    return "Norn";
        case Mumble::ERace::Sylvari: return "Sylvari";
        default:                     return "Unknown";
    }
}

namespace {

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FixedCharArrayToString
//--------------------------------------------------------------------------------
// Converts a fixed-size char array to a std::string. Mumble.h/RTAPI.hpp
// document these as null-terminated in practice, but neither guarantees
// it if the real value ever exactly fills the buffer -- strnlen bounds
// the read instead of trusting an unguaranteed terminator.
//--------------------------------------------------------------------------------
std::string FixedCharArrayToString(const char* arr, size_t capacity)
{
    return std::string(arr, strnlen(arr, capacity));
}

} //. namespace

std::string GameState_GetAccountName()
{
    if (GameState_IsRTAPILive())
        return FixedCharArrayToString(s_rtapiData->AccountName, sizeof(s_rtapiData->AccountName));
    return std::string();
}

std::string GameState_GetCharacterName()
{
    if (GameState_IsRTAPILive())
        return FixedCharArrayToString(s_rtapiData->CharacterName, sizeof(s_rtapiData->CharacterName));
    if (s_mumbleIdentity)
        return FixedCharArrayToString(s_mumbleIdentity->Name, sizeof(s_mumbleIdentity->Name));
    return std::string();
}