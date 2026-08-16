//################################################################################
// installed_tree_view.h
//--------------------------------------------------------------------------------
// RenderInstalledEffects(dir)   draws the "Installed Effects" tree section
//--------------------------------------------------------------------------------
// Split out of addon.cpp -- see installed_tree_view.cpp's own file header for
// what's file-local there and how it reaches the editing/store/overlay/search
// modules. RenderInstalledEffects is the only symbol this module exposes;
// OptionsRenderCallback (still in addon.cpp) is the only outside caller, and it
// only ever needs to draw the whole section.
//
// denoiserAddonDir is passed in, same convention as
// RenderReportSection/RenderBackupsSection/RenderLiveLogSection -- addon.cpp is
// still the sole owner of s_denoiserAddonDir.
//--------------------------------------------------------------------------------

#pragma once

#include <string>

void RenderInstalledEffects(const std::string& denoiserAddonDir);