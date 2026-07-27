// ui_colors.h
//
// Colors used to flag pending-update content overlaid onto the installed-
// effects tree (see BuildDiffOverlayTree in installed_tree_overlay.*): a
// brand-new effect not yet applied, an existing effect whose GUIDs would be
// refreshed, and any category that contains one of those somewhere
// underneath it. One color per kind of change, used consistently whether
// it's painting the leaf effect itself or an ancestor category header that
// contains one -- green for "new", orange for "reworked" (a rework wins the
// category tint over a new effect if a category has both underneath, since
// a rework is the thing worth a second look). Chosen to read clearly
// against imgui's default dark theme without being confused for the
// existing error-red used elsewhere.
//
// Shared across addon.cpp, the installed-tree renderer/editor, and
// report_ui.cpp (error text) -- anything that needs to flag new/reworked/
// duplicate-or-error state can just include this rather than redeclaring
// the constants locally.
#pragma once

#include "imgui.h"

inline const ImVec4 kNewColor       (0.40f, 0.85f, 0.40f, 1.0f); // green
inline const ImVec4 kReworkColor    (0.95f, 0.60f, 0.20f, 1.0f); // orange
inline const ImVec4 kDuplicateColor (0.90f, 0.25f, 0.25f, 1.0f); // red
