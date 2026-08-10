//################################################################################
// installed_tree_search.h
//--------------------------------------------------------------------------------
// ContainsCI()                    case-insensitive substring test
// Effect*Matches()                does one effect match the query
// Category*Matches()              does one category (not its children) match
// CategorySubtreeMatchesSearch()  does a category or anything under it match
// CategoryHasDescendantMatch()    does anything BELOW a category match
// SilentlyCloseSubtree/Children() force-close ImGui's open-state for a subtree
//--------------------------------------------------------------------------------
// Pure matching/closing functions over a JSON category plus a query
// string, split out of addon.cpp. No shared state, no ImGui calls except
// SilentlyCloseSubtree/SilentlyCloseChildren's direct writes into ImGui's
// per-ID open/closed storage (they draw nothing).
//
// The search *state* itself -- s_treeSearchBuf, s_treeSearchQueryLower,
// s_treeSearchQueryChanged, kMinTreeSearchLength -- lives in
// installed_tree_view.cpp, read/written by RenderCategoryTree/
// RenderInstalledEffects's search-box UI. Every function here already
// takes the query string as a parameter, so this is a real "no shared
// state" move for the functions themselves, just not for every static
// that used to sit next to them in the original file.
//--------------------------------------------------------------------------------

#pragma once

#include "merge.h" //. nlohmann::ordered_json

#include <string>
#include <unordered_map>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ContainsCI
//--------------------------------------------------------------------------------
// Case-insensitive substring test. An empty `needleLower` always matches
// (an empty search box means "no filter"), so callers don't need their own
// early-out for that case.
//--------------------------------------------------------------------------------
bool ContainsCI(const std::string& haystack, const std::string& needleLower);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectNameMatches / EffectHiddenContentMatches / EffectMatchesSearch
//--------------------------------------------------------------------------------
// True if `effect` matches `queryLower` by name (visible on its row even
// collapsed, so a name match never needs the node opened) or by hidden
// content -- description or any GUID, which DOES need the node forced
// open, or the reason it matched never becomes visible. EffectMatchesSearch
// is the OR of both, for the filtering decision (show or skip), which
// doesn't care which part matched, only whether it did.
//--------------------------------------------------------------------------------
bool EffectNameMatches(const nlohmann::ordered_json& effect, const std::string& queryLower);
bool EffectHiddenContentMatches(const nlohmann::ordered_json& effect, const std::string& queryLower);
bool EffectMatchesSearch(const nlohmann::ordered_json& effect, const std::string& queryLower);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CategoryNameMatches / CategoryDescriptionMatches
//--------------------------------------------------------------------------------
// True if `category`'s own name / description (not its children) contains
// `queryLower`. Same split as the Effect matchers above: name is visible
// on the (possibly collapsed) row so doesn't need the node opened;
// description only shows once the node is open, so a description match
// DOES need it forced open to be seen at all.
//--------------------------------------------------------------------------------
bool CategoryNameMatches(const nlohmann::ordered_json& category, const std::string& queryLower);
bool CategoryDescriptionMatches(const nlohmann::ordered_json& category, const std::string& queryLower);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CategorySubtreeMatchesSearch
//--------------------------------------------------------------------------------
// True if `category` (its own name/description), any effect directly
// inside it, or any nested subcategory (recursively) matches `queryLower`.
// The "does this subtree have anything worth showing at all" check
// RenderCategoryTree uses to decide whether to draw a category during a
// search rather than skip it outright.
//--------------------------------------------------------------------------------
bool CategorySubtreeMatchesSearch(const nlohmann::ordered_json& category, const std::string& queryLower);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CategoryHasDescendantMatch
//--------------------------------------------------------------------------------
// True if something *below* `category` (a direct effect, or a nested
// subcategory by its own name/description or transitively) matches
// `queryLower`. Deliberately excludes `category`'s own name/description --
// this is only about whether opening THIS category is necessary to reveal
// a match further down. That distinction keeps "Warrior" itself collapsed
// when a search only matched its own name, while still forcing "Classes"
// (Warrior's parent) open so Warrior's row isn't hidden.
//--------------------------------------------------------------------------------
bool CategoryHasDescendantMatch(const nlohmann::ordered_json& category, const std::string& queryLower);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CategoryMatchCache / EffectMatchCache / BuildCategoryMatchCache / Cached*
//--------------------------------------------------------------------------------
// CategorySubtreeMatchesSearch/CategoryHasDescendantMatch above are each
// independently recursive, and effect matching means lowercasing/scanning
// name, description, and every GUID -- calling any of these directly per
// node/effect, every frame a search stays active, costs O(size x depth)
// for categories, or re-scans every effect needlessly (the query hasn't
// changed between frames).
//
// These caches hold the same answers per node, built in one bottom-up
// pass (each node/effect visited once) so top-down lookups are O(1).
// Keyed by each json node's own address -- safe to rebuild fresh every
// frame, so nothing here is cached *across* frames.
//--------------------------------------------------------------------------------
struct CategoryMatchResult
{
    bool subtreeMatches      = false;   //. CategorySubtreeMatchesSearch's answer
    bool hasDescendantMatch  = false;   //. CategoryHasDescendantMatch's answer
};

//_ Same idea as CategoryMatchResult, but for a leaf effect node (see the
// group comment above).
struct EffectMatchResult
{
    bool matches       = false;   //. EffectMatchesSearch's answer (name/hidden)
    bool hiddenMatches = false;   //. EffectHiddenContentMatches's answer (description/GUIDs only)
};

using CategoryMatchCache = std::unordered_map<const void*, CategoryMatchResult>;
using EffectMatchCache   = std::unordered_map<const void*, EffectMatchResult>;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildCategoryMatchCache
//--------------------------------------------------------------------------------
// Populates `categoryCache`/`effectCache` for `category`, every effect
// directly inside it, and everything beneath it, in one bottom-up pass.
// Rebuild fresh whenever the query or tree contents change -- cheap
// enough to do every frame while a search is active.
//--------------------------------------------------------------------------------
void BuildCategoryMatchCache(const nlohmann::ordered_json& category,
                              const std::string& queryLower,
                              CategoryMatchCache& categoryCache,
                              EffectMatchCache& effectCache);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CachedSubtreeMatches / CachedHasDescendantMatch / CachedEffectMatches /
// CachedEffectHiddenMatches
//--------------------------------------------------------------------------------
// O(1) reads from caches already populated by BuildCategoryMatchCache for
// this exact node. Falls back to the direct (uncached, but always
// correct) functions above if the node isn't present, so a stale/partial
// cache never produces a wrong answer -- only ever a slower one.
//--------------------------------------------------------------------------------
bool CachedSubtreeMatches(const nlohmann::ordered_json& category, const std::string& queryLower,
                           const CategoryMatchCache& cache);
bool CachedHasDescendantMatch(const nlohmann::ordered_json& category, const std::string& queryLower,
                               const CategoryMatchCache& cache);
bool CachedEffectMatches(const nlohmann::ordered_json& effect, const std::string& queryLower,
                          const EffectMatchCache& cache);
bool CachedEffectHiddenMatches(const nlohmann::ordered_json& effect, const std::string& queryLower,
                                const EffectMatchCache& cache);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SilentlyCloseSubtree / SilentlyCloseChildren
//--------------------------------------------------------------------------------
// A category/effect's forced-open state only gets set on the frame the
// query changes, for whatever RenderCategoryTree actually visits that
// frame -- a collapsed or search-hidden category's children are never
// visited, so force-CLOSING it leaves what's underneath untouched. A
// grandchild force-opened by an earlier query stays "open" in ImGui's
// per-ID memory though invisible, reappearing already expanded the next
// time its ancestor opens.
//
// These two functions fix that by walking the JSON tree directly (nothing
// drawn) and writing "closed" into ImGui's per-ID storage for every node
// underneath, using the render pass's own ID scheme (PushID(index) for
// siblings, GetID(name)/GetID("effect") for a category/effect). Only
// worth calling on the frame the query changed -- see
// installed_tree_view.cpp's s_treeSearchQueryChanged comment for why.
//--------------------------------------------------------------------------------
void SilentlyCloseSubtree(const nlohmann::ordered_json& category);
void SilentlyCloseChildren(const nlohmann::ordered_json& category);