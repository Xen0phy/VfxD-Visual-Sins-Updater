//################################################################################
// installed_tree_search.cpp   (see: installed_tree_search.h)
//--------------------------------------------------------------------------------

#include "imgui.h"
#include "installed_tree_search.h"

#include <algorithm>
#include <cctype>

bool ContainsCI(const std::string& haystack, const std::string& needleLower)
{
    if (needleLower.empty())
        return true;

    std::string haystackLower = haystack;
    std::transform(haystackLower.begin(), haystackLower.end(), haystackLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystackLower.find(needleLower) != std::string::npos;
}

bool EffectNameMatches(const nlohmann::ordered_json& effect, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;
    return ContainsCI(effect.value("name", std::string()), queryLower);
}

bool EffectHiddenContentMatches(const nlohmann::ordered_json& effect, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;

    if (ContainsCI(effect.value("description", std::string()), queryLower))
        return true;

    if (effect.contains("guids") && effect["guids"].is_array())
        for (const auto& g : effect["guids"])
            if (g.is_string() && ContainsCI(g.get<std::string>(), queryLower))
                return true;

    return false;
}

bool EffectMatchesSearch(const nlohmann::ordered_json& effect, const std::string& queryLower)
{
    if (queryLower.empty())
        return true;
    return EffectNameMatches(effect, queryLower) || EffectHiddenContentMatches(effect, queryLower);
}

bool CategoryNameMatches(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;
    return ContainsCI(category.value("name", std::string()), queryLower);
}

bool CategoryDescriptionMatches(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;
    return ContainsCI(category.value("description", std::string()), queryLower);
}

bool CategorySubtreeMatchesSearch(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return true;

    if (CategoryNameMatches(category, queryLower) || CategoryDescriptionMatches(category, queryLower))
        return true;

    if (category.contains("effects") && category["effects"].is_array())
        for (const auto& eff : category["effects"])
            if (EffectMatchesSearch(eff, queryLower))
                return true;

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            if (CategorySubtreeMatchesSearch(sub, queryLower))
                return true;

    return false;
}

bool CategoryHasDescendantMatch(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;

    if (category.contains("effects") && category["effects"].is_array())
        for (const auto& eff : category["effects"])
            if (EffectMatchesSearch(eff, queryLower))
                return true;

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            if (CategoryNameMatches(sub, queryLower) || CategoryDescriptionMatches(sub, queryLower) ||
                CategoryHasDescendantMatch(sub, queryLower))
                return true;

    return false;
}

void BuildCategoryMatchCache(const nlohmann::ordered_json& category, const std::string& queryLower,
                              CategoryMatchCache& categoryCache, EffectMatchCache& effectCache)
{
    //_ Bottom-up -- children cached before their parent needs the answer.
    bool anyDescendantMatches = false;

    if (category.contains("effects") && category["effects"].is_array())
        for (const auto& eff : category["effects"])
        {
            EffectMatchResult effResult;
            effResult.hiddenMatches = EffectHiddenContentMatches(eff, queryLower);
            effResult.matches       = queryLower.empty() || effResult.hiddenMatches || EffectNameMatches(eff, queryLower);
            effectCache[&eff] = effResult;

            if (effResult.matches)
                anyDescendantMatches = true;
        }

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
        {
            BuildCategoryMatchCache(sub, queryLower, categoryCache, effectCache);
            if (categoryCache[&sub].subtreeMatches)
                anyDescendantMatches = true;
        }

    CategoryMatchResult result;
    result.hasDescendantMatch = !queryLower.empty() && anyDescendantMatches;
    result.subtreeMatches     = queryLower.empty() || anyDescendantMatches ||
        CategoryNameMatches(category, queryLower) || CategoryDescriptionMatches(category, queryLower);
    categoryCache[&category] = result;
}

bool CachedSubtreeMatches(const nlohmann::ordered_json& category, const std::string& queryLower,
                           const CategoryMatchCache& cache)
{
    auto it = cache.find(&category);
    if (it != cache.end())
        return it->second.subtreeMatches;
    return CategorySubtreeMatchesSearch(category, queryLower); //. fallback: cache miss
}

bool CachedHasDescendantMatch(const nlohmann::ordered_json& category, const std::string& queryLower,
                               const CategoryMatchCache& cache)
{
    auto it = cache.find(&category);
    if (it != cache.end())
        return it->second.hasDescendantMatch;
    return CategoryHasDescendantMatch(category, queryLower); //. fallback: cache miss
}

bool CachedEffectMatches(const nlohmann::ordered_json& effect, const std::string& queryLower,
                          const EffectMatchCache& cache)
{
    auto it = cache.find(&effect);
    if (it != cache.end())
        return it->second.matches;
    return EffectMatchesSearch(effect, queryLower); //. fallback: cache miss
}

bool CachedEffectHiddenMatches(const nlohmann::ordered_json& effect, const std::string& queryLower,
                                const EffectMatchCache& cache)
{
    auto it = cache.find(&effect);
    if (it != cache.end())
        return it->second.hiddenMatches;
    return EffectHiddenContentMatches(effect, queryLower); //. fallback: cache miss
}

void SilentlyCloseChildren(const nlohmann::ordered_json& category)
{
    if (category.contains("effects") && category["effects"].is_array())
    {
        int i = 0;
        for (const auto& eff : category["effects"])
        {
            (void)eff;
            ImGui::PushID(i);
            ImGui::GetStateStorage()->SetInt(ImGui::GetID("effect"), 0);
            ImGui::PopID();
            ++i;
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
    {
        int i = 0;
        for (const auto& sub : category["categories"])
        {
            ImGui::PushID(i);
            SilentlyCloseSubtree(sub);
            ImGui::PopID();
            ++i;
        }
    }
}

void SilentlyCloseSubtree(const nlohmann::ordered_json& category)
{
    std::string name = category.value("name", std::string("(unnamed category)"));
    ImGui::GetStateStorage()->SetInt(ImGui::GetID(name.c_str()), 0);

    //_ Mirrors the ID scope TreeNode(name) would have auto-pushed if opened.
    ImGui::PushID(name.c_str());
    SilentlyCloseChildren(category);
    ImGui::PopID();
}