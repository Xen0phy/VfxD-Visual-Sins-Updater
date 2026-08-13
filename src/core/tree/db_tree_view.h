//################################################################################
// db_tree_view.h
//--------------------------------------------------------------------------------
// BuildDbTree()   builds a fresh, self-contained tree straight from
//                 effect_db, in the same shape RenderCategoryTree already
//                 expects -- no installed JSON is read or needed.
//--------------------------------------------------------------------------------
// Pure data-transformation, same spirit as installed_tree_overlay.h: no
// shared state, no ImGui calls, takes its input as a parameter and
// returns a fresh in-memory tree. Unlike installed_tree_overlay.h's
// Build*OverlayTree functions, this one has no "installed" JSON to start
// from -- see EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md's "Building the DB
// tab": the DB tab is independent of any sin file, not an overlay on one.
//--------------------------------------------------------------------------------

#pragma once

#include "effect_db.h"        //. EffectDbEffect
#include "nlohmann_json.hpp"  //. nlohmann::ordered_json

#include <string>
#include <unordered_set>
#include <vector>

//_ Sentinel sinName the DB tab is rendered under -- never a real sin
// file, so it can never collide with an actual installed sin name.
// Compared against directly (not via a lookup) in RenderCategoryTree's
// category-level drop target, same shape as the old "sinName == \"Greed\""
// db-only special case it sits beside.
inline constexpr const char* kDbTabSinName = "__vfxd_db_tab__";

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildDbTree
//--------------------------------------------------------------------------------
// Groups `dbEffects` (as returned by EffectDb_GetAllEffects) by effect_id
// -- one tree node per effect, every guid sharing that effect_id folded
// into a single "guids" array on the node, not one node per guid. Builds
// category nodes by splitting each group's category_path on \x1f, same
// materialize-as-needed shape as installed_tree_overlay.cpp's own
// FindOrCreateDiffCategory (categories tagged "__vfxd_virtual" so
// RenderCategoryTree's existing gate suppresses every JSON-only editing
// affordance on them for free -- see the handoff's "Building the DB tab").
//
// Every effect node is tagged "__vfxd_db_effect": true and carries its
// full "guids" array, "effect_id", and "in_json" -- deliberately NOT
// reusing the old "__vfxd_db_only"/"dbOnlyGuid" shape (single-guid only,
// see the handoff for why that would silently drop data here).
//
// "in_json" is computed here, live, against `installedGuids` -- the
// caller's snapshot of every guid across every currently-loaded sin file
// (see installed_tree_store.h's CollectInstalledGuids). Deliberately NOT
// read from EffectDbEffect::in_json (a column this addon only ever seeds
// externally and never refreshes, see effect_db.h) -- that column would
// go stale the moment a guid is added to or removed from JSON without a
// matching effect_db write, which is the common case. A node counts as
// "also installed" if ANY of its guids are, not all -- a presence badge,
// not a completeness claim, same as before.
//
// An effect with an empty category_path (never placed) lands in a root
// "Uncategorized" bucket, same idea as the old overlay's "Unrecognized
// (for science)" bucket but framed for a tab where EVERY row is
// necessarily db-known, not just the not-yet-JSON ones.
//
// Sorted alphabetically by name at every level, categories before
// effects -- deterministic, no drag-to-reorder (effect_meta has no
// ordering column, see the handoff's "Settled, not just flagged"
// section).
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildDbTree(const std::vector<EffectDbEffect>& dbEffects,
                                    const std::unordered_set<std::string>& installedGuids);