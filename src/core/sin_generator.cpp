//################################################################################
// sin_generator.cpp   (see: sin_generator.h)
//--------------------------------------------------------------------------------

#include "sin_generator.h"

#include "effect_db.h"

#include <algorithm>
#include <map>

namespace
{

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// JoinKey
//--------------------------------------------------------------------------------
// Local map key only -- doesn't need to match effect_db.cpp's private
// kCategoryDelim ('\x1f') since this is never written to SQL, only used to look
// an already-split EffectDbCategory::categoryPath up by value.
//--------------------------------------------------------------------------------
std::string JoinKey(const std::vector<std::string>& path)
{
    std::string out;
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (i) out += '\x1f';
        out += path[i];
    }
    return out;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// NormalizeCaster
//--------------------------------------------------------------------------------
// Mirrors generate_sins.py's own transform (not in this repo): Pride and Sloth
// both switch every Hide/SetDuration entry whose caster is All to Others. Show
// entries, and any entry not caster:All (already Others, say), pass through
// untouched. Gluttony never calls this.
//--------------------------------------------------------------------------------
std::string NormalizeCaster(const std::string& type, const std::string& caster)
{
    if ((type == "Hide" || type == "SetDuration") && caster == "All")
        return "Others";
    return caster;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateCategory
//--------------------------------------------------------------------------------
// Same "materialize on first reference" shape db_tree_view.cpp's
// FindOrCreateDbCategory uses, not shared with it -- that one always creates
// both "categories"/"effects" arrays (fine for an internal render tree
// everything else reads via .contains()) and tags nodes __vfxd_virtual
// (meaningless for a real output file). This one only adds
// "categories"/"effects" once something is actually placed under them, and
// pulls each category's own "description" from the externally-seeded
// `categories` table -- matching the hand-authored master's own minimal-keys
// shape (see the real VfxD_Greed.json: a category with no direct effects has no
// "effects" key at all, one with no description has no "description" key).
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindOrCreateCategory(nlohmann::ordered_json& root,
                                              const std::vector<std::string>& path,
                                              const std::map<std::string, const EffectDbCategory*>& categoriesByKey)
{
    nlohmann::ordered_json* cursor = &root;
    std::vector<std::string> soFar;
    soFar.reserve(path.size());

    for (const auto& segment : path)
    {
        soFar.push_back(segment);

        if (!cursor->contains("categories"))
            (*cursor)["categories"] = nlohmann::ordered_json::array();

        nlohmann::ordered_json* next = nullptr;
        for (auto& sub : (*cursor)["categories"])
            if (sub.value("name", std::string()) == segment) { next = &sub; break; }

        if (!next)
        {
            nlohmann::ordered_json newCat;
            newCat["name"] = segment;

            auto it = categoriesByKey.find(JoinKey(soFar));
            if (it != categoriesByKey.end() && !it->second->description.empty())
                newCat["description"] = it->second->description;

            (*cursor)["categories"].push_back(std::move(newCat));
            next = &(*cursor)["categories"].back();
        }
        cursor = next;
    }
    return cursor;
}

} //. anonymous namespace

nlohmann::ordered_json SinGenerator_Generate(ESinGeneratorVariant variant, int major, int minor)
{
    nlohmann::ordered_json root;
    root["version"]["major"] = major;
    root["version"]["minor"] = minor;

    std::vector<EffectDbEffect>   allEffects    = EffectDb_GetAllEffects();
    std::vector<EffectDbCategory> allCategories = EffectDb_GetAllCategories();

    std::map<std::string, const EffectDbCategory*> categoriesByKey;
    for (const auto& c : allCategories)
        categoriesByKey[JoinKey(c.categoryPath)] = &c;

    //_ Grouped by effect_id -- shared metadata read off the first member.
    std::map<int64_t, std::vector<const EffectDbEffect*>> byEffectId;
    for (const auto& e : allEffects)
    {
        if (e.categoryPath.empty())
            continue;
        //_ Sloth drops "Caution" entirely here -- not filtered post-hoc.
        if (variant == ESinGeneratorVariant::Sloth && e.categoryPath.front() == "Caution")
            continue;
        byEffectId[e.effect_id].push_back(&e);
    }

    //_ effect_id is just a tiebreaker -- sort_order duplicates aren't expected.
    std::vector<std::pair<int64_t, const std::vector<const EffectDbEffect*>*>> ordered;
    ordered.reserve(byEffectId.size());
    for (const auto& [effectId, members] : byEffectId)
        ordered.push_back({ effectId, &members });

    std::sort(ordered.begin(), ordered.end(),
        [](const auto& a, const auto& b)
        {
            int soA = a.second->front()->sortOrder;
            int soB = b.second->front()->sortOrder;
            if (soA != soB) return soA < soB;
            return a.first < b.first;
        });

    for (const auto& [effectId, membersPtr] : ordered)
    {
        const auto& members = *membersPtr;
        const EffectDbEffect& rep = *members.front();

        nlohmann::ordered_json* dest = FindOrCreateCategory(root, rep.categoryPath, categoriesByKey);

        nlohmann::ordered_json node;
        node["name"] = rep.name;
        if (!rep.description.empty())
            node["description"] = rep.description;

        nlohmann::ordered_json guids = nlohmann::ordered_json::array();
        for (const auto* m : members)
            guids.push_back(m->guid_b64);
        node["guids"] = std::move(guids);

        //_ Blank behaviorType writes an empty array, matching an unset effect.
        nlohmann::ordered_json behaviors = nlohmann::ordered_json::array();
        if (!rep.behaviorType.empty())
        {
            nlohmann::ordered_json behavior;
            behavior["type"] = rep.behaviorType;
            behavior["caster"] = (variant == ESinGeneratorVariant::Gluttony)
                ? rep.behaviorCaster
                : NormalizeCaster(rep.behaviorType, rep.behaviorCaster);
            if (rep.behaviorType == "SetDuration")
                behavior["duration"] = rep.behaviorDuration;
            behaviors.push_back(std::move(behavior));
        }
        node["behaviors"] = std::move(behaviors);

        if (!dest->contains("effects"))
            (*dest)["effects"] = nlohmann::ordered_json::array();
        (*dest)["effects"].push_back(std::move(node));
    }

    if (!root.contains("categories"))
        root["categories"] = nlohmann::ordered_json::array();

    return root;
}

int SinGenerator_CountEmittedGuids(ESinGeneratorVariant variant)
{
    std::vector<EffectDbEffect> allEffects = EffectDb_GetAllEffects();

    int count = 0;
    for (const auto& e : allEffects)
    {
        if (e.categoryPath.empty())
            continue;
        if (variant == ESinGeneratorVariant::Sloth && e.categoryPath.front() == "Caution")
            continue;
        ++count;
    }
    return count;
}