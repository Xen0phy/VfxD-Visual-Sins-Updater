//################################################################################
// ui_colors.h
//--------------------------------------------------------------------------------
// kNewColor         flags a brand-new effect not yet applied
// kReworkColor      flags an existing effect whose GUIDs would be refreshed
// kDuplicateColor   flags a duplicate-or-error condition
//--------------------------------------------------------------------------------
// Colors used to flag pending-update content overlaid onto the installed-
// effects tree (see BuildDiffOverlayTree in installed_tree_overlay.*). One
// color per kind of change, used consistently whether it's painting the
// leaf effect itself or an ancestor category header that contains one -
// rework wins the category tint over new if a category has both
// underneath, since a rework is the thing worth a second look. Chosen to
// read clearly against imgui's default dark theme without being confused
// for the existing error-red used elsewhere.
//
// Shared across addon.cpp, the installed-tree renderer/editor, and
// report_ui.cpp (error text) so nothing needing to flag new/reworked/
// duplicate-or-error state has to redeclare these constants locally.
//--------------------------------------------------------------------------------

#pragma once

#include "imgui.h"

inline const ImVec4 kNewColor       (0.40f, 0.85f, 0.40f, 1.0f); //. green
inline const ImVec4 kReworkColor    (0.95f, 0.60f, 0.20f, 1.0f); //. orange
inline const ImVec4 kDuplicateColor (0.90f, 0.25f, 0.25f, 1.0f); //. red
