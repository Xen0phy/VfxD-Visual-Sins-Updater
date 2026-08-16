//################################################################################
// db_tree_view.h
//--------------------------------------------------------------------------------
// BuildDbTree(dbEffects, installedGuids)   builds the DB tab's category tree
// kDbTabSinName                            sentinel "sinName" for the DB tab
//--------------------------------------------------------------------------------
// Split out of installed_tree_view.cpp so the DB tab's tree-building logic
// (grouping effect_db rows into a category tree) sits apart from the JSON tab's
// render code. kDbTabSinName lets the DB tab reuse RenderCategoryTree and
// s_searchCache's per-sin machinery under a fake "sin" identity, since the DB tab
// isn't backed by a real sin file.
//--------------------------------------------------------------------------------

#pragma once

#include "effect_db.h"
#include "nlohmann_json.hpp"

#include <string>
#include <unordered_set>
#include <vector>

inline constexpr const char* kDbTabSinName = "__vfxd_db_tab__";

nlohmann::ordered_json BuildDbTree(const std::vector<EffectDbEffect>& dbEffects,
                                    const std::unordered_set<std::string>& installedGuids);