#pragma once
#include "core/merge.h" // nlohmann::ordered_json
#include <string>

// ---------------------------------------------------------------------------
// Pure matching/closing functions over a JSON category plus a query string,
// split out of addon.cpp. No shared
// state, no ImGui calls except SilentlyCloseSubtree/SilentlyCloseChildren's
// direct writes into ImGui's per-ID open/closed storage (they draw nothing).
//
// The search *state* itself -- s_treeSearchBuf, s_treeSearchQueryLower,
// s_treeSearchQueryChanged, kMinTreeSearchLength -- lives in
// installed_tree_view.cpp, read/written by RenderCategoryTree/
// RenderInstalledEffects's search-box UI. Every function here already
// takes the query string as a parameter, so this is a real "no shared
// state" move for the functions themselves, just not for every static
// that used to sit next to them in the original file.
// ---------------------------------------------------------------------------

// Case-insensitive substring test. An empty `needleLower` always matches (an
// empty search box means "no filter"), so callers don't need their own
// early-out for that case.
bool ContainsCI(const std::string& haystack, const std::string& needleLower);

// True if `effect`'s own visible name -- what's shown right on its (possibly
// collapsed) row -- contains `queryLower`. A name match never needs the
// effect's own node opened: the match is already on-screen.
bool EffectNameMatches(const nlohmann::ordered_json& effect, const std::string& queryLower);

// True if `effect` matches `queryLower` only through content that's hidden
// until its own node is opened -- its description or any one of its GUIDs.
// Unlike a name match, this DOES need the node forced open, or the reason it
// matched never becomes visible.
bool EffectHiddenContentMatches(const nlohmann::ordered_json& effect, const std::string& queryLower);

// True if `effect` matches `queryLower` at all -- by name or by hidden
// content. Used for the filtering decision (show this effect or skip it),
// which doesn't care which part of it matched, only whether it did.
bool EffectMatchesSearch(const nlohmann::ordered_json& effect, const std::string& queryLower);

// True if `category`'s own visible name -- shown right on its (possibly
// collapsed) row -- contains `queryLower`. Same reasoning as
// EffectNameMatches: a name match doesn't by itself need this category's own
// node opened.
bool CategoryNameMatches(const nlohmann::ordered_json& category, const std::string& queryLower);

// True if `category`'s own description -- only shown once this category's
// node is open -- contains `queryLower`. Unlike a name match, this DOES need
// the node forced open to be seen at all.
bool CategoryDescriptionMatches(const nlohmann::ordered_json& category, const std::string& queryLower);

// True if `category` (its own name/description), any effect directly inside
// it, or any nested subcategory (recursively) matches `queryLower`. This is
// the "does this subtree have anything worth showing at all" check
// RenderCategoryTree uses to decide whether to draw a category during a
// search rather than skip it outright.
bool CategorySubtreeMatchesSearch(const nlohmann::ordered_json& category, const std::string& queryLower);

// True if something *below* `category` (a direct effect, or a nested
// subcategory either by its own name/description or transitively via this
// same check) matches `queryLower`. Deliberately excludes `category`'s own
// name/description -- this is only about whether opening THIS category is
// necessary to reveal a match further down, not about whether this category
// is itself the match. That distinction is exactly what keeps "Warrior"
// itself collapsed when a search for "Warrior" only matched its own name,
// while still forcing "Classes" (Warrior's parent) open so Warrior's row
// isn't hidden.
bool CategoryHasDescendantMatch(const nlohmann::ordered_json& category, const std::string& queryLower);

// A category's, or an effect's, forced-open state (see the force-open
// comments in RenderCategoryTree) only ever gets set on the one frame the
// query changes, and only for whatever RenderCategoryTree actually visits
// that frame. But a collapsed (or search-hidden) category's own children are
// never visited at all -- the code that would recurse into them, or force
// their own open state, lives inside "if (categoryOpen)" further down, which
// simply doesn't run when this category isn't open. So a category that gets
// force-CLOSED (or hidden by search) on a query-change frame leaves whatever
// is underneath it exactly as it was -- including any grandchildren that got
// force-opened by an *earlier* query and are now invisible, but still "open"
// as far as ImGui's own per-ID memory is concerned. The next time that
// ancestor is opened again -- by a new search, or by hand -- those
// descendants reappear already expanded, looking like ImGui just "forgot" to
// close them.
//
// These two functions fix that by walking the JSON tree directly (not
// through TreeNode/TreePop at all, so nothing is actually drawn) and writing
// "closed" straight into ImGui's per-ID open/closed storage for every node
// underneath, using the exact same ID scheme the real render pass uses
// (PushID(index) for siblings, GetID(name) for a category, GetID("effect")
// for an effect -- mirroring what TreeNode(name.c_str()) and
// TreeNode("effect", ...) compute internally, and what TreeNode auto-pushes
// onto the ID stack for its children when it opens). Only worth calling on
// the frame the query actually changed -- see addon.cpp's
// s_treeSearchQueryChanged comment for why redoing this every frame
// regardless would reintroduce the exact stall that was already fixed once
// here.
void SilentlyCloseSubtree(const nlohmann::ordered_json& category);
void SilentlyCloseChildren(const nlohmann::ordered_json& category);
