#pragma once
#include "core/merge.h" // nlohmann::ordered_json, MergePlan
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Pure data-transformation over an installed sin's JSON, split out of
// addon.cpp. No shared state, no
// ImGui calls -- every function here takes its inputs as parameters and
// returns a deep-copied overlay tree, so RenderInstalledEffects (still in
// addon.cpp) can paint a pending update's diff, or
// existing duplicate-guid problems, onto the installed tree it already
// renders, without a second, separate list ever existing.
// ---------------------------------------------------------------------------

// Joins a category path like {"Combat", "Downstate"} into "Combat / Downstate".
// Tiny and pure enough that it's kept here rather than in its own header --
// also called from addon.cpp's JoinCategoryPathNames (part of the editing
// subsystem), which is why this stays a plain externally-visible
// inline function rather than moving into this file's own anonymous
// namespace.
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

// Deep-copies `installed` and overlays `plan` onto it purely for display,
// so the installed-effects tree stays the single source of truth for what
// a pending update would do instead of a second, separate list. See this
// function's own definition in installed_tree_overlay.cpp for the full
// phase-ordering writeup (mirrors ApplyMergePlan's index-once /
// mutate-in-place / remove-once / reinsert-once shape). Nothing here is
// ever written back to disk -- the "__vfxd_*" marker fields it adds exist
// only in this in-memory copy.
nlohmann::ordered_json BuildDiffOverlayTree(const nlohmann::ordered_json& installed, const MergePlan& plan);

// Deep-copies `installed` and tags every effect owning one of `dupeGuids`
// with a display-only "__vfxd_dupe_guid" marker (bubbling a
// "__vfxd_hasdupe" marker up onto ancestor categories), for the same
// reason BuildDiffOverlayTree above never mutates the store's own copy:
// these markers must never reach what ApplyPendingEdit/SaveInstalledSinFile
// eventually serialize back to disk verbatim. Flags a correctness problem
// already present in the installed file itself, not a pending update.
nlohmann::ordered_json BuildDuplicateOverlayTree(const nlohmann::ordered_json& installed, const std::vector<std::string>& dupeGuids);
