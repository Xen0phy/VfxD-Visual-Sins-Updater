//################################################################################
// effect_db_tab_view.cpp
//--------------------------------------------------------------------------------
// See effect_db_tab_view.h for the module contract.
//--------------------------------------------------------------------------------

#include "effect_db_tab_view.h"

#include "effect_db.h"
#include "effect_db_tree.h"
#include "imgui.h"
#include "installed_tree_edit.h"
#include "installed_tree_search.h"
#include "installed_tree_store.h"
#include "ui_colors.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

//_ The DB tab's own cached tree -- same "don't rebuild every frame"
// reasoning as installed_tree_view.cpp's s_overlayCache, but there's only
// ever one of these (not per-sin), so a single generation-gated static
// is enough; no map needed.
nlohmann::ordered_json s_dbTree;
int s_dbTreeEffectDbGeneration = -1;   //. EffectDb_GetGeneration() as of the last rebuild
int s_dbTreeInstalledGeneration = -1;  //. GetInstalledTreeGeneration() as of the last in_json refresh

//_ The DB tab's own search state -- deliberately separate from
// installed_tree_view.cpp's s_treeSearchBuf/etc: the two tabs' searches
// are independent by design, so clearing one never affects the other.
char        s_dbSearchBuf[128] = {};
std::string s_dbSearchQueryLower;
bool        s_dbSearchQueryChanged = false;

const size_t kMinDbSearchLength = 2; //. same threshold as the JSON tabs' own search

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RefreshInJsonFlagsIfNeeded
//--------------------------------------------------------------------------------
// Recomputes effect_db's `in_json` column against whatever's currently
// loaded whenever GetInstalledTreeGeneration() has moved since the last
// call -- a cheap generation-counter compare gates this to "once per
// reload", not per frame. Harmless (and a fast no-op) to call even if the
// installed tree has never been loaded at all: GetInstalledJson() is
// simply empty then, so every guid's in_json goes to false.
//--------------------------------------------------------------------------------
void RefreshInJsonFlagsIfNeeded()
{
    int installedGenNow = GetInstalledTreeGeneration();
    if (installedGenNow == s_dbTreeInstalledGeneration)
        return;

    std::unordered_set<std::string> guidsInJson;
    for (const auto& [sinName, file] : GetInstalledJson())
    {
        if (!file.contains("categories") || !file["categories"].is_array())
            continue;

        //_ Flat guid collection, depth-agnostic -- a small local walk
        // rather than pulling in RenderCategoryTree's own recursive
        // machinery, since all this needs is every guid, not the tree shape.
        std::vector<const nlohmann::ordered_json*> stack;
        for (const auto& cat : file["categories"])
            stack.push_back(&cat);

        while (!stack.empty())
        {
            const nlohmann::ordered_json* cat = stack.back();
            stack.pop_back();

            if (cat->contains("effects") && (*cat)["effects"].is_array())
                for (const auto& eff : (*cat)["effects"])
                    if (eff.contains("guids") && eff["guids"].is_array())
                        for (const auto& g : eff["guids"])
                            if (g.is_string())
                                guidsInJson.insert(g.get<std::string>());

            if (cat->contains("categories") && (*cat)["categories"].is_array())
                for (const auto& sub : (*cat)["categories"])
                    stack.push_back(&sub);
        }
    }

    EffectDb_RefreshInJsonFlags(guidsInJson);
    s_dbTreeInstalledGeneration = installedGenNow;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RebuildDbTreeIfNeeded
//--------------------------------------------------------------------------------
// Rebuilds s_dbTree whenever EffectDb_GetGeneration() has moved since the
// last build, or on the very first call. Always called after
// RefreshInJsonFlagsIfNeeded, so a fresh in_json refresh is reflected in
// the same rebuild rather than needing a second generation bump to show up.
//--------------------------------------------------------------------------------
void RebuildDbTreeIfNeeded()
{
    int genNow = EffectDb_GetGeneration();
    if (genNow == s_dbTreeEffectDbGeneration && s_dbTreeEffectDbGeneration != -1)
        return;

    s_dbTree = BuildEffectDbTree();
    s_dbTreeEffectDbGeneration = genNow;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderDbCategory
//--------------------------------------------------------------------------------
// The DB tab's own recursive category renderer -- deliberately a smaller,
// separate function from installed_tree_view.cpp's RenderCategoryTree
// (see effect_db_tab_view.h's own doc on why), though it reuses the same
// search match-cache machinery (installed_tree_search.h) and the same
// general "push name, check subtree match, TreeNode, recurse, pop name"
// shape.
//
// No reorder (no pathSoFar/index identity at all -- everything here is
// keyed by effect_id or by name path, never by position), no create-
// empty-category, no delete, no full effect editor. Just: browse,
// right-click an effect to rename it, drag an effect onto a category to
// place it there.
//--------------------------------------------------------------------------------
void RenderDbCategory(const nlohmann::ordered_json& category, std::vector<std::string>& namePathSoFar,
                       const CategoryMatchCache& matchCache, const EffectMatchCache& effectMatchCache)
{
    std::string name = category.value("name", std::string("(unnamed category)"));
    namePathSoFar.push_back(name);

    bool searchActive = !s_dbSearchQueryLower.empty();

    //_ An unmatched subtree isn't drawn at all (not even collapsed) --
    // same reasoning as RenderCategoryTree's own early-out. Cancel a
    // rename scoped under here first, so nothing keeps running invisibly.
    if (searchActive && !CachedSubtreeMatches(category, s_dbSearchQueryLower, matchCache))
    {
        if (s_dbSearchQueryChanged)
            SilentlyCloseSubtree(category);

        namePathSoFar.pop_back();
        return;
    }

    //_ Forced open only if collapsing would hide a match not already
    // visible on the row itself -- same gating as the JSON tabs' own search.
    if (s_dbSearchQueryChanged)
    {
        bool categoryNeedsForceOpen = searchActive &&
            CachedHasDescendantMatch(category, s_dbSearchQueryLower, matchCache);
        ImGui::SetNextItemOpen(categoryNeedsForceOpen, ImGuiCond_Always);
    }

    bool categoryOpen = ImGui::TreeNode(name.c_str());

    //_ Drop target for a dragged effect -- places it at this category,
    // effect_id-keyed (see QueueDbTabCategoryPlacement).
    if (ImGui::BeginDragDropTarget())
    {
        if (ImGui::AcceptDragDropPayload("VFXD_DBTAB_EFFECT"))
        {
            const DbTabEffectDragPayload& payload = GetDbTabEffectDragPayload();
            QueueDbTabCategoryPlacement(payload.effectId, namePathSoFar);
        }
        ImGui::EndDragDropTarget();
    }

    if (!categoryOpen && s_dbSearchQueryChanged)
        SilentlyCloseChildren(category);

    if (categoryOpen)
    {
        if (category.contains("categories") && category["categories"].is_array())
        {
            int catIndex = 0;
            for (const auto& sub : category["categories"])
            {
                //_ Every sibling category needs a distinct ID -- two
                // categories with the same name at the same level would
                // otherwise collide (ImGui's TreeNode(label) hashes the
                // label text itself when no separate str_id is given).
                // Same reasoning as the effect loop's PushID(effectId)
                // below, just index-keyed since categories have no
                // stable unique id of their own.
                ImGui::PushID(catIndex);
                RenderDbCategory(sub, namePathSoFar, matchCache, effectMatchCache);
                ImGui::PopID();
                ++catIndex;
            }
        }

        if (category.contains("effects") && category["effects"].is_array())
        {
            for (const auto& effect : category["effects"])
            {
                if (searchActive && !CachedEffectMatches(effect, s_dbSearchQueryLower, effectMatchCache))
                    continue;

                bool hiddenMatch = searchActive && CachedEffectHiddenMatches(effect, s_dbSearchQueryLower, effectMatchCache);
                if (s_dbSearchQueryChanged)
                    ImGui::SetNextItemOpen(hiddenMatch, ImGuiCond_Always);

                std::string effName = effect.value("name", std::string("(unnamed effect)"));
                int64_t     effectId = effect.value("__vfxd_effect_id", static_cast<int64_t>(0));
                bool        inJson   = effect.value("__vfxd_in_json", false);
                bool        isRenamingThis = IsDbTabEffectBeingRenamed(effectId);

                //_ REQUIRED, not defensive -- every effect row below used
                // the literal string id "effect" with nothing else
                // distinguishing siblings in ImGui's ID stack, which
                // collided for any category with more than one effect
                // (ImGui detects this as a hard error, not just a visual
                // glitch). effect_id is this effect's own stable, unique
                // identity -- the natural PushID key, and unique even
                // across categories, unlike an index.
                ImGui::PushID(static_cast<int>(effectId));

                bool nodeOpen = ImGui::TreeNode("effect", "%s%s", effName.c_str(), isRenamingThis ? " (renaming)" : "");

                //_ MUST come immediately after TreeNode, before any other
                // widget call -- BeginDragDropSource() with no explicit ID
                // falls back to window->DC.LastItemId, which the very next
                // widget (even a plain Text()) overwrites to 0. Drawing
                // the "(also installed)" badge first (as this used to)
                // zeroed it whenever inJson was true, and
                // BeginDragDropSource() hard-asserts on a null source ID
                // rather than failing quietly -- exactly the MSVC/clang-cl
                // "Expression: 0" crash reported, reproduced only on a
                // real mouse-down over such a row (a keyboard-nav-only
                // test never reaches that check at all, which is why an
                // earlier headless pass here missed it). Gated on no
                // other edit in flight, same reasoning as the JSON tabs'
                // own effect drag.
                if (!AnyEditInFlight() && ImGui::BeginDragDropSource())
                {
                    BeginDbTabEffectDrag(effectId, effName);
                    ImGui::SetDragDropPayload("VFXD_DBTAB_EFFECT", &kDbTabEffectDragMarker, sizeof(kDbTabEffectDragMarker));
                    ImGui::Text("Move \"%s\"", effName.c_str());
                    ImGui::EndDragDropSource();
                }

                if (inJson)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(kInJsonBadgeColor, "(also installed)");
                }

                if (!AnyEditInFlight() && ImGui::BeginPopupContextItem("dbtab_effect_ctx"))
                {
                    if (ImGui::MenuItem("Rename"))
                        BeginDbTabRename(effectId, effName);
                    ImGui::EndPopup();
                }

                if (nodeOpen)
                {
                    if (isRenamingThis)
                    {
                        RenderDbTabRenameEditor();
                    }
                    else
                    {
                        if (effect.contains("description") && effect["description"].is_string() &&
                            !effect["description"].get<std::string>().empty())
                            ImGui::TextWrapped("%s", effect["description"].get<std::string>().c_str());

                        if (effect.contains("guids") && effect["guids"].is_array())
                            for (const auto& g : effect["guids"])
                                if (g.is_string())
                                    ImGui::BulletText("%s", g.get<std::string>().c_str());
                    }
                    ImGui::TreePop();
                }

                ImGui::PopID();
            }
        }

        ImGui::TreePop();
    }
    else if (s_dbSearchQueryChanged)
    {
        SilentlyCloseChildren(category);
    }

    namePathSoFar.pop_back();
}

} //. namespace

void RenderEffectDbTab(const std::string& denoiserAddonDir)
{
    //_ Deliberately independent of "for science" capture ever having been
    // toggled on, and of VfxD_Greed.json existing -- see
    // EffectDb_EnsureOpenForBrowsing's own doc. A genuine open failure
    // (bad path, permissions, or -- most likely in practice -- a
    // migration failure on a corrupt/unexpected pre-existing file) is
    // surfaced here rather than silently rendering an empty tree.
    std::string openError;
    if (!EffectDb_EnsureOpenForBrowsing(denoiserAddonDir, openError))
    {
        ImGui::TextColored(kDuplicateColor, "Couldn't open the effect database: %s", openError.c_str());
        return;
    }

    RefreshInJsonFlagsIfNeeded();
    RebuildDbTreeIfNeeded();

    ImGui::TextDisabled("Every effect \"for science\" capture has ever seen, independent of any installed sin file.\n"
                         "Right-click an effect to rename it, or drag it onto a category to place it there.");

    ImGui::InputTextWithHint("##db_tab_search", "Search name / description / GUID...",
                              s_dbSearchBuf, sizeof(s_dbSearchBuf));
    if (s_dbSearchBuf[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##db_tab_search"))
            s_dbSearchBuf[0] = '\0';
    }

    std::string typedLower = s_dbSearchBuf;
    std::transform(typedLower.begin(), typedLower.end(), typedLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (!typedLower.empty() && typedLower.size() < kMinDbSearchLength)
        ImGui::TextDisabled("Keep typing... (search starts at %zu characters)", kMinDbSearchLength);

    std::string newQueryLower = (typedLower.size() >= kMinDbSearchLength) ? typedLower : std::string();
    s_dbSearchQueryChanged = (newQueryLower != s_dbSearchQueryLower);
    s_dbSearchQueryLower   = std::move(newQueryLower);

    if (!GetEditResultMessage().empty())
        ImGui::TextWrapped("%s", GetEditResultMessage().c_str());

    CategoryMatchCache matchCache;
    EffectMatchCache   effectMatchCache;
    if (!s_dbSearchQueryLower.empty())
        BuildCategoryMatchCache(s_dbTree, s_dbSearchQueryLower, matchCache, effectMatchCache);

    std::vector<std::string> namePathSoFar;
    if (s_dbTree.contains("categories") && s_dbTree["categories"].is_array())
    {
        if (s_dbTree["categories"].empty())
            ImGui::TextDisabled("Nothing captured yet.");

        for (const auto& cat : s_dbTree["categories"])
            RenderDbCategory(cat, namePathSoFar, matchCache, effectMatchCache);
    }

    //_ Deferred to here, after the tree has finished rendering for the
    // frame -- same reasoning as RenderInstalledEffects's own
    // end-of-function ApplyPending* calls.
    ApplyPendingDbTabRename();
    ApplyPendingDbTabCategoryPlacement();
}
