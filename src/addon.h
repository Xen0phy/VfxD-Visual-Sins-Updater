#pragma once
#include "Nexus.h"

// ---------------------------------------------------------------------------
// The addon's actual behavior, as opposed to entry.cpp's bare Nexus wiring:
// locating VfxDenoiser's folder, registering the options-panel UI, kicking
// off the initial (silent) update check, and tearing all of that down again
// on unload. entry.cpp's AddonLoad/AddonUnload -- the functions actually
// assigned to AddonDefinition_t::Load/Unload -- just forward into these.
// ---------------------------------------------------------------------------

// Called from entry.cpp's AddonLoad once Nexus has handed over the API
// pointer. Sets the ImGui context, locates <GW2>/addons/VfxDenoiser,
// registers the RT_OptionsRender callback, and starts the initial
// version-only update check (see StartUpdateCheck's alsoLoadDiff comment
// for why that check doesn't download anything by itself).
void Addon_Load(AddonAPI_t* aApi);

// Called from entry.cpp's AddonUnload. Deregisters the options-panel
// callback and unblocks any in-flight background HTTP call so it can exit
// promptly rather than the DLL unload having to wait one out. `aApi` may
// be null if Addon_Load was never reached (e.g. Nexus aborted early) --
// this is a no-op deregistration in that case, not a crash.
void Addon_Unload(AddonAPI_t* aApi);
