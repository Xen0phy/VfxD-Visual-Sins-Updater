//##############################################################################
// installed_tree_view.h
//------------------------------------------------------------------------------
// RenderInstalledEffects(dir)   draws the "Installed Effects" tree section
//------------------------------------------------------------------------------
// Split out of addon.cpp. Covers RenderCategoryTree (the 700+-line recursive
// tree renderer), RenderInstalledEffects (the section wrapper -- loads the
// tree, builds the per-sin overlay cache, calls RenderCategoryTree), and
// four small leaf renderers (GuidList/GuidDiff/JsonValue/Behavior) that only
// RenderCategoryTree calls. Everything except RenderInstalledEffects itself
// is file-local, in installed_tree_view.cpp's unnamed namespace --
// OptionsRenderCallback (still in addon.cpp) is the only outside caller, and
// it only ever needs to draw the whole section.
//
// Reaches the editing subsystem, the store, the overlay builders, and the
// search helpers only through their own accessor APIs
// (installed_tree_edit.h / installed_tree_store.h / installed_tree_overlay.h
// / installed_tree_search.h) -- never through another module's statics.
//
// denoiserAddonDir is passed in rather than read from a global, same
// convention as RenderReportSection/RenderBackupsSection/RenderLiveLogSection
// -- addon.cpp is still the sole owner of s_denoiserAddonDir.
//------------------------------------------------------------------------------

#pragma once

#include <string>

void RenderInstalledEffects(const std::string& denoiserAddonDir);