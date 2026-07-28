//################################################################################
// report_ui.cpp
//--------------------------------------------------------------------------------
// "Report an Effect" options-panel section. Extracted from addon.cpp -- a
// mechanical move, no behavior change. See report_ui.h for what's exposed
// and why.
//--------------------------------------------------------------------------------
// Manual "report an effect back" form: a reporter identity line
// (Account/Character name, or anonymous), zero or more per-GUID blocks
// (each with its own Type and a snapshot of that GUID's self-context, if
// it has one), and one required free-text note. Validation itself lives
// in report.cpp's StartSendReport; this section's job is the form
// widgets, composing each GUID's display block, and showing whatever
// outcome comes back.
//--------------------------------------------------------------------------------

#include "game_state.h"
#include "imgui.h"
#include "installed_tree_store.h"
#include "report_ui.h"
#include "specialization_names.h"
#include "ui_colors.h"
#include "webhook_report.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

namespace {

//_ Race has no None/Unknown in Mumble.h, so -1 is this form's own
// sentinel for "not set" -- never a real ERace value.
constexpr int kRaceUnset = -1;
constexpr Mumble::ERace kRaceValues[] = {
    Mumble::ERace::Asura, Mumble::ERace::Charr, Mumble::ERace::Human,
    Mumble::ERace::Norn,  Mumble::ERace::Sylvari,
};
constexpr Mumble::EProfession kProfessionValues[] = {
    Mumble::EProfession::Guardian,     Mumble::EProfession::Warrior,
    Mumble::EProfession::Engineer,     Mumble::EProfession::Ranger,
    Mumble::EProfession::Thief,        Mumble::EProfession::Elementalist,
    Mumble::EProfession::Mesmer,       Mumble::EProfession::Necromancer,
    Mumble::EProfession::Revenant,
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
// specializationText  autocomplete box's live text, resolved to an id at
//                     compose time
// showSpecSuggest     visibility of the suggestion window, tracked by hand
//--------------------------------------------------------------------------------
// One row holds everything editable about a single GUID entry, including
// its self context -- which used to be an all-or-nothing snapshot hidden
// entirely when absent, but is now four independently editable fields.
// Each is pre-filled from a live-log snapshot when "report new" was
// clicked and that GUID had one, snapshotted once at that point (not
// re-read live, so the form doesn't change under the user while they're
// filling it out); otherwise it starts at its own default, still fully
// editable by hand.
//--------------------------------------------------------------------------------
struct ReportFormRow
{
    char                 guid[128] = {};
    char                 typeText[8] = {};
    int                  mapID = 0;
    int                  raceIndex = kRaceUnset;
    Mumble::EProfession  profession = Mumble::EProfession::None;
    char                 specializationText[64] = {};
    bool                 showSpecSuggest = false;
};

static bool                       s_reportAnonymous = false;
static char                       s_reportAccountNameBuf[128]   = {};
static char                       s_reportCharacterNameBuf[128] = {};
static std::vector<ReportFormRow> s_reportRows;
static char                       s_reportNoteBuf[1024] = {};

//_ Validation error shown until the next attempt, cleared on success.
static std::string                s_reportFormError;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RefreshReportNameFieldsFromGameState
//--------------------------------------------------------------------------------
// Re-reads Account/Character Name from GameState and overwrites the
// report form's name buffers with whatever's live right now. Called only
// from AddReportRowFromLiveLogEntry (every "report new" click) -- not on
// section render/addon load, and not for a manually-added row.
//
// Overwrite is unconditional, not "only if still blank" -- a later click
// re-syncs with whoever's actually playing now, even if the user had
// typed something else into the boxes since (e.g. switched characters
// between two clicks).
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
// See report_ui.h for the contract. Refreshes the name fields here rather
// than on render -- see RefreshReportNameFieldsFromGameState's comment
// for why that's tied to this call specifically.
//--------------------------------------------------------------------------------
void AddReportRowFromLiveLogEntry(const LiveLogEntry& entry)
{
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
    //_ else: leave every field at its default -- nothing was ever
    // observed for this GUID, same as before, just no longer hidden.
    s_reportRows.push_back(row);
}

namespace {

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// TrimReportText
//--------------------------------------------------------------------------------
// Local to this file's report-form code -- report.cpp has its own Trim
// for the same purpose, private to that translation unit.
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
// "Not set" tri-state: blank, or case-insensitively "not set", means
// unset; otherwise must parse as 0-11. Returns false (with outError set)
// for anything else, e.g. stray text or a number outside that range.
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
// Every known (id, name) pair, built once by probing SpecializationName()
// across an id range comfortably past today's 81-entry table -- so a
// newly-added elite spec shows up automatically once that table is
// updated, without touching this loop. Kept as strings (not const char*)
// since SpecializationName() only promises its return value for the
// call's duration, not this cache's lifetime.
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
// Resolves the autocomplete box's free-typed text to a specialization id:
// blank -> 0 ("not set"); an all-digits string -> that raw id, taken as
// typed (still allowed, e.g. a future elite spec not yet in this table);
// otherwise an exact case-insensitive name match. Anything else (a typo,
// a partial word) resolves to 0/"not set" rather than rejecting the
// submission -- a convenience field, not a validated one.
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
// Renders the per-GUID block template for the relay payload -- the one
// place that turns raw enum/numeric self-context values into the
// human-readable names the live log's tree already uses; report.cpp
// never sees anything but this finished string.
//
// Self context now has four independently-editable fields rather than
// one all-or-nothing snapshot: only when every field is still at its
// default does this print "Not observed", otherwise it prints all four,
// substituting "Unknown" for whichever are still unset.
//--------------------------------------------------------------------------------
std::string ComposeReportGuidBlock(const std::string& guid, bool typeIsSet, int typeValue, const ReportFormRow& row)
{
    std::ostringstream out;
    out << "GUID: " << guid << "\n";
    out << "  Type: " << (typeIsSet ? std::to_string(typeValue) : std::string("Not set")) << "\n";

    unsigned int specId  = ResolveSpecializationId(row.specializationText);
    bool         raceSet = (row.raceIndex != kRaceUnset);
    bool         profSet = (row.profession != Mumble::EProfession::None);
    bool         specSet = (specId != 0);
    bool         mapSet  = (row.mapID != 0);

    if (!mapSet && !raceSet && !profSet && !specSet)
    {
        out << "  Self context: Not observed\n";
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

        out << "  Self context: MapID " << row.mapID << ", "
            << (raceSet ? GameState_RaceName(kRaceValues[row.raceIndex]) : "Unknown") << ", "
            << (profSet ? GameState_ProfessionName(row.profession) : "Unknown") << ", "
            << specName << "\n";
    }
    return out.str();
}

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderReportSection
//--------------------------------------------------------------------------------
// Lazily loads the installed-effects tree (same pattern as
// RenderInstalledEffects) so per-GUID display names have something to
// check against even if opened before "Installed Effects".
//
// Reporter-identity fields are only auto-filled/re-synced by "report new"
// (see RefreshReportNameFieldsFromGameState); this function just displays
// and edits whatever's currently in the buffers, starting blank each
// session until the first click.
//--------------------------------------------------------------------------------
void RenderReportSection(const std::string& denoiserAddonDir)
{
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    const float kFieldWidth = 320.0f;
    EReportStatus reportStatus = GetReportStatus();
    bool sending = (reportStatus == EReportStatus::Sending);

    ImGui::PushItemWidth(kFieldWidth);
    if (s_reportAnonymous)
    {
        //_ Show fixed text rather than an editable box the user could
        // half-clear and retype into, which would defeat "anonymous".
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
        "Attach zero or more GUIDs -- click \"report new\" next to an entry in the "
        "Live Log, or add one by hand below.");

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

        //_ Self context is always shown now (never hidden) and every
        // field is independently editable; all four sit on one line
        // by design, so a report with self context reads as one unit.
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
        const bool  profUnset    = (row.profession == Mumble::EProfession::None);
        const char* profPreview  = profUnset ? "Profession" : GameState_ProfessionName(row.profession);
        if (ImGui::BeginCombo("##combo_profession", profPreview))
        {
            if (ImGui::Selectable("Profession", profUnset))
                row.profession = Mumble::EProfession::None;
            for (Mumble::EProfession p : kProfessionValues)
            {
                bool selected = (row.profession == p);
                if (ImGui::Selectable(GameState_ProfessionName(p), selected))
                    row.profession = p;
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();

        //_ Free-text box with live autocomplete rather than a dropdown --
        // easier to search 80+ names by typing than by scrolling.
        ImGui::PushItemWidth(90.0f);
        bool specTextChanged = ImGui::InputTextWithHint("##combo_specialization", "Specialization", row.specializationText, sizeof(row.specializationText));
        bool specJustActivated = ImGui::IsItemActivated();
        bool specDeactivated   = ImGui::IsItemDeactivated();
        ImVec2 specBoxMin = ImGui::GetItemRectMin();
        ImVec2 specBoxMax = ImGui::GetItemRectMax();
        ImGui::PopItemWidth();
        ImGui::SameLine();

        //_ Plain Begin()/End() window, not a real Popup -- NoFocusOnAppearing
        // keeps input focus, but a real Popup would get torn down every
        // frame by ClosePopupsOverWindow; visibility is tracked by hand.
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

    if (ImGui::SmallButton("+ Add GUID manually"))
        s_reportRows.push_back(ReportFormRow{});

    ImGui::Spacing();
    ImGui::TextDisabled("Additional information (required):");
    ImGui::InputTextMultiline("##report_note", s_reportNoteBuf, sizeof(s_reportNoteBuf), ImVec2(kFieldWidth, 80));

    //_ imgui 1.80 doesn't have BeginDisabled/EndDisabled -- swap the label
    // and ignore clicks while busy instead, same workaround used elsewhere
    // in this addon (e.g. "Check now"/"Apply this update").
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
                reporterLine = "Reporter: (anonymous)";
            }
            else
            {
                //_ Use whatever's actually in the boxes now -- may be the
                // auto-filled value untouched, edited, or (if auto-fill
                // had nothing to offer) typed from scratch.
                std::string acct = TrimReportText(s_reportAccountNameBuf);
                std::string chr  = TrimReportText(s_reportCharacterNameBuf);
                if (acct.empty()) acct = "(unknown)";
                if (chr.empty())  chr  = "(unknown)";
                reporterLine = "Reporter: " + acct + " / " + chr;
            }

            if (StartSendReport(reporterLine, payloadEntries, s_reportNoteBuf, error))
            {
                s_reportFormError.clear();
                s_reportRows.clear();
                s_reportNoteBuf[0] = '\0';
            }
            else
            {
                s_reportFormError = error;
            }
        }
    }

    if (!s_reportFormError.empty())
        ImGui::TextColored(kDuplicateColor, "%s", s_reportFormError.c_str());

    std::string lastMsg = GetLastReportMessage();
    if (!lastMsg.empty())
    {
        const ImVec4* color = (reportStatus == EReportStatus::Error) ? &kDuplicateColor : nullptr;
        if (color)
            ImGui::TextColored(*color, "%s", lastMsg.c_str());
        else
            ImGui::TextWrapped("%s", lastMsg.c_str());
    }
}