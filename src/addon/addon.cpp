//################################################################################
// addon.cpp
//--------------------------------------------------------------------------------
// RenderSinDiffStatus(diff)   result text under a sin's action button
// RenderSinActionRow()        three per-sin action columns (install/check/apply)
// OptionsRenderCallback()     top-level options-panel draw
// Addon_Init(...)             stores aApi/dir/found handed off from entry.cpp
//--------------------------------------------------------------------------------
// The addon's actual behavior, split out from entry.cpp's bare Nexus wiring
// (which owns AddonLoad/AddonUnload, including locating VfxDenoiser and the
// initial silent update check): the options-panel UI (RT_OptionsRender),
// driven by Addon_Init handing off entry.cpp's load-time findings. All the
// update-check/merge logic itself lives in sin_files.*, github_update.*
// and merge.*; this file is UI glue plus the addon's own state (which
// folder it's pointed at, what's currently cached for display) over that.
//
// The addon has no floating window of its own - everything lives inside
// Nexus's own options panel, registered once and drawn only while that
// panel is open. The always-visible installed-effects tree (data owned by
// installed_tree_store.*) is what RenderInstalledEffects draws below the
// action row, and is also what the right-click-to-edit feature extends.
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

//_ Set once via Addon_Init, to the AddonAPI_t pointer entry.cpp got from
// Nexus; only used for aApi->Log here. Never reassigned, so reading it
// later is safe without a lock, same as s_denoiserAddonDir above.
static AddonAPI_t* s_api = nullptr;

//_ Set once via Addon_Init to whether VfxDenoiser's folder actually
// exists; avoids repeatedly rescanning a folder already known missing.
static std::atomic<bool> s_denoiserFound{false};

//_ Set when the user clicks Install/Apply so the right column can say
// Installing.../Applying... instead of a generic busy state -- at most
// one is pending, per github_update.cpp's single in-flight guard.
static std::string s_pendingActionSin;

//_ Set when the user clicks a SQL Install/Apply action so the message
// underneath is attributed to the sin that produced it (ApplySqlUpdate/
// InstallSqlSin are synchronous, so there's no "pending" state the way
// the GitHub column has -- the call has already finished by the time
// the button click is processed).
static std::string s_lastSqlMessageSin;
static std::string s_lastSqlMessage;

//_ Cached synchronous CheckSqlUpdates() result -- see RefreshSqlSinInfo.
// Unlike GetSinUpdateInfo()/GetCheckStatus() (github_update.h), there's
// no background thread updating this on its own; it's refreshed lazily
// on first render and again after anything that could have changed it
// (a successful Install/Apply here), same "re-verify against what's
// actually on disk" reasoning StartApplyUpdate/StartInstallSin already
// apply to their own GitHub-sourced counterparts.
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
// TODO_B.md item 8 / EFFECT_DB_D1_HANDOFF.md's local-db-sourced update
// path -- the real thing item 7's old "Generate from SQL (test)" button
// was standing in for, now mirroring the GitHub column's own Install/
// "Update available" -> "Apply changes"/Up to date shape via
// sql_update.h instead of github_update.h. Deliberately side by side
// with the GitHub column above (owner's call), not replacing it -- the
// GitHub column only goes away once D1 is fully integrated, the last
// step in that direction, not this one.
//
// Unlike the GitHub column, every call here (CheckSqlUpdates/
// LoadSqlDiff/ApplySqlUpdate/InstallSqlSin) is synchronous -- no
// Checking.../Installing... busy state to render, the click has already
// resolved by the time the next line runs. RenderSinDiffStatus is reused
// as-is (it only reads a SinDiffInfo -- see sql_update.h reusing that
// struct/EDiffStatus/ESinUpdateState from github_update.h wholesale, so
// nothing about that function's wording needed to change for a
// SQL-sourced diff instead of a GitHub-sourced one).
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

    //_ No result yet (check errored, or hasn't run) reads as NotInstalled,
    // same fallback RenderSinActionRow uses for the GitHub column.
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
                RefreshSqlSinInfo(denoiserAddonDir); //. re-verify against what's actually on disk
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
                //_ LoadSqlDiff is synchronous -- this status is never
                // actually returned here, kept only because EDiffStatus
                // is shared with the (async) GitHub path.
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
                    RefreshSqlSinInfo(denoiserAddonDir); //. re-verify, same as ApplyUpdate's GitHub counterpart
            }
            else
            {
                LoadSqlDiff(denoiserAddonDir, sinName);
            }
        }

        SinDiffInfo diffForDisplay = GetSqlDiffInfo(sinName);
        RenderSinDiffStatus(&diffForDisplay);
    }
    //_ UpToDate falls here (Unknown too -- nothing actionable, same as
    // the GitHub column's own fallback).
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
// for both installing a sin and checking/applying its pending update -
// deliberately above the collapsing headers so nothing needs expanding.
// NotInstalled calls StartInstallSin directly. UpdateAvailable's button
// doubles as both steps: first click calls StartLoadDiff, then relabels
// to "Apply changes" and calls StartApplyUpdate; RenderSinDiffStatus
// renders the result underneath.
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

    //_ The pending tag only matters while applying; once it settles the
    // label it reserved is stale.
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

    //_ imgui 1.80 lacks BeginDisabled/EndDisabled; buttons below swap
    // label or ignore the click instead of true graying-out.
    //
    // GitHub's own action content is state-dependent height (NotInstalled
    // is a couple lines, UpdateAvailable with a loaded diff can be five
    // or six) -- different per sin. Stacking the SQL section directly
    // underneath it *inside the same per-sin column* would start each
    // sin's SQL block at whatever Y its own GitHub content happened to
    // end at, staggering the three "SQL (local db)" blocks against each
    // other (a staircase, one found by actually looking at the running
    // addon rather than just compiling it). Two separate Columns() rows
    // -- GitHub's, then SQL's -- fixes it: each row starts fresh at the
    // same Y for all three columns (see ImGui::NextColumn()'s LineMinY
    // reset), so only within-row bottoms can differ, not tops.
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

        //_ No result yet (first frame or two after load) reads as
        // NotInstalled; settles once GetSinUpdateInfo() has data.
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
                    //_ Stays until the duplicate GUID is resolved (see
                    // tree below); another click just re-checks it.
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
        //_ UpToDate falls here (Unknown too - treated the same, nothing
        // actionable).
        else
        {
            ImGui::Button("Up to date");
        }

        ImGui::NextColumn();
        ImGui::PopID();
    }
    ImGui::Columns(1);

    ImGui::Separator();

    //_ Second row, same column count -- see the comment above for why
    // this is a separate Columns() call rather than continuing inside
    // the loop above. Column widths are computed the same way both
    // times (same count, same window width), so column i here lines up
    // under column i above without needing to repeat the sin's name.
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

    //_ New content was written to disk; drop the installed-tree cache so
    // it reloads next time expanded (compares against the message text,
    // already polled every frame here).
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

    //_ EffectDb_Poll rate-limits itself internally (see effect_db.h),
    // so calling it unconditionally here is cheap. Latched into a
    // static so a stop message outlives the single frame it is returned on.
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
