//################################################################################
// entry.cpp
//--------------------------------------------------------------------------------
// AddonLoad(aApi)   Nexus load callback: locates VfxDenoiser, wires subsystems
// AddonUnload()     Nexus unload callback: tears the above back down
// GetAddonDef()     sole DLL export Nexus looks for
// DllMain           standard Windows DLL entry point
//--------------------------------------------------------------------------------
// Nexus wiring: the AddonLoad/AddonUnload functions assigned into
// AddonDefinition_t (including everything that happens on load/unload
// itself - locating VfxDenoiser, wiring up the game-state/live-log/
// update-check subsystems, registering the options-panel callback, and
// tearing all of that down again), plus GetAddonDef and DllMain. addon.cpp
// only owns what happens after load: the options-panel UI
// (OptionsRenderCallback) and the addon state (Addon_Init) that UI reads.
// Kept separate on purpose: this file is "what Nexus expects from an
// addon, and what happens at those two moments", addon.cpp is "what the
// addon looks like the rest of the time".
//--------------------------------------------------------------------------------

#include "addon.h"
#include "game_state.h"
#include "github_update.h"
#include "imgui.h"
#include "live_log.h"
#include "Nexus.h"
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
// AddonLoad / AddonUnload
//--------------------------------------------------------------------------------
// Nexus load/unload callbacks assigned into AddonDefinition_t (see
// GetAddonDef). The load-time update check is version numbers only, no
// downloading (see StartUpdateCheck's alsoLoadDiff parameter) - it just
// leaves a note in the options panel; the "Check now" button is what
// actually downloads and diffs anything. On unload, background WinHTTP
// calls are cancelled and given a brief best-effort window to exit before
// the DLL may get unloaded out from under them; this is a minimal safety
// net, not a guarantee.
//--------------------------------------------------------------------------------
void AddonLoad(AddonAPI_t* aApi)
{
    s_api = aApi;

    ImGui::SetCurrentContext((ImGuiContext*)aApi->ImguiContext);

    //_ Lets github_update.cpp's background-thread failures reach Nexus's
    //_ log too.
    SetUpdaterLogger(aApi);
    GameState_Init(aApi);   //. caches DataLink pointers, see game_state.h
    LiveLog_Init(aApi);     //. subscribes EV_VFXD_SINS_LOG

    //_ Paths_GetAddonDirectory only constructs the path string; checking
    //_ fs::is_directory here is what makes `found` mean what it says.
    std::string denoiserAddonDir = aApi->Paths_GetAddonDirectory("VfxDenoiser");
    std::error_code ec;
    bool found = !denoiserAddonDir.empty() && fs::is_directory(denoiserAddonDir, ec) && !ec;

    //_ Hands addon.cpp the api pointer and this load-time result; both are
    //_ referenced throughout the options-panel rendering, not just here.
    Addon_Init(aApi, denoiserAddonDir, found);

    aApi->GUI_Register(RT_OptionsRender, OptionsRenderCallback);

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
        s_api->GUI_Deregister(OptionsRenderCallback);

    LiveLog_Shutdown(s_api); //. unsubscribes, stops capture if on
    GameState_Shutdown();    //. clears cached DataLink pointers

    CancelInFlightUpdateRequest();
    CancelInFlightReportRequest();

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetAddonDef
//--------------------------------------------------------------------------------
// Assembles the AddonDefinition_t Nexus reads on load, including the
// Load/Unload callbacks pointing at AddonLoad/AddonUnload above.
//--------------------------------------------------------------------------------
extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    s_addonDef.Signature   = 0x56465344; //. 'VFSD'
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
