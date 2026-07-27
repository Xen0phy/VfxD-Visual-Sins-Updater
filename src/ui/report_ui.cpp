// report_ui.cpp
//
// "Report an Effect" options-panel section. Extracted from addon.cpp --
// a mechanical move, no behavior change. See report_ui.h for what's
// exposed and why.
#include "ui/report_ui.h"
#include "integration/webhook_report.h"
#include "imgui.h"
#include "addon/ui_colors.h"
#include "core/tree/installed_tree_store.h"
#include "core/game_state.h"
#include "core/specialization_names.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>

// ---------------------------------------------------------------------------
// Manual "report an effect back" form: a
// reporter identity line (Account/Character name, or anonymous), zero or
// more per-GUID blocks (each with its own Type and a snapshot of that
// GUID's self-context, if it has one), and one required free-text note.
// Validation itself lives in report.cpp's StartSendReport; this section's
// job is the form widgets, composing each GUID's display block, and
// showing whatever outcome comes back.
//
// One row here holds everything editable about a single GUID entry,
// including its self context -- which used to be an all-or-nothing
// snapshot hidden entirely when absent, but is now four independently
// editable fields, each
// pre-filled from a live-log snapshot when "report new" was clicked and
// that GUID had one, or left at its own "not set" default otherwise.
// Snapshotted once at that point either way -- not re-read live -- so
// this form doesn't change under the user's feet while they're still
// filling it out; from there the user can edit any of it by hand.
// ---------------------------------------------------------------------------
namespace {

// Race has no None/Unknown enumerator in Mumble.h, so -1 is this form's
// own sentinel for "not set" -- never a real
// ERace value. Profession, unlike Race, already has a real None (0), so
// it doubles as both the enum's own "no profession" value AND this
// field's placeholder -- no separate sentinel needed there.
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
    return kRaceUnset; // shouldn't happen -- ERace has no values outside kRaceValues
}

} // namespace

struct ReportFormRow
{
    char guid[128] = {};
    char typeText[8] = {}; // digits (0-11) or blank/"Not set" -- see ParseReportTypeText

    // Self context -- individually editable now, not gated behind a
    // single hasSelfContext
    // bool anymore. Each field has its own "not set" default and is
    // pre-filled independently from a live-log snapshot when one
    // exists; a manually-added row (or one for a GUID that never took
    // the isSelfEvent branch) simply starts every field at its default,
    // still fully editable by hand.
    int                 mapID          = 0;           // 0 doubles as "not set" -- MapID stays a raw number either way
    int                 raceIndex      = kRaceUnset;   // index into kRaceValues, or kRaceUnset
    Mumble::EProfession profession     = Mumble::EProfession::None; // None doubles as "not set"
    char                specializationText[64] = {};  // autocomplete box's live text; resolved to an id at compose time
};

static bool                       s_reportAnonymous = false;
static char                       s_reportAccountNameBuf[128]   = {};
static char                       s_reportCharacterNameBuf[128] = {};
static std::vector<ReportFormRow> s_reportRows;
static char                       s_reportNoteBuf[1024] = {};
static std::string                s_reportFormError; // validation error shown until the next attempt, cleared on success

// Re-reads Account/Character Name from GameState and overwrites the
// report form's name buffers with whatever's live right now. Called
// only from AddReportRowFromLiveLogEntry (i.e. every "report new"
// click) -- NOT on section render/addon load, and NOT for a manually-
// added row. Auto-fill (and
// re-fill on a later click, if the account/character has since changed
// -- e.g. switched characters between two "report new" clicks) is tied
// to the button, not to opening the Report tab. Unconditional overwrite
// rather than "only if still blank" -- a later click is meant to
// re-sync with whoever's actually playing right now, even if the user
// had typed something else into the boxes in the meantime.
void RefreshReportNameFieldsFromGameState()
{
    std::string acct = GameState_GetAccountName();
    std::string chr  = GameState_GetCharacterName();
    std::snprintf(s_reportAccountNameBuf, sizeof(s_reportAccountNameBuf), "%s", acct.c_str());
    std::snprintf(s_reportCharacterNameBuf, sizeof(s_reportCharacterNameBuf), "%s", chr.c_str());
}

// Appends a row auto-filled from a live-log entry -- this is what the
// live log's per-entry "report new" button (RenderLiveLogSection, now in
// live_log_ui.cpp) calls. Also (re-)syncs the reporter-identity name
// fields from GameState right here -- see
// RefreshReportNameFieldsFromGameState's comment for why that's tied to
// this call specifically. Declared in report_ui.h (not static) so that
// separate translation unit can reach it.
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
    // else: leave every field at its default -- nothing was ever
    // observed for this GUID, same as before, just no longer hidden.
    s_reportRows.push_back(row);
}

namespace {

// Local to this file's report-form code -- report.cpp has its own Trim
// for the same purpose, but it's private to that translation unit, and
// this is the one spot that needs it (user-typed name fields, below).
std::string TrimReportText(const std::string& s)
{
    size_t start = 0, end = s.size();
    while (start < end && std::isspace((unsigned char)s[start])) ++start;
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(start, end - start);
}

// "Not set" tri-state: Type gets its own independent "Not set" state --
// blank (or, case-insensitively, the literal
// text "not set") means unset; otherwise must parse as 0-11. Returns
// false (with outError set) for anything else, e.g. stray text or a
// number outside that range.
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

// Every known (id, name) pair, built once by probing SpecializationName()
// across an id range comfortably past today's 81-entry table (see
// specialization_names.h) -- so a newly-added elite spec shows up in the
// autocomplete list automatically the next time that table's updated,
// without touching this loop again. Kept as strings (not const char*)
// since SpecializationName() only promises its return value is valid for
// the call's duration, not for the lifetime of this cache.
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

// Resolves the autocomplete box's free-typed text to a specialization id:
// blank -> 0 ("not set"); an all-digits string -> that raw id, taken as
// typed (still allowed, same as the old hasSelfContext-snapshot path --
// e.g. a future elite spec not yet in this table); otherwise an exact
// case-insensitive name match against AllSpecializations(). Anything
// else (a typo, a partial word) resolves to 0/"not set" rather than
// rejecting the whole submission -- this field is a convenience, not a
// validated one, same spirit as leaving a GUID row's Type blank.
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

// Renders the per-GUID block template used for the payload sent to the
// relay. This is the one place that turns raw
// enum/numeric self-context values into the human-readable names the
// live log's own tree already uses (GameState_RaceName/ProfessionName,
// SpecializationName) -- report.cpp never sees anything but this
// finished string.
//
// Self context used to be all-or-nothing for the *live-capture* path
// (unaffected by this change): one bundled block, written or not. This
// form now lets the user adjust any of the four
// fields independently, so the composed text follows suit: only when
// every field is still at its own default does this print the original
// "Not observed" line; otherwise it prints all four, substituting
// "Unknown" for whichever ones are still unset -- the same word
// GameState_RaceName/ProfessionName already fall back to for an
// out-of-range value, reused here for "not filled in" instead.
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

} // namespace

void RenderReportSection(const std::string& denoiserAddonDir)
{
    // The tree isn't necessarily loaded yet if the user opens this header
    // before ever expanding "Installed Effects" -- load it here too so
    // per-GUID display names elsewhere in this addon have something to
    // check against, same lazy-load-on-first-open pattern
    // RenderInstalledEffects already uses.
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    const float kFieldWidth = 320.0f;
    EReportStatus reportStatus = GetReportStatus();
    bool sending = (reportStatus == EReportStatus::Sending);

    // --- Reporter identity -----------------------------------------
    // Auto-filled (and re-synced) only by "report new" -- see
    // RefreshReportNameFieldsFromGameState/AddReportRowFromLiveLogEntry
    // above -- NOT here on render/section-open, and NOT re-read every
    // frame either way. This section just displays/edits whatever's
    // currently in the buffers; it never populates them itself. Until
    // the first "report new" click of the session, both boxes simply
    // start blank (showing their hint text) -- there's nothing to
    // auto-fill from yet.
    ImGui::PushItemWidth(kFieldWidth);
    if (s_reportAnonymous)
    {
        // Anonymous wins outright while checked: show fixed text, not an
        // editable box the user could half-clear and retype into,
        // which would defeat "anonymous". Whatever's in the buffers is
        // preserved underneath and reappears if the box is unchecked.
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

    // --- Per-GUID blocks --------------------------------------------
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

        // --- Self context -- always shown now, never hidden, and every
        // field below is independently editable. Each is pre-filled from a live-log
        // snapshot when "report new" populated this row and that GUID
        // had one; otherwise it just starts at its own "not set"
        // default, same as a manually-added row -- either way, nothing
        // here is read-only. All four sit on this same line by design,
        // so a report with self context reads as one unit at a glance.
        ImGui::PushItemWidth(40.0f);
        ImGui::InputInt("MapID", &row.mapID, 0, 0); // no +/- step buttons -- a raw id, not a counter
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

        // Specialization: a free-text box with live autocomplete rather
        // than a dropdown -- easier to search 80+ names by typing than
        // by scrolling. Suggestions render as
        // plain Selectable() rows right below this same input (not a
        // popup) specifically so the input keeps keyboard focus while
        // the user keeps typing; a real popup would steal focus away
        // from it every frame.
        ImGui::PushItemWidth(90.0f);
        bool specTextChanged = ImGui::InputTextWithHint("##combo_specialization", "Specialization", row.specializationText, sizeof(row.specializationText));
        bool specJustActivated = ImGui::IsItemActivated();
        ImVec2 specBoxMin = ImGui::GetItemRectMin();
        ImVec2 specBoxMax = ImGui::GetItemRectMax();
        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::SmallButton("Remove"))
            removeIndex = (int)i;

        // Real floating popup instead of inline Selectable()s gated on
        // IsItemActive() -- clicking a suggestion moves focus off the
        // input in that same frame, so an IsItemActive()-gated inline
        // list can vanish (or shift layout underneath the click) before
        // the click actually lands. OpenPopup here fires once, when you
        // start typing or click into the box -- not every frame based
        // on focus -- so the popup survives the click, and Selectable()
        // inside a popup already closes it correctly on click.
        const char* specPopupId = "##spec_suggest_popup";
        if ((specJustActivated || specTextChanged) && row.specializationText[0] != '\0')
            ImGui::OpenPopup(specPopupId);

        if (ImGui::IsPopupOpen(specPopupId))
        {
            ImGui::SetNextWindowPos(ImVec2(specBoxMin.x, specBoxMax.y));
            ImGui::SetNextWindowSize(ImVec2(specBoxMax.x - specBoxMin.x, 0));
        }
        if (ImGui::BeginPopup(specPopupId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing))
        {
            if (row.specializationText[0] == '\0')
            {
                ImGui::CloseCurrentPopup();
            }
            else
            {
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
                        ImGui::CloseCurrentPopup();
                    }
                    ++shown;
                }
                if (shown == 0)
                    ImGui::TextDisabled("(no match -- will send as Unknown)");
            }
            ImGui::EndPopup();
        }

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

    // imgui 1.80 doesn't have BeginDisabled/EndDisabled -- swap the label
    // and ignore clicks while busy instead, same workaround used elsewhere
    // in this file (e.g. "Check now"/"Apply this update").
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
                // Use whatever's actually in the boxes now -- may be the
                // auto-filled value untouched, edited, or (if
                // auto-fill had nothing to offer) typed from scratch.
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
