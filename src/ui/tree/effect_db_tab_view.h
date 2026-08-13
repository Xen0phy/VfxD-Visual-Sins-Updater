//################################################################################
// effect_db_tab_view.h
//--------------------------------------------------------------------------------
// RenderEffectDbTab()   renders the "for science" database tab
//--------------------------------------------------------------------------------
// The DB tab -- a browsable view of SQLite's own effects/effect_meta,
// entirely independent of any installed sin's JSON (see the DB-tab
// source-of-truth handoff doc). Reuses BuildEffectDbTree (effect_db_tree.h)
// for its data and installed_tree_search.h's match-cache machinery for
// its search box, same shape as the JSON tabs' own search, but is
// otherwise its own smaller renderer -- no reorder, no create-empty-
// category, no full effect editor (behaviors/description aren't editable
// here this phase), just browse, rename, and drag-to-category. See
// installed_tree_edit.h's BeginDbTabRename/QueueDbTabCategoryPlacement
// group for the edit-apply half.
//--------------------------------------------------------------------------------

#pragma once

#include <string>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderEffectDbTab
//--------------------------------------------------------------------------------
// Call once per frame, from wherever the installed-effects section wants
// this tab drawn (see RenderInstalledEffects in installed_tree_view.cpp).
// Independent of whether any sin file is currently loaded -- the DB tab
// has its own data source and renders (possibly empty) regardless.
//
// `denoiserAddonDir` is used to open the SQLite file directly
// (EffectDb_EnsureOpenForBrowsing) if it isn't open yet -- this tab does
// NOT require "for science" capture to have ever been toggled on, and
// does NOT require VfxD_Greed.json to exist (unlike EffectDb_SetEnabled's
// gate) -- an externally-seeded db is a completely valid, capture-free
// way for this tab to have data. See effect_db.h.
//
// Refreshes effect_db's `in_json` flags (EffectDb_RefreshInJsonFlags)
// whenever GetInstalledTreeGeneration() has moved since the last call,
// and rebuilds its own tree cache whenever EffectDb_GetGeneration() has
// moved since the last call -- both cheap generation-counter compares,
// not per-frame rebuilds.
//--------------------------------------------------------------------------------
void RenderEffectDbTab(const std::string& denoiserAddonDir);
