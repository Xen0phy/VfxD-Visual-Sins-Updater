//################################################################################
// installed_tree_search.cpp
//--------------------------------------------------------------------------------
// See installed_tree_search.h for the module contract. Installed-effects
// tree search matching/closing functions, extracted from addon.cpp -- a
// mechanical move, no behavior change.
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

    //_ Mirror the ID scope TreeNode(name) would have auto-pushed for its
    // children had it actually opened.
    ImGui::PushID(name.c_str());
    SilentlyCloseChildren(category);
    ImGui::PopID();
}