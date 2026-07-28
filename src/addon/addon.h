//################################################################################
// addon.h
//--------------------------------------------------------------------------------
// Addon_Init(aApi, denoiserAddonDir, denoiserFound)   stores addon state
// OptionsRenderCallback()                             draws the options panel
//--------------------------------------------------------------------------------
// The addon's actual behavior, as opposed to entry.cpp's bare Nexus wiring:
// the options-panel UI and the addon's own state (which folder it's pointed
// at, what's currently cached for display) that the UI reads. The
// AddonLoad/AddonUnload functions assigned to AddonDefinition_t::Load/Unload
// live in entry.cpp, not here; this header just exposes what they hand off
// to this file.
//--------------------------------------------------------------------------------

#pragma once

#include "Nexus.h"

#include <string>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Addon_Init
//--------------------------------------------------------------------------------
// Called from entry.cpp's AddonLoad once it has located
// <GW2>/addons/VfxDenoiser (or confirmed it's missing). Stores aApi and that
// result for the rest of this file to use.
//--------------------------------------------------------------------------------
void Addon_Init(AddonAPI_t* aApi, const std::string& denoiserAddonDir, bool denoiserFound);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// OptionsRenderCallback
//--------------------------------------------------------------------------------
// The options-panel UI, registered/deregistered by entry.cpp's
// AddonLoad/AddonUnload. Only draws while VfxDenoiser was found by
// Addon_Init above.
//--------------------------------------------------------------------------------
void OptionsRenderCallback();
