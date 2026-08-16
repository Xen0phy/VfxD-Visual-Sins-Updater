//################################################################################
// entry.cpp
//--------------------------------------------------------------------------------
// AddonLoad(aApi)   Nexus load callback: locates VfxDenoiser, wires subsystems
// AddonUnload()     Nexus unload callback: tears the above back down
// GetAddonDef()     sole DLL export Nexus looks for
// DllMain           standard Windows DLL entry point
//--------------------------------------------------------------------------------
// Nexus wiring: the AddonLoad/AddonUnload functions assigned into AddonDefinition_t
// (including everything that happens on load/unload itself - locating VfxDenoiser,
// wiring up the game-state/live-log/update-check subsystems, registering the
// options-panel callback, and tearing all of that down again), plus GetAddonDef and
// DllMain. addon.cpp only owns what happens after load: the options-panel UI
// (OptionsRenderCallback) and the addon state (Addon_Init) that UI reads. Kept
// separate on purpose: this file is "what Nexus expects from an addon, and what
// happens at those two moments", addon.cpp is "what the addon looks like the rest
// of the time".
//--------------------------------------------------------------------------------

#include "addon.h"
#include "effect_db.h"
#include "game_state.h"
#include "github_update.h"
#include "imgui.h"
#include "live_log.h"
#include "Nexus.h"
#include "sql_update.h"
#include "version.h"
#include "webhook_report.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

static AddonDefinition_t s_addonDef{};
static AddonAPI_t*       s_api = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FlushEffectDbRenderCallback
//--------------------------------------------------------------------------------
// Registered under RT_PostRender because it has to run every real frame
// regardless of whether the options panel is open -- capture keeps running with
// the panel closed, and EffectDb_FlushPendingWrites is what commits whatever
// EffectDb_RecordEvent buffered up since the last frame (see that pair's comments
// in effect_db.h/.cpp). A no-op call (nothing pending) is a single boolean check,
// so registering it unconditionally here is cheap.
//--------------------------------------------------------------------------------
static void FlushEffectDbRenderCallback()
{
    EffectDb_FlushPendingWrites();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// AddonLoad / AddonUnload
//--------------------------------------------------------------------------------
// Nexus load/unload callbacks assigned into AddonDefinition_t (see GetAddonDef).
// The load-time update check is version numbers only, no downloading (see
// StartUpdateCheck's alsoLoadDiff parameter) - it just leaves a note in the
// options panel; the "Check now" button is what actually downloads and diffs
// anything. On unload, shutdown flags are flipped before the WinHTTP calls are
// cancelled, since a thread already past its call (e.g. mid file-write) can only
// be caught by that flag, not by cancellation. The thread-count poll that follows
// has a fixed deadline, since Nexus doesn't wait on unload to finish; this is a
// minimal safety net, not a guarantee.
//--------------------------------------------------------------------------------
void AddonLoad(AddonAPI_t* aApi)
{
    s_api = aApi;

    ImGui::SetCurrentContext((ImGuiContext*)aApi->ImguiContext);

    //_ Lets github_update.cpp's background-thread failures reach Nexus's log too.
    SetUpdaterLogger(aApi);
    //_ Same reasoning, for sql_update.cpp's own synchronous write path.
    SetSqlUpdateLogger(aApi);
    GameState_Init(aApi);   //. caches DataLink pointers, see game_state.h
    LiveLog_Init(aApi);     //. subscribes EV_VFXD_SINS_LOG
    //_ Only stores the log pointer; EffectDb_Open happens lazily on first enable.
    EffectDb_SetApi(aApi);

    //_ Checking fs::is_directory here is what makes `found` mean what it says.
    std::string denoiserAddonDir = aApi->Paths_GetAddonDirectory("VfxDenoiser");
    std::error_code ec;
    bool found = !denoiserAddonDir.empty() && fs::is_directory(denoiserAddonDir, ec) && !ec;

    //_ Values here are also referenced throughout the options-panel rendering.
    Addon_Init(aApi, denoiserAddonDir, found);

    aApi->GUI_Register(RT_OptionsRender, OptionsRenderCallback);
    aApi->GUI_Register(RT_PostRender, FlushEffectDbRenderCallback);

    if (found)
    {
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
    {
        s_api->GUI_Deregister(OptionsRenderCallback);
        s_api->GUI_Deregister(FlushEffectDbRenderCallback);
    }

    LiveLog_Shutdown(s_api); //. unsubscribes, stops capture if on
    //_ Finalizes statements, closes the sqlite connection -- same reasoning as above.
    EffectDb_Close();
    GameState_Shutdown();    //. clears cached DataLink pointers

    //_ Ordering vs. the cancels below explained in the block comment above.
    BeginUpdateShutdown();
    BeginReportShutdown();

    //_ Still worth it: closing handles is faster than waiting for the flag check.
    CancelInFlightUpdateRequest();
    CancelInFlightReportRequest();

    //_ Polls for zero threads instead of a fixed sleep (deadline explained above).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline &&
           (GetUpdateActiveThreadCount() > 0 || GetReportActiveThreadCount() > 0))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    //_ Logged as critical, matching every other unrecoverable failure in this addon.
    const int stillRunning = GetUpdateActiveThreadCount() + GetReportActiveThreadCount();
    if (stillRunning > 0 && s_api)
    {
        s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater",
                   ("Unload: " + std::to_string(stillRunning) +
                    " background thread(s) still running after the wait -- "
                    "may crash if the DLL is unloaded now.").c_str());
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetAddonDef
//--------------------------------------------------------------------------------
// Assembles the AddonDefinition_t Nexus reads on load, including the
// Load/Unload callbacks pointing at AddonLoad/AddonUnload above.
//--------------------------------------------------------------------------------
extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    s_addonDef.Signature   = 0x58565355; //. 'XVSU'
    s_addonDef.APIVersion  = NEXUS_API_VERSION;
    s_addonDef.Name        = "VfxD Visual Sins Updater";
    s_addonDef.Version     = { Maj, Min, Bld, Rev };
    s_addonDef.Author      = "Xenophy.2716";
    s_addonDef.Description = "Requires VfxDenoiser. An installer/updater/editor for the VfxD Visual Sins effect collection.";
    s_addonDef.Load        = AddonLoad;
    s_addonDef.Unload      = AddonUnload;
    s_addonDef.Flags       = AF_None;
    s_addonDef.Provider    = UP_GitHub;
    s_addonDef.UpdateLink  = "https://github.com/Xen0phy/VfxD-Visual-Sins-Updater";

    return &s_addonDef;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// DllMain
//--------------------------------------------------------------------------------
// Standard Windows DLL entry point.
//--------------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH: DisableThreadLibraryCalls(hModule); break;
        case DLL_PROCESS_DETACH: break;
    }
    return TRUE;
}