#pragma once

// ---------------------------------------------------------------------------
// The "Installed Effects" tree view -- RenderCategoryTree (the 700+-line
// recursive renderer), RenderInstalledEffects (the section wrapper: loads
// the tree, builds the per-sin overlay cache, calls RenderCategoryTree), and
// the four small leaf renderers (RenderGuidList/RenderGuidDiff/
// RenderJsonValue/RenderBehavior) that only RenderCategoryTree calls. Split
// out of addon.cpp.
//
// Everything except RenderInstalledEffects itself is file-local (an unnamed
// namespace in installed_tree_view.cpp, same convention as every other
// extracted module in this project) -- OptionsRenderCallback (still in
// addon.cpp) is the only outside caller, and it only ever needs to draw the
// whole section.
//
// This module reaches the editing subsystem, the store, the overlay
// builders, and the search helpers only through their own accessor APIs
// (installed_tree_edit.h / installed_tree_store.h / installed_tree_overlay.h
// / installed_tree_search.h) -- nothing here touches another module's
// statics directly.
//
// denoiserAddonDir: passed in rather than read from a global, same
// convention as RenderReportSection/RenderBackupsSection/RenderLiveLogSection
// -- addon.cpp is still the sole owner of s_denoiserAddonDir.
// ---------------------------------------------------------------------------

#include <string>

void RenderInstalledEffects(const std::string& denoiserAddonDir);
