#include "sin_generator.h"

#include "effect_db.h"

#include <algorithm>
#include <map>

namespace
{

//_ Local map key only -- doesn't need to match effect_db.cpp's private
// kCategoryDelim ('\x1f'), since this is never written to SQL, only used
// to look an already-split EffectDbCategory::categoryPath up by value.
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
// NormalizeBehaviorForVariant
//--------------------------------------------------------------------------------
// generate_sins.py's own transform (see HANDOFF_VfxSins.md, not in this
// repo -- rules confirmed against it in TODO_B.md item 7): Pride and
// Sloth both switch every Hide/SetDuration entry whose caster is All to
// Others. Show entries, and any entry not caster:All (already Others,
// say), pass through untouched. Gluttony never calls this.
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
// FindOrCreateDbCategory uses, deliberately not shared with it -- that
// one always creates both "categories"/"effects" arrays (fine for an
// internal render tree everything else reads via .contains()) and tags
// nodes __vfxd_virtual (meaningless for a real output file). This one
// only adds "categories"/"effects" once something is actually placed
// under them, and pulls each category's own "description" from the
// externally-seeded `categories` table -- matching the hand-authored
// master's own minimal-keys shape (see the real VfxD_Greed.json: a
// category with no direct effects has no "effects" key at all, one with
// no description has no "description" key).
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
    //_ {major, minor} object, not a bare int -- matches the shape VfxD
    // itself writes at the top of every file it manages (see sin_files.h's
    // ScanInstalledSinFiles doc and this function's own header comment).
    root["version"]["major"] = major;
    root["version"]["minor"] = minor;

    std::vector<EffectDbEffect>   allEffects    = EffectDb_GetAllEffects();
    std::vector<EffectDbCategory> allCategories = EffectDb_GetAllCategories();

    std::map<std::string, const EffectDbCategory*> categoriesByKey;
    for (const auto& c : allCategories)
        categoriesByKey[JoinKey(c.categoryPath)] = &c;

    //_ Group by effect_id first -- every guid sharing one effect_id is one
    // curated effect, one output node. name/category_path/description/
    // behavior*/sort_order are identical across the group by construction
    // (one shared effect_meta row), so reading them off the first member
    // is never wrong, same reasoning db_tree_view.cpp's BuildDbTree uses.
    std::map<int64_t, std::vector<const EffectDbEffect*>> byEffectId;
    for (const auto& e : allEffects)
    {
        //_ Matches BuildDbTree's own "Uncategorized" carve-out reasoning,
        // inverted: an uncategorized effect has nowhere real to land in a
        // generated file, so it's excluded entirely rather than invented
        // a bucket for -- see this file's header comment.
        if (e.categoryPath.empty())
            continue;
        //_ Sloth drops the entire top-level "Caution" category, whatever
        // it contains -- checked here (not just at materialization) so a
        // Caution-only sub-branch never even allocates an entry in
        // byEffectId, matching "gets dropped, not filtered post-hoc".
        if (variant == ESinGeneratorVariant::Sloth && e.categoryPath.front() == "Caution")
            continue;
        byEffectId[e.effect_id].push_back(&e);
    }

    //_ Ascending sort_order, effect_id as a stable tiebreaker only (two
    // effects should never truly share a sort_order from a real seed, but
    // std::map already handles the primary key -- this just keeps the
    // walk fully deterministic if they ever do).
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

    //_ Construction order IS the final order here -- deliberately no
    // alphabetical SortTreeRecursive the way db_tree_view.cpp's DB tab
    // applies; that sort is a DB-tab display convenience and would
    // defeat sort_order's entire purpose in a generated file.
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

        //_ Owner-confirmed: real effects always carry exactly one
        // behavior (see TODO_B.md item 7), so effect_meta's single
        // behavior_type/caster/duration columns round-trip into a
        // one-entry array without any collapse ambiguity. An effect
        // whose behavior_type is blank (never captured/seeded a
        // default) writes an empty array, same as a real installed
        // effect with no behaviors set yet.
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
