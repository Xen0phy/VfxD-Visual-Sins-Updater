// entry.cpp
//
// Nexus wiring: GetAddonDef (the sole DLL export Nexus looks for), the
// AddonLoad/AddonUnload functions actually assigned into AddonDefinition_t
// -- including everything that happens on load/unload itself (locating
// VfxDenoiser, wiring up the game-state/live-log/update-check subsystems,
// registering the options-panel callback, and tearing all of that down
// again) -- and the DllMain every Windows DLL needs. addon.cpp only owns
// what happens *after* load: the options-panel UI (OptionsRenderCallback)
// and the addon state (Addon_Init) that UI reads. Kept separate on
// purpose: this file is "what Nexus expects from an addon, and what
// happens at those two moments", addon.cpp is "what the addon looks like
// the rest of the time".
#include "Nexus.h"
#include "addon/addon.h"
#include "addon/version.h"
#include "imgui.h"
#include "integration/github_update.h"
#include "integration/webhook_report.h"
#include "core/live_log.h"
#include "core/game_state.h"
#include <chrono>
#include <thread>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

static AddonDefinition_t s_addonDef{};
static AddonAPI_t*       s_api = nullptr;

void AddonLoad(AddonAPI_t* aApi)
{
    s_api = aApi;

    ImGui::SetCurrentContext((ImGuiContext*)aApi->ImguiContext);

    SetUpdaterLogger(aApi); // so github_update.cpp's background-thread failures can also reach Nexus's log
    GameState_Init(aApi);   // caches DL_MUMBLE_LINK/_IDENTITY and DL_RTAPI pointers -- see game_state.h
    LiveLog_Init(aApi);     // subscribes EV_VFXD_SINS_LOG -- see live_log.h/vfxd_sins_bridge.h

    // "<GW2>/addons/VfxDenoiser" -- Paths_GetAddonDirectory only ever
    // *constructs* this path string (see its doc comment in Nexus.h); it
    // doesn't check whether the folder is actually there. Checking
    // fs::is_directory ourselves is what makes `found` mean what it says.
    std::string denoiserAddonDir = aApi->Paths_GetAddonDirectory("VfxDenoiser");
    std::error_code ec;
    bool found = !denoiserAddonDir.empty() && fs::is_directory(denoiserAddonDir, ec) && !ec;

    // Hands addon.cpp the api pointer and this load-time result -- both
    // are referenced throughout the options-panel rendering, not just here.
    Addon_Init(aApi, denoiserAddonDir, found);

    aApi->GUI_Register(RT_OptionsRender, OptionsRenderCallback);

    if (found)
    {
        // One check on load -- version numbers only, no downloading (see
        // StartUpdateCheck's alsoLoadDiff parameter). If this finds an
        // update it just shows as a note in the options panel; the
        // "Check now" button is what actually downloads and diffs
        // anything, so nothing gets fetched until the user asks for it.
        StartUpdateCheck(denoiserAddonDir);
    }
    else
    {
        aApi->Log(LOGL_INFO, "VfxDSinsUpdater", "VfxDenoiser addon folder not found -- nothing to check.");
    }
}

void AddonUnload()
{
    if (s_api)
        s_api->GUI_Deregister(OptionsRenderCallback);

    LiveLog_Shutdown(s_api); // unsubscribes, and raises LISTEN_STOP if capture was still on (no-op if s_api is null)
    GameState_Shutdown();    // clears cached DataLink pointers -- safe even if GameState_Init was never reached

    // Unblock any WinHTTP call currently parked mid-request so the
    // background thread can exit promptly.
    CancelInFlightUpdateRequest();
    CancelInFlightReportRequest();

    // Best-effort: give any in-flight background thread a brief window to
    // notice the cancellation and exit before the DLL potentially gets
    // unloaded out from under it. This is a minimal safety net, not a
    // guarantee -- a production version of this should track its threads
    // explicitly (e.g. the reference project's WaitForBackgroundThreads)
    // rather than relying on a fixed sleep.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    s_addonDef.Signature   = 0x56465344; // 'VFSD'
    s_addonDef.APIVersion  = NEXUS_API_VERSION;
    s_addonDef.Name        = "Visual Sins Updater";
    s_addonDef.Version     = { Maj, Min, Bld, Rev };
    s_addonDef.Author      = "You";
    s_addonDef.Description = "Checks installed Visual Sins effect files for updates and merges new effects without touching your settings.";
    s_addonDef.Load        = AddonLoad;
    s_addonDef.Unload      = AddonUnload;
    s_addonDef.Flags       = AF_None;
    s_addonDef.Provider    = UP_None; // this addon isn't itself distributed via auto-update in this minimal version
    s_addonDef.UpdateLink  = nullptr;

    return &s_addonDef;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH: DisableThreadLibraryCalls(hModule); break;
        case DLL_PROCESS_DETACH: break;
    }
    return TRUE;
}