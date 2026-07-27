#pragma once
#include "Nexus.h"
#include <string>

// ---------------------------------------------------------------------------
// The addon's actual behavior, as opposed to entry.cpp's bare Nexus wiring:
// the options-panel UI (registered/deregistered by entry.cpp's
// AddonLoad/AddonUnload) and the addon's own state (which folder it's
// pointed at, what's currently cached for display) that UI reads. The
// AddonLoad/AddonUnload functions themselves -- the ones actually assigned
// to AddonDefinition_t::Load/Unload -- live in entry.cpp, not here; this
// header just exposes what they need to hand off to this file.
// ---------------------------------------------------------------------------

// Called from entry.cpp's AddonLoad once it has located
// <GW2>/addons/VfxDenoiser (or confirmed it's missing). Stores aApi and
// that result for the rest of this file to use -- both are referenced
// throughout the options-panel rendering below, not just at load time.
void Addon_Init(AddonAPI_t* aApi, const std::string& denoiserAddonDir, bool denoiserFound);

// The options-panel UI, registered with Nexus's GUI_Register in
// entry.cpp's AddonLoad and deregistered with GUI_Deregister in
// AddonUnload. Only draws anything while VfxDenoiser was found by
// Addon_Init above.
void OptionsRenderCallback();