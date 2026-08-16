//################################################################################
// report_ui.cpp   (see: report_ui.h)
//--------------------------------------------------------------------------------

#include "game_state.h"
#include "imgui.h"
#include "installed_tree_store.h"
#include "report_ui.h"
#include "specialization_info.h"
#include "ui_colors.h"
#include "webhook_report.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

namespace {

//_ -1 is this form's own sentinel for unset race; Mumble.h has no None value.
constexpr int kRaceUnset = -1;
constexpr Mumble::ERace kRaceValues[] = {
    Mumble::ERace::Asura, Mumble::ERace::Charr, Mumble::ERace::Human,
    Mumble::ERace::Norn,  Mumble::ERace::Sylvari,
};
constexpr EProfession kProfessionValues[] = {
    EGuardian, EWarrior,
    EEngineer, ERanger,
    EThief,    EElementalist,
    EMesmer,   ENecromancer,
    ERevenant,
};

int RaceToIndex(Mumble::ERace race)
{
    for (int i = 0; i < (int)std::size(kRaceValues); ++i)
        if (kRaceValues[i] == race)
            return i;
    return kRaceUnset;   //. unreachable in practice
}

} //. namespace

//********************************************************************************
// ReportFormRow
//--------------------------------------------------------------------------------
// guid                one GUID's identifier text
// typeText            digits 0-11, or blank/"Not set" (see ParseReportTypeText)
// mapID               self-context MapID; 0 doubles as "not set"
// raceIndex           index into kRaceValues, or kRaceUnset
// profession          self-context profession; None doubles as "not set"
// specializationText  autocomplete text, resolved to an id at compose time
// showSpecSuggest     visibility of the suggestion window, tracked by hand
//--------------------------------------------------------------------------------
// One row holds everything editable about a single GUID entry, including self
// context: pre-filled from a live-log snapshot (taken once, at "report" time)
// when one exists, otherwise starting at each field's own default.
//--------------------------------------------------------------------------------
struct ReportFormRow
{
    char         guid[128] = {};
    char         typeText[8] = {};
    int          mapID = 0;
    int          raceIndex = kRaceUnset;
    EProfession  profession = ENone;
    char         specializationText[64] = {};
    bool         showSpecSuggest = false;
};

static bool                       s_reportAnonymous = false;
static char                       s_reportAccountNameBuf[128]   = {};
static char                       s_reportCharacterNameBuf[128] = {};
static std::vector<ReportFormRow> s_reportRows;
static char                       s_reportNoteBuf[1024] = {};

//_ Validation error shown until the next attempt, cleared on success.
static std::string                s_reportFormError;

//_ True from a successful send until RenderReportSection reacts once.
static bool                       s_reportAwaitingResult = false;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RefreshReportNameFieldsFromGameState
//--------------------------------------------------------------------------------
// Re-reads Account/Character Name from GameState and overwrites the report form's
// name buffers with whatever's live right now. Called only from
// AddReportRowFromLiveLogEntry (every "report" click) -- not on section
// render/addon load, and not for a manually-added row.
//
// Overwrite is unconditional, not "only if still blank" -- a later click re-syncs
// with whoever's actually playing now, even if the user had typed something else
// into the boxes since (e.g. switched characters between two clicks).
//--------------------------------------------------------------------------------
void RefreshReportNameFieldsFromGameState()
{
    std::string acct = GameState_GetAccountName();
    std::string chr  = GameState_GetCharacterName();
    std::snprintf(s_reportAccountNameBuf, sizeof(s_reportAccountNameBuf), "%s", acct.c_str());
    std::snprintf(s_reportCharacterNameBuf, sizeof(s_reportCharacterNameBuf), "%s", chr.c_str());
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// AddReportRowFromLiveLogEntry
//--------------------------------------------------------------------------------
// See report_ui.h for the contract. No-ops (with a form error instead of a silent
// drop) once s_reportRows is already at kMaxReportGuids -- see its own comment in
// webhook_report.h. Refreshes the name fields here instead of on render -- see
// RefreshReportNameFieldsFromGameState's comment for why that's tied to this call
// specifically.
//--------------------------------------------------------------------------------
void AddReportRowFromLiveLogEntry(const LiveLogEntry& entry)
{
    if (s_reportRows.size() >= kMaxReportGuids)
    {
        s_reportFormError = "Reports are capped at " + std::to_string(kMaxReportGuids) +
                            " GUIDs at a time -- send this batch first, or remove one below.";
        return;
    }

    RefreshReportNameFieldsFromGameState();

    ReportFormRow row;
    std::snprintf(row.guid, sizeof(row.guid), "%s", entry.guid_b64.c_str());
    std::snprintf(row.typeText, sizeof(row.typeText), "%d", entry.type);
    if (entry.hasSelfContext)
    {
        row.mapID      = (int)entry.mapID;
        row.raceIndex  = RaceToIndex(entry.race);
        row.profession = entry.profession;
        if (entry.specialization != 0)
        {
            const char* name = SpecializationName(entry.specialization);
            if (name)
                std::snprintf(row.specializationText, sizeof(row.specializationText), "%s", name);
            else
                std::snprintf(row.specializationText, sizeof(row.specializationText), "%u", entry.specialization);
        }
    }
    //_ else: every field stays default -- nothing was observed for this GUID.
    s_reportRows.push_back(row);
}

namespace {

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// TrimReportText
//--------------------------------------------------------------------------------
// Local to this file's report-form code -- report.cpp has its own Trim for the
// same purpose, private to that translation unit.
//--------------------------------------------------------------------------------
std::string TrimReportText(const std::string& s)
{
    size_t start = 0, end = s.size();
    while (start < end && std::isspace((unsigned char)s[start])) ++start;
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(start, end - start);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ParseReportTypeText
//--------------------------------------------------------------------------------
// "Not set" tri-state: blank, or case-insensitively "not set", means unset;
// otherwise must parse as 0-11. Returns false (with outError set) for anything
// else, e.g. stray text or a number outside that range.
//--------------------------------------------------------------------------------
bool ParseReportTypeText(const std::string& text, bool& outIsSet, int& outValue, std::string& outError)
{
    std::string trimmed = text;
    size_t start = 0, end = trimmed.size();
    while (start < end && std::isspace((unsigned char)trimmed[start])) ++start;
    while (end > start && std::isspace((unsigned char)trimmed[end - 1])) --end;
    trimmed = trimmed.substr(start, end - start);

    std::string lower = trimmed;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);

    if (trimmed.empty() || lower == "not set")
    {
        outIsSet = false;
        outValue = -1;
        return true;
    }

    try
    {
        size_t consumed = 0;
        int value = std::stoi(trimmed, &consumed);
        if (consumed != trimmed.size() || value < 0 || value > 11)
        {
            outError = "Type must be blank/\"Not set\" or a number 0-11.";
            return false;
        }
        outIsSet = true;
        outValue = value;
        return true;
    }
    catch (...)
    {
        outError = "Type must be blank/\"Not set\" or a number 0-11.";
        return false;
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// AllSpecializations
//--------------------------------------------------------------------------------
// Every known (id, name) pair, built once by probing SpecializationName() across
// an id range comfortably past today's 81-entry table -- so a newly-added elite
// spec shows up automatically once that table is updated, without touching this
// loop. Kept as strings (not const char*) since SpecializationName() only
// promises its return value for the call's duration, not this cache's lifetime.
//--------------------------------------------------------------------------------
const std::vector<std::pair<unsigned int, std::string>>& AllSpecializations()
{
    static const std::vector<std::pair<unsigned int, std::string>> all = []
    {
        std::vector<std::pair<unsigned int, std::string>> v;
        for (unsigned int id = 1; id <= 200; ++id)
            if (const char* name = SpecializationName(id))
                v.emplace_back(id, name);
        return v;
    }();
    return all;
}

std::string LowerCopy(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ResolveSpecializationId
//--------------------------------------------------------------------------------
// Resolves the autocomplete box's free-typed text to a specialization id: blank
// -> 0 ("not set"); an all-digits string -> that raw id, taken as typed (still
// allowed, e.g. a future elite spec not yet in this table); otherwise an exact
// case-insensitive name match. Anything else (a typo, a partial word) resolves to
// 0/"not set" instead of rejecting the submission -- a convenience field, not a
// validated one.
//--------------------------------------------------------------------------------
unsigned int ResolveSpecializationId(const std::string& text)
{
    std::string trimmed = TrimReportText(text);
    if (trimmed.empty())
        return 0;

    bool allDigits = std::all_of(trimmed.begin(), trimmed.end(),
                                  [](unsigned char c) { return std::isdigit(c) != 0; });
    if (allDigits)
        return (unsigned int)std::stoul(trimmed);

    std::string needle = LowerCopy(trimmed);
    for (const auto& entry : AllSpecializations())
        if (LowerCopy(entry.second) == needle)
            return entry.first;

    return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ComposeReportGuidBlock
//--------------------------------------------------------------------------------
// Renders the per-GUID block template for the relay payload: a "GUID: `<guid>`"
// line (backtick-wrapped for Discord) followed by a fenced code block with
// Type/Self context lines -- see vfxd-sins-report-relay/src/index.js for how this
// fits into the full message. report.cpp never sees anything but this finished
// string.
//
// Self context prints all four fields, substituting "Unknown" for any still
// unset, unless every field is still at its default, in which case it prints "Not
// observed".
//--------------------------------------------------------------------------------
std::string ComposeReportGuidBlock(const std::string& guid, bool typeIsSet, int typeValue, const ReportFormRow& row)
{
    std::ostringstream out;
    out << "GUID: `" << guid << "`\n";
    out << "```\n";
    out << "Type: " << (typeIsSet ? std::to_string(typeValue) : std::string("Not set")) << "\n";

    unsigned int specId  = ResolveSpecializationId(row.specializationText);
    bool         raceSet = (row.raceIndex != kRaceUnset);
    bool         profSet = (row.profession != ENone);
    bool         specSet = (specId != 0);
    bool         mapSet  = (row.mapID != 0);

    if (!mapSet && !raceSet && !profSet && !specSet)
    {
        out << "Self context: Not observed\n";
    }
    else
    {
        std::string specName = "Unknown";
        if (specSet)
        {
            if (const char* n = SpecializationName(specId))
                specName = n;
            else
                specName = std::to_string(specId);
        }

        out << "Self context: MapID " << row.mapID << ", "
            << (raceSet ? GameState_RaceName(kRaceValues[row.raceIndex]) : "Unknown") << ", "
            << (profSet ? GameState_ProfessionName(row.profession) : "Unknown") << ", "
            << specName << "\n";
    }
    out << "```";
    return out.str();
}

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderReportSection
//--------------------------------------------------------------------------------
// Reporter-identity fields are only auto-filled/re-synced by "report" (see
// RefreshReportNameFieldsFromGameState); this function just displays and edits
// whatever's currently in the buffers, starting blank each session until the
// first click.
//
// Reacts to a just-finished send exactly once: a full send (AllSent) clears the
// form; partial/none-sent or error leaves rows/note in place so the person can
// see what happened.
//--------------------------------------------------------------------------------
void RenderReportSection(const std::string& denoiserAddonDir)
{
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    const float kFieldWidth = 320.0f;
    EReportStatus reportStatus = GetReportStatus();
    bool sending = (reportStatus == EReportStatus::Sending);

    if (s_reportAwaitingResult && reportStatus != EReportStatus::Sending)
    {
        if (reportStatus == EReportStatus::Done && GetLastReportOutcome() == EReportOutcome::AllSent)
        {
            s_reportRows.clear();
            s_reportNoteBuf[0] = '\0';
        }
        s_reportAwaitingResult = false;
    }

    ImGui::PushItemWidth(kFieldWidth);
    if (s_reportAnonymous)
    {
        //_ Fixed text, not editable -- avoids a half-clear/retype that defeats "anonymous".
        ImGui::TextDisabled("Account Name (RTAPI only):");
        ImGui::TextDisabled("(anonymous)");
        ImGui::TextDisabled("Character Name:");
        ImGui::TextDisabled("(anonymous)");
    }
    else
    {
        ImGui::TextDisabled("Account Name (RTAPI only):");
        ImGui::InputTextWithHint("##report_account_name", "(unavailable -- type your own)",
                                  s_reportAccountNameBuf, sizeof(s_reportAccountNameBuf));
        ImGui::TextDisabled("Character Name:");
        ImGui::InputTextWithHint("##report_character_name", "(unavailable -- type your own)",
                                  s_reportCharacterNameBuf, sizeof(s_reportCharacterNameBuf));
    }
    ImGui::PopItemWidth();

    ImGui::Checkbox("Send anonymously", &s_reportAnonymous);
    ImGui::Separator();

    ImGui::TextWrapped(
        "Attach up to %d GUIDs -- click \"report\" next to an "
        "entry in the Live Log, or add one by hand below.", (int)kMaxReportGuids);

    int removeIndex = -1;
    for (size_t i = 0; i < s_reportRows.size(); ++i)
    {
        ReportFormRow& row = s_reportRows[i];
        ImGui::PushID((int)i);

        ImGui::PushItemWidth(kFieldWidth * 0.7f);
        ImGui::InputText("GUID", row.guid, sizeof(row.guid));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::PushItemWidth(50.0f);
        ImGui::InputText("Type", row.typeText, sizeof(row.typeText));
        ImGui::PopItemWidth();

        //_ Self context is always shown, all four fields on one line by design.
        ImGui::PushItemWidth(40.0f);
        //_ No +/- step buttons -- a raw id, not a counter.
        ImGui::InputInt("MapID", &row.mapID, 0, 0);
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushItemWidth(60.0f);
        const char* racePreview = (row.raceIndex == kRaceUnset) ? "Race" : GameState_RaceName(kRaceValues[row.raceIndex]);
        if (ImGui::BeginCombo("##combo_race", racePreview))
        {
            if (ImGui::Selectable("Race", row.raceIndex == kRaceUnset))
                row.raceIndex = kRaceUnset;
            for (int r = 0; r < (int)std::size(kRaceValues); ++r)
            {
                bool selected = (row.raceIndex == r);
                if (ImGui::Selectable(GameState_RaceName(kRaceValues[r]), selected))
                    row.raceIndex = r;
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushItemWidth(80.0f);
        const bool  profUnset    = (row.profession == ENone);
        const char* profPreview  = profUnset ? "Profession" : GameState_ProfessionName(row.profession);
        if (ImGui::BeginCombo("##combo_profession", profPreview))
        {
            if (ImGui::Selectable("Profession", profUnset))
                row.profession = ENone;
            for (EProfession p : kProfessionValues)
            {
                bool selected = (row.profession == p);
                if (ImGui::Selectable(GameState_ProfessionName(p), selected))
                    row.profession = p;
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();

        //_ Free-text autocomplete, not a dropdown -- faster to search 80+ names by typing.
        ImGui::PushItemWidth(90.0f);
        bool specTextChanged = ImGui::InputTextWithHint("##combo_specialization", "Specialization", row.specializationText, sizeof(row.specializationText));
        bool specJustActivated = ImGui::IsItemActivated();
        bool specDeactivated   = ImGui::IsItemDeactivated();
        ImVec2 specBoxMin = ImGui::GetItemRectMin();
        ImVec2 specBoxMax = ImGui::GetItemRectMax();
        ImGui::PopItemWidth();
        ImGui::SameLine();

        //_ Plain Begin()/End(), not Popup -- avoids ClosePopupsOverWindow tear-down; visibility tracked by hand.
        if ((specJustActivated || specTextChanged) && row.specializationText[0] != '\0')
            row.showSpecSuggest = true;
        if (row.specializationText[0] == '\0' || specDeactivated)
            row.showSpecSuggest = false;

        if (row.showSpecSuggest)
        {
            ImGui::SetNextWindowPos(ImVec2(specBoxMin.x, specBoxMax.y));
            ImGui::SetNextWindowSize(ImVec2(specBoxMax.x - specBoxMin.x, 0));

            char specSuggestWindowId[32];
            std::snprintf(specSuggestWindowId, sizeof(specSuggestWindowId), "##spec_suggest_%d", (int)i);
            ImGui::Begin(specSuggestWindowId, nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

            std::string needle = LowerCopy(row.specializationText);
            int shown = 0;
            for (const auto& entry : AllSpecializations())
            {
                if (shown >= 8)
                    break;
                if (LowerCopy(entry.second).find(needle) == std::string::npos)
                    continue;
                if (ImGui::Selectable(entry.second.c_str()))
                {
                    std::snprintf(row.specializationText, sizeof(row.specializationText), "%s", entry.second.c_str());
                    row.showSpecSuggest = false;
                }
                ++shown;
            }
            if (shown == 0)
                ImGui::TextDisabled("(no match -- will send as Unknown)");

            ImGui::End();
        }

        if (ImGui::SmallButton("Remove"))
            removeIndex = (int)i;

        ImGui::PopID();
        ImGui::Separator();
    }
    if (removeIndex >= 0)
        s_reportRows.erase(s_reportRows.begin() + removeIndex);

    //_ imgui 1.80 lacks BeginDisabled/EndDisabled; swap-label workaround, gated on kMaxReportGuids.
    bool atGuidCap = s_reportRows.size() >= kMaxReportGuids;
    if (ImGui::SmallButton(atGuidCap ? "Max GUIDs reached" : "+ Add GUID manually") && !atGuidCap)
        s_reportRows.push_back(ReportFormRow{});

    ImGui::Spacing();
    ImGui::TextDisabled("Additional information (required):");
    ImGui::InputTextMultiline("##report_note", s_reportNoteBuf, sizeof(s_reportNoteBuf), ImVec2(kFieldWidth, 80));

    //_ Same imgui 1.80 workaround as above, gated on sending instead of kMaxReportGuids.
    if (ImGui::Button(sending ? "Sending..." : "Send") && !sending)
    {
        std::string error;
        std::vector<ReportGuidBlock> payloadEntries;
        payloadEntries.reserve(s_reportRows.size());
        bool typeParseFailed = false;

        for (const ReportFormRow& row : s_reportRows)
        {
            bool typeIsSet = false;
            int  typeValue = -1;
            if (!ParseReportTypeText(row.typeText, typeIsSet, typeValue, error))
            {
                typeParseFailed = true;
                break;
            }

            ReportGuidBlock entry;
            entry.guid  = row.guid;
            entry.block = ComposeReportGuidBlock(row.guid, typeIsSet, typeValue, row);
            payloadEntries.push_back(std::move(entry));
        }

        if (typeParseFailed)
        {
            s_reportFormError = error;
        }
        else
        {
            std::string reporterLine;
            if (s_reportAnonymous)
            {
                reporterLine = "Reporter: `(anonymous)`";
            }
            else
            {
                //_ Boxes' current text (auto-filled/edited/typed); backtick-wrapped like ComposeReportGuidBlock's guids.
                std::string acct = TrimReportText(s_reportAccountNameBuf);
                std::string chr  = TrimReportText(s_reportCharacterNameBuf);
                if (acct.empty()) acct = "(unknown)";
                if (chr.empty())  chr  = "(unknown)";
                reporterLine = "Reporter: `" + acct + "` / `" + chr + "`";
            }

            if (StartSendReport(reporterLine, payloadEntries, s_reportNoteBuf, error))
            {
                s_reportFormError.clear();
                s_reportAwaitingResult = true;
            }
            else
            {
                s_reportFormError = error;
            }
        }
    }

    ImGui::SameLine();
    if (!s_reportFormError.empty())
        ImGui::TextColored(kDuplicateColor, "%s", s_reportFormError.c_str());

    std::string lastMsg = GetLastReportMessage();
    if (!lastMsg.empty())
    {
        //_ Color by outcome: full send=green, partial=orange, error/none-sent=red (ui_colors.h).
        const ImVec4* color = nullptr;
        if (reportStatus == EReportStatus::Error)
        {
            color = &kDuplicateColor;
        }
        else if (reportStatus == EReportStatus::Done)
        {
            switch (GetLastReportOutcome())
            {
                case EReportOutcome::AllSent:       color = &kNewColor;       break;
                case EReportOutcome::PartiallySent: color = &kReworkColor;    break;
                case EReportOutcome::NoneSent:      color = &kDuplicateColor; break;
                default: break;
            }
        }

        if (color)
            ImGui::TextColored(*color, "%s", lastMsg.c_str());
        else
            ImGui::TextWrapped("%s", lastMsg.c_str());
    }
}