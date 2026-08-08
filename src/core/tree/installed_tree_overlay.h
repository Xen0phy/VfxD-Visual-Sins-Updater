//################################################################################
// installed_tree_overlay.h
//--------------------------------------------------------------------------------
// JoinPath()                  joins a category path into "A / B" for display
// BuildDiffOverlayTree()      tags a copy of `installed` with a pending
//                              update's diff (rework/merge/insert/move)
// BuildDuplicateOverlayTree() tags a copy of `installed` with duplicate-guid
//                              markers
//--------------------------------------------------------------------------------
// Pure data-transformation over an installed sin's JSON, split out of
// addon.cpp. No shared state, no ImGui calls -- every function here takes
// its inputs as parameters and returns a deep-copied overlay tree, so
// RenderInstalledEffects (still in addon.cpp) can paint a pending update's
// diff, or existing duplicate-guid problems, onto the installed tree it
// already renders, without a second, separate list ever existing. The
// "__vfxd_*" marker fields these functions add exist only in the returned
// in-memory copy and are never written back to disk.
//--------------------------------------------------------------------------------

#pragma once

#include "effect_db.h" //. EffectDbEffect
#include "merge.h"      //. nlohmann::ordered_json, MergePlan

#include <string>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// JoinPath
//--------------------------------------------------------------------------------
// Joins a category path like {"Combat", "Downstate"} into "Combat /
// Downstate". Kept here rather than its own header since it's tiny and
// pure -- also called from addon.cpp's JoinCategoryPathNames (the editing
// subsystem), which is why it stays a plain externally-visible inline
// function rather than moving into this file's own anonymous namespace.
//--------------------------------------------------------------------------------
inline std::string JoinPath(const std::vector<std::string>& path)
{
    std::string out;
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (i) out += " / ";
        out += path[i];
    }
    return out;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildDiffOverlayTree
//--------------------------------------------------------------------------------
// Deep-copies `installed` and overlays `plan` onto it purely for display,
// so the installed-effects tree stays the single source of truth for what
// a pending update would do instead of a second, separate list. Full
// phase-ordering writeup (mirrors ApplyMergePlan's index-once /
// mutate-in-place / remove-once / reinsert-once shape) is on this
// function's own definition in installed_tree_overlay.cpp.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildDiffOverlayTree(const nlohmann::ordered_json& installed, const MergePlan& plan);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildDuplicateOverlayTree
//--------------------------------------------------------------------------------
// Deep-copies `installed` and tags every effect owning one of `dupeGuids`
// with a "__vfxd_dupe_guid" marker, bubbling a "__vfxd_hasdupe" marker up
// onto ancestor categories. Flags a correctness problem already present in
// the installed file itself, not a pending update.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildDuplicateOverlayTree(const nlohmann::ordered_json& installed, const std::vector<std::string>& dupeGuids);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildEffectDbOverlayTree
//--------------------------------------------------------------------------------
// Deep-copies `installed` and adds one synthetic effect node, tagged
// "__vfxd_db_only", per guid in `dbEffects` that ISN'T already a real
// effect somewhere in `installed` -- a guid that's already JSON-backed
// just gets skipped here entirely, it's not this overlay's concern.
//
// Placed at each db-only guid's own categoryPath (see effect_db.h's
// EffectDb_SetCategoryPath), materializing categories that don't exist
// in `installed` yet exactly the way BuildDiffOverlayTree already does
// for a pending update's brand-new categories (same
// FindOrCreateDiffCategory helper, same "__vfxd_virtual" tag on any
// category it has to create) -- one guid with an empty categoryPath
// (never placed by a drag yet) falls into a synthetic "Unrecognized
// (for science)" bucket at the root instead of being dropped.
//
// Meant to be called on `installed` for whichever sin "for science"
// promotion always targets (Greed) -- see effect_db.h on why promotion
// has no per-guid file choice to make. Calling it against a different
// sin isn't wrong, exactly, it's just modeling a placement decision that
// promotion would never actually honor.
//
// outAddedCount, if non-null, receives how many db-only nodes were
// actually added (i.e. excluding guids skipped as already JSON-backed) --
// lets a caller drive an "any db-only content shown" legend line without
// re-querying the db itself every frame just to find out.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildEffectDbOverlayTree(const nlohmann::ordered_json& installed, const std::vector<EffectDbEffect>& dbEffects,
                                                 size_t* outAddedCount = nullptr);