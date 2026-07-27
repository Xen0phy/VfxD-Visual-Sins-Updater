// addon.cpp
//
// The addon's actual behavior, split out from entry.cpp's bare Nexus
// wiring (which now owns AddonLoad/AddonUnload themselves, including
// locating VfxDenoiser and kicking off/tearing down the initial silent
// update check): the options-panel UI (RT_OptionsRender), driven by
// Addon_Init below handing off entry.cpp's load-time findings. All the
// update-check/merge logic itself lives in sin_files.*, github_update.*
// and merge.*; this file is UI glue plus the addon's own state (which
// folder it's pointed at, what's currently cached for display) over that.
//
// The addon has no floating window of its own -- everything lives inside
// Nexus's own options panel (RT_OptionsRender), registered once and drawn
// only while that panel is open.
#include "addon/addon.h"
#include "imgui.h"
#include "integration/github_update.h"
#include "core/sin_files.h"
#include "addon/ui_colors.h"
#include "core/tree/installed_tree_store.h"
#include "ui/tree/installed_tree_view.h"
#include "ui/report_ui.h"
#include "ui/backups_ui.h"
#include "ui/live_log_ui.h"
#include <string>
#include <atomic>

static std::string s_denoiserAddonDir;

// Set once, via Addon_Init (called from entry.cpp's AddonLoad), to the same
// AddonAPI_t pointer entry.cpp got from Nexus. Only used for aApi->Log calls
// from this file (SaveInstalledSinFile's write-failure path) -- never
// reassigned afterward, so reading it later is safe without a lock, same as
// s_denoiserAddonDir below.
static AddonAPI_t* s_api = nullptr;

// Set once, via Addon_Init, to true only if VfxDenoiser's addon folder
// actually exists (as determined by entry.cpp's AddonLoad) -- avoids
// repeatedly rescanning a folder we already know isn't there.
static std::atomic<bool> s_denoiserFound{false};

// ---------------------------------------------------------------------------
// Always-visible read-only effect tree (separate from the update diff view
// below it). The "what's actually installed" data itself -- the parsed
// sin files, the loaded/generation bookkeeping -- now lives in
// installed_tree_store.h/.cpp; see that header for the read/write API.
// This is also the renderer the right-click-to-edit feature extends.
// ---------------------------------------------------------------------------

// Set right when the user clicks Install or Apply changes (both live in the
// top action row's per-sin button now, see RenderSinActionRow) --
// StartInstallSin/StartApplyUpdate already
// serialize with each other via github_update.cpp's own single in-flight
// guard, so at most one of these is ever meaningfully "the" pending one;
// this just lets the right column say "Installing.../Applying..." instead
// of every column reading the same generic busy state.
static std::string s_pendingActionSin;

namespace {

static void RenderSinDiffStatus(const SinDiffInfo* diff)
{
    if (!diff || diff->status == EDiffStatus::NotLoaded)
        return; // button above already reads "Update available"; nothing more to say yet

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
        // Set by StartLoadDiff, which deliberately never even downloaded
        // anything for this sin -- see its own comment. Same underlying
        // condition RenderInstalledEffects already shows in red on the
        // tree above; this is the same gate surfacing on the update side
        // rather than a second, independent check.
        ImGui::TextColored(kDuplicateColor,
            "Duplicate GUID (see Installed Effects tree above) -- resolve it, then click above to retry.");
        return;
    }

    // diff->status == Ready from here on.
    const MergePlan& plan = diff->plan;

    if (plan.IsEmpty())
    {
        // Version bumped upstream but nothing this addon tracks actually
        // changed (e.g. only metadata outside the merge rules changed) --
        // still safe/useful to let the user bump the stored version.
        ImGui::TextDisabled("No effect changes -- just a version bump.");
    }
    else
    {
        // Per-item detail (which effects, old/new guids, category
        // placement) is shown as coloring directly in the "Installed
        // Effects" tree below -- BuildDiffOverlayTree overlays this same
        // plan onto it -- rather than a second, separate list here.
        ImGui::TextDisabled(
            "%d new, %d refreshed -- see Installed Effects below (green = new, orange = refreshed).",
            (int)plan.inserts.size(), (int)plan.reworks.size());

        // A merge (case 1c) whose folded-together candidates had
        // different settings is flagged here too -- purely informational,
        // never blocks Apply, but worth calling out right under the
        // button that would apply it rather than only as coloring several
        // scrolls down in the tree.
        int conflictCount = 0;
        for (const auto& rw : plan.reworks)
            if (rw.behaviorsConflict)
                ++conflictCount;
        if (conflictCount > 0)
            ImGui::TextColored(kDuplicateColor, "%d settings conflict%s -- review before applying.",
                conflictCount, conflictCount == 1 ? "" : "s");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Three always-visible columns, one per known sin (kSinNames order:
// Gluttony, Pride, Sloth) -- the entry point for both "get this sin at all"
// and "see/apply a pending update," entirely from up here. Deliberately
// placed above the collapsing headers so it doesn't require expanding
// anything, and is now the ONLY place any of this lives -- there is no
// separate "Check now" section below anymore.
//
// NotInstalled calls StartInstallSin directly (a fresh file, nothing to
// preview -- there's no local copy to diff against). UpdateAvailable's
// button doubles as both steps of the check->apply cycle for just that one
// sin: first click calls StartLoadDiff, and once that resolves, the same
// button relabels to "Apply changes" and applies via StartApplyUpdate. The
// diff's result text (RenderSinDiffStatus) renders right under that same
// button once it has something to say, rather than in a separate section
// elsewhere -- see that function's own comment for why.
// ---------------------------------------------------------------------------
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

    // The pending-sin tag only means anything while something is actually
    // applying -- once it settles (Done/Error/Idle) the label it was
    // reserving is stale.
    if (!applying)
        s_pendingActionSin.clear();

    if (checkStatus == ECheckStatus::Error)
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Last check failed -- showing previous results, if any.");

    std::vector<SinUpdateInfo> sinInfo = GetSinUpdateInfo();
    std::vector<SinDiffInfo>   diffs   = GetSinDiffInfo();

    // imgui 1.80 doesn't have BeginDisabled/EndDisabled -- every button
    // below follows addon.cpp's existing convention elsewhere (swap the
    // label, ignore the click) rather than true graying-out.
    ImGui::Columns(kSinCount, "sin_action_columns", false);
    for (int i = 0; i < kSinCount; ++i)
    {
        std::string sinName = kSinNames[i];
        ImGui::PushID(sinName.c_str());

        ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f), "%s", sinName.c_str());
        ImGui::TextWrapped("%s", kSinDescriptions[i]);

        const SinUpdateInfo* info = nullptr;
        for (const auto& s : sinInfo)
            if (s.sinName == sinName) { info = &s; break; }

        // No result yet at all (e.g. the very first frame or two after
        // addon load, before the on-load check has landed) reads the same
        // as NotInstalled for button purposes -- it'll settle within a
        // frame or two once GetSinUpdateInfo() has something.
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
            // Same button doubles as two steps: first click loads just
            // THIS sin's diff (StartLoadDiff's per-sin filter -- doesn't
            // touch the other two outdated sins, if any), which is also
            // what makes RenderSinDiffStatus below have something to show
            // and the colored Installed Effects tree overlay appear
            // further down (both already key off GetSinDiffInfo(); no
            // separate wiring needed for that part). Once that diff is
            // Ready, the same button relabels to "Apply changes" and a
            // second click applies it via StartApplyUpdate.
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
                    // Never becomes Ready until the duplicate guid this is
                    // warning about is resolved (see the Installed
                    // Effects tree below) -- clicking this button again
                    // just re-checks that.
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
        else // UpToDate (or Unknown, treated the same -- nothing actionable)
        {
            ImGui::Button("Up to date");
        }

        ImGui::NextColumn();
        ImGui::PopID();
    }
    ImGui::Columns(1);

    std::string lastMsg = GetLastApplyMessage();
    if (!lastMsg.empty())
        ImGui::TextWrapped("%s", lastMsg.c_str());

    // An apply/install just wrote new content to disk -- drop the
    // installed-tree cache so the next time that section is open/expanded
    // it reloads from the just-written file rather than showing what was
    // there before the update. Compared against the message text rather
    // than a one-shot flag since GetLastApplyMessage() is what's already
    // being polled every frame here.
    static std::string s_lastSeenApplyMsg;
    if (lastMsg != s_lastSeenApplyMsg)
    {
        s_lastSeenApplyMsg = lastMsg;
        if (!lastMsg.empty())
            InvalidateInstalledTree();
    }

    ImGui::Separator();
}

void OptionsRenderCallback()
{
    if (!s_denoiserFound.load())
    {
        ImGui::TextDisabled("VfxDenoiser isn't installed -- nothing to update.");
        return;
    }

    // imgui 1.80 doesn't have SeparatorText (added in a later version).
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