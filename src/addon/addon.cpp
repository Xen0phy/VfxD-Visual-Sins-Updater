//################################################################################
// addon.cpp   (see: addon.h)
//--------------------------------------------------------------------------------

#include "addon.h"
#include "backups_ui.h"
#include "effect_db.h"
#include "github_update.h"
#include "imgui.h"
#include "installed_tree_store.h"
#include "installed_tree_view.h"
#include "live_log_ui.h"
#include "report_ui.h"
#include "sin_files.h"
#include "sql_update.h"
#include "ui_colors.h"

#include <atomic>
#include <string>

static std::string s_denoiserAddonDir;

//_ AddonAPI_t from Nexus, set once via Addon_Init and used only for logging.
static AddonAPI_t* s_api = nullptr;

//_ Whether VfxDenoiser's folder exists, skips rescanning one already missing.
static std::atomic<bool> s_denoiserFound{false};

//_ Sin with an Install/Apply in flight, so its button reads Installing/Applying.
static std::string s_pendingActionSin;

//_ Sin name paired with the SQL action's synchronous result message below.
static std::string s_lastSqlMessageSin;
static std::string s_lastSqlMessage;

//_ Cached result of RefreshSqlSinInfo below; nothing refreshes it automatically.
static std::vector<SqlSinUpdateInfo> s_sqlSinInfo;
static bool                          s_sqlSinInfoLoaded = false;
static std::string                   s_sqlCheckError;

namespace {

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RefreshSqlSinInfo
//--------------------------------------------------------------------------------
// Runs CheckSqlUpdates synchronously (a local SQLite read + a filesystem
// scan -- cheap enough to call on demand, not every frame; see
// sql_update.h) and latches the result into the statics above.
//--------------------------------------------------------------------------------
static void RefreshSqlSinInfo(const std::string& denoiserAddonDir)
{
    s_sqlSinInfo = CheckSqlUpdates(denoiserAddonDir, s_sqlCheckError);
    s_sqlSinInfoLoaded = true;
}

} //. namespace

namespace {

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderSinDiffStatus
//--------------------------------------------------------------------------------
// Renders the result text under a sin's action button once its diff state
// has something to say (see RenderSinActionRow). NotLoaded/Loading/Error/
// Blocked cover the check step; Ready covers the loaded plan - empty means
// only a version bump, otherwise counts new/reworked effects (colored to
// match the Installed Effects tree overlay) and flags any merged items
// with conflicting settings. Blocked mirrors the duplicate-GUID gate
// already shown in red on the Installed Effects tree, not a second,
// independent check.
//--------------------------------------------------------------------------------
static void RenderSinDiffStatus(const SinDiffInfo* diff)
{
    if (!diff || diff->status == EDiffStatus::NotLoaded)
        return;

    if (diff->status == EDiffStatus::Loading)
    {
        ImGui::TextDisabled("Downloading changes...");
        return;
    }

    if (diff->status == EDiffStatus::Error)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Couldn't load changes -- click above to retry.");
        return;
    }

    if (diff->status == EDiffStatus::Blocked)
    {
        ImGui::TextColored(kDuplicateColor,
            "Duplicate GUID (see Installed Effects tree above) -- resolve it, then click above to retry.");
        return;
    }

    //_ diff->status == Ready falls through from here.
    const MergePlan& plan = diff->plan;

    if (plan.IsEmpty())
    {
        ImGui::TextDisabled("No effect changes -- just a version bump.");
    }
    else
    {
        ImGui::TextDisabled(
            "%d new, %d refreshed -- see Installed Effects below (green = new, orange = refreshed).",
            (int)plan.inserts.size(), (int)plan.reworks.size());

        int conflictCount = 0;
        for (const auto& rw : plan.reworks)
            if (rw.behaviorsConflict)
                ++conflictCount;
        if (conflictCount > 0)
            ImGui::TextColored(kDuplicateColor, "%d settings conflict%s -- review before applying.",
                conflictCount, conflictCount == 1 ? "" : "s");
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderSqlSinAction
//--------------------------------------------------------------------------------
// The local-db-sourced update path, mirroring the GitHub column's own
// Install/"Update available" -> "Apply changes"/Up to date shape via
// sql_update.h instead of github_update.h. Sits alongside the GitHub column,
// both active until the local-db path is fully integrated.
//
// Every call here is synchronous, so unlike the GitHub column there's no
// Checking.../Installing... busy state. RenderSinDiffStatus is reused as-is
// since sql_update.h reuses SinDiffInfo/EDiffStatus/ESinUpdateState from
// github_update.h wholesale.
//--------------------------------------------------------------------------------
static void RenderSqlSinAction(const std::string& denoiserAddonDir, const std::string& sinName)
{
    if (!s_sqlSinInfoLoaded)
        RefreshSqlSinInfo(denoiserAddonDir);

    ImGui::TextDisabled("SQL (local db)");

    if (!s_sqlCheckError.empty())
    {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", s_sqlCheckError.c_str());
        if (ImGui::SmallButton("Retry##sqlCheck"))
            RefreshSqlSinInfo(denoiserAddonDir);
    }

    const SqlSinUpdateInfo* info = nullptr;
    for (const auto& s : s_sqlSinInfo)
        if (s.sinName == sinName) { info = &s; break; }

    //_ No result reads as NotInstalled, the same fallback RenderSinActionRow uses.
    ESinUpdateState state = info ? info->state : ESinUpdateState::NotInstalled;

    if (state == ESinUpdateState::NotInstalled)
    {
        if (ImGui::Button("Install##sql"))
        {
            std::string msg;
            bool ok = InstallSqlSin(denoiserAddonDir, sinName, msg);
            s_lastSqlMessageSin = sinName;
            s_lastSqlMessage    = msg;
            if (ok)
                RefreshSqlSinInfo(denoiserAddonDir); //. re-verify against disk
        }
    }
    else if (state == ESinUpdateState::UpdateAvailable)
    {
        if (info)
            ImGui::Text("%d guids -> %d guids", info->installedVersion, info->latestVersion);

        SinDiffInfo diff = GetSqlDiffInfo(sinName);

        const char* label = "Update available";
        bool clickable = true;
        bool isApplyStep = false;
        switch (diff.status)
        {
            case EDiffStatus::NotLoaded:
                label = "Update available"; break;
            case EDiffStatus::Ready:
                label = "Apply changes"; isApplyStep = true; break;
            case EDiffStatus::Error:
                label = "Error -- retry"; break;
            case EDiffStatus::Blocked:
                label = "Blocked -- see below"; break;
            case EDiffStatus::Loading:
                //_ Never returned here (LoadSqlDiff is synchronous); kept for the shared enum.
                label = "Loading..."; clickable = false; break;
        }

        if (ImGui::Button((std::string(label) + "##sql").c_str()) && clickable)
        {
            if (isApplyStep)
            {
                std::string msg;
                bool ok = ApplySqlUpdate(denoiserAddonDir, sinName, msg);
                s_lastSqlMessageSin = sinName;
                s_lastSqlMessage    = msg;
                if (ok)
                    RefreshSqlSinInfo(denoiserAddonDir); //. re-verify against disk
            }
            else
            {
                LoadSqlDiff(denoiserAddonDir, sinName);
            }
        }

        SinDiffInfo diffForDisplay = GetSqlDiffInfo(sinName);
        RenderSinDiffStatus(&diffForDisplay);
    }
    //_ UpToDate falls here (Unknown too), same as the GitHub column's fallback.
    else
    {
        ImGui::Button("Up to date##sql");
    }

    if (s_lastSqlMessageSin == sinName && !s_lastSqlMessage.empty())
        ImGui::TextWrapped("%s", s_lastSqlMessage.c_str());
}

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderSinActionRow
//--------------------------------------------------------------------------------
// Three always-visible per-sin columns (kSinNames order), the entry point
// for both installing a sin and checking/applying its pending update,
// above the collapsing headers so nothing needs expanding first.
// NotInstalled calls StartInstallSin directly. UpdateAvailable's button
// doubles as both steps: first click calls StartLoadDiff, then relabels
// to "Apply changes" and calls StartApplyUpdate; RenderSinDiffStatus
// renders the result underneath. Draws GitHub and SQL actions as two
// separate Columns() rows (not stacked) so each row starts at a
// consistent Y regardless of the other's per-sin content height.
//--------------------------------------------------------------------------------
static void RenderSinActionRow()
{
    static const char* kSinDescriptions[kSinCount] = {
        "Hides all collected effects.",
        "Hides all collected effects from other players.",
        "Hides all collected effects from other players; insecure effects are also removed.",
    };

    ECheckStatus checkStatus = GetCheckStatus();
    EApplyStatus applyStatus = GetApplyStatus();
    bool checking = (checkStatus == ECheckStatus::Checking);
    bool applying = (applyStatus == EApplyStatus::Applying);

    //_ The pending tag only matters while applying; it's stale once that settles.
    if (!applying)
        s_pendingActionSin.clear();

    if (checkStatus == ECheckStatus::Error)
    {
        std::string why = GetLastCheckMessage();
        if (why.empty())
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Last check failed -- showing previous results, if any.");
        else
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Last check failed: %s", why.c_str());
    }

    std::vector<SinUpdateInfo> sinInfo = GetSinUpdateInfo();
    std::vector<SinDiffInfo>   diffs   = GetSinDiffInfo();

    //_ imgui 1.80 lacks BeginDisabled/EndDisabled; buttons swap label or ignore clicks.
    ImGui::Columns(kSinCount, "sin_github_columns", false);
    for (int i = 0; i < kSinCount; ++i)
    {
        std::string sinName = kSinNames[i];
        ImGui::PushID(sinName.c_str());

        ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f), "%s", sinName.c_str());
        ImGui::TextWrapped("%s", kSinDescriptions[i]);
        ImGui::TextDisabled("GitHub");

        const SinUpdateInfo* info = nullptr;
        for (const auto& s : sinInfo)
            if (s.sinName == sinName) { info = &s; break; }

        //_ No result yet reads as NotInstalled; settles once GetSinUpdateInfo() has data.
        ESinUpdateState state = info ? info->state : ESinUpdateState::NotInstalled;
        bool pendingHere = (applying && s_pendingActionSin == sinName);

        if (checking)
        {
            ImGui::Button("Checking...");
        }
        else if (state == ESinUpdateState::NotInstalled)
        {
            bool hasUrl = info && !info->latestDownloadUrl.empty();
            const char* label = pendingHere ? "Installing..." : "Install";
            if (ImGui::Button(label) && !applying && hasUrl)
            {
                s_pendingActionSin = sinName;
                StartInstallSin(s_denoiserAddonDir, sinName);
            }
            if (!hasUrl && !pendingHere)
                ImGui::TextDisabled("Not available yet.");
        }
        else if (state == ESinUpdateState::UpdateAvailable)
        {
            const SinDiffInfo* diff = nullptr;
            for (const auto& d : diffs)
                if (d.sinName == sinName) { diff = &d; break; }
            EDiffStatus diffStatus = diff ? diff->status : EDiffStatus::NotLoaded;

            if (info)
                ImGui::Text("v%d -> v%d", info->installedVersion, info->latestVersion);

            const char* label = "Update available";
            bool clickable = false;
            bool isApplyStep = false;
            switch (diffStatus)
            {
                case EDiffStatus::NotLoaded:
                    label = "Update available"; clickable = true; break;
                case EDiffStatus::Loading:
                    label = "Loading...";       clickable = false; break;
                case EDiffStatus::Ready:
                    label = pendingHere ? "Applying..." : "Apply changes";
                    clickable = !pendingHere; isApplyStep = true; break;
                case EDiffStatus::Error:
                    label = "Error -- retry";   clickable = true; break;
                case EDiffStatus::Blocked:
                    //_ Stays until the duplicate GUID below is resolved; a click just re-checks.
                    label = "Blocked -- see below"; clickable = true; break;
            }

            if (ImGui::Button(label) && clickable && !applying)
            {
                if (isApplyStep)
                {
                    s_pendingActionSin = sinName;
                    StartApplyUpdate(s_denoiserAddonDir, sinName);
                }
                else
                {
                    StartLoadDiff(s_denoiserAddonDir, sinName);
                }
            }

            RenderSinDiffStatus(diff);
        }
        //_ UpToDate falls here (Unknown too), nothing actionable either way.
        else
        {
            ImGui::Button("Up to date");
        }

        ImGui::NextColumn();
        ImGui::PopID();
    }
    ImGui::Columns(1);

    ImGui::Separator();

    //_ Same column count as above, so column i here sits under column i above.
    ImGui::Columns(kSinCount, "sin_sql_columns", false);
    for (int i = 0; i < kSinCount; ++i)
    {
        std::string sinName = kSinNames[i];
        ImGui::PushID(sinName.c_str());

        RenderSqlSinAction(s_denoiserAddonDir, sinName);

        ImGui::NextColumn();
        ImGui::PopID();
    }
    ImGui::Columns(1);

    std::string lastMsg = GetLastApplyMessage();
    if (!lastMsg.empty())
        ImGui::TextWrapped("%s", lastMsg.c_str());

    //_ New content on disk drops the installed-tree cache so it reloads once expanded.
    static std::string s_lastSeenApplyMsg;
    if (lastMsg != s_lastSeenApplyMsg)
    {
        s_lastSeenApplyMsg = lastMsg;
        if (!lastMsg.empty())
            InvalidateInstalledTree();
    }

    ImGui::Separator();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// OptionsRenderCallback
//--------------------------------------------------------------------------------
// Top-level options-panel draw: the sin action row, then Installed
// Effects/Live Log/Backups/Report sections as collapsing headers. Shows a
// disabled message instead if VfxDenoiser isn't installed.
//--------------------------------------------------------------------------------
void OptionsRenderCallback()
{
    if (!s_denoiserFound.load())
    {
        ImGui::TextDisabled("VfxDenoiser isn't installed -- nothing to update.");
        return;
    }

    //_ EffectDb_Poll rate-limits itself internally; latched to outlive one frame.
    static std::string s_effectDbStoppedMsg;
    std::string polled = EffectDb_Poll(s_denoiserAddonDir);
    if (!polled.empty())
        s_effectDbStoppedMsg = polled;

    if (!s_effectDbStoppedMsg.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_effectDbStoppedMsg.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss##effectDbStoppedMsg"))
            s_effectDbStoppedMsg.clear();
    }

    //_ imgui 1.80 has no SeparatorText (added in a later version).
    ImGui::Text("Visual Sins Updater");
    ImGui::Separator();

    RenderSinActionRow();

    if (ImGui::CollapsingHeader("Installed Effects"))
        RenderInstalledEffects(s_denoiserAddonDir);

    if (ImGui::CollapsingHeader("Live Log (VfxDenoiser)"))
        RenderLiveLogSection(s_api, s_denoiserAddonDir);

    if (ImGui::CollapsingHeader("Backups"))
        RenderBackupsSection(s_denoiserAddonDir);

    if (ImGui::CollapsingHeader("Report an Effect"))
        RenderReportSection(s_denoiserAddonDir);
}

void Addon_Init(AddonAPI_t* aApi, const std::string& denoiserAddonDir, bool denoiserFound)
{
    s_api = aApi;
    s_denoiserAddonDir = denoiserAddonDir;
    s_denoiserFound.store(denoiserFound);
    InstalledTreeStore_SetApi(aApi);
}