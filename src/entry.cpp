// entry.cpp
//
// Bare Nexus wiring, nothing else: GetAddonDef (the sole DLL export Nexus
// looks for), the AddonLoad/AddonUnload functions actually assigned into
// AddonDefinition_t, and the DllMain every Windows DLL needs. What the
// addon actually *does* on load/unload -- locating VfxDenoiser, the
// options-panel UI, the initial update check -- lives in addon.cpp's
// Addon_Load/Addon_Unload; the functions here just forward into those.
// Kept separate on purpose: this file is "what Nexus expects from an
// addon", addon.cpp is "what this addon does".
#include "Nexus.h"
#include "addon.h"
#include "version.h"

static AddonDefinition_t s_addonDef{};
static AddonAPI_t*       s_api = nullptr;

void AddonLoad(AddonAPI_t* aApi)
{
    s_api = aApi;
    Addon_Load(aApi);
}

void AddonUnload()
{
    Addon_Unload(s_api);
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
