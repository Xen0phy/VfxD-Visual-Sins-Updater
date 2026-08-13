#include "db_tree_view.h"

#include <algorithm>
#include <map>
#include <string>

namespace
{

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildOccurrencesJson
//--------------------------------------------------------------------------------
// EffectDb_GetOccurrences(guid), reshaped into the flat JSON array
// RenderEffectDbDetail groups at render time. Moved here from
// installed_tree_overlay.cpp's now-retired BuildEffectDbOverlayTree -- the
// shape RenderEffectDbDetail expects is unchanged, only where it's built
// from. specialization_ids is pre-unpacked (see EffectDb_SpecOrCoreIdsInMask)
// so the render-time consumer never touches EffectDbSpecializationMask's
// lo/hi words directly.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildOccurrencesJson(const std::string& guid_b64)
{
    nlohmann::ordered_json occurrences = nlohmann::ordered_json::array();
    for (const auto& occ : EffectDb_GetOccurrences(guid_b64))
    {
        nlohmann::ordered_json o;
        o["duration"]           = occ.duration;
        o["a4"]                 = occ.a4;
        o["a6"]                 = occ.a6;
        o["self_mask"]          = static_cast<int>(occ.self_mask);
        o["race_mask"]          = occ.raceMask;
        //_ Flattened to a plain array of raw ids (1..127, see effect_db.h's
        // EffectDbSpecializationMask) rather than the lo/hi words -- easier
        // for the render-time consumer to iterate without redoing the unpack.
        o["specialization_ids"] = EffectDb_SpecOrCoreIdsInMask(occ.specializationMask);
        occurrences.push_back(std::move(o));
    }
    return occurrences;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildGroupsJson
//--------------------------------------------------------------------------------
// EffectDb_GetGroupsStarted/EffectDb_GetGroupsMemberOf(guid), reshaped
// into the flat JSON RenderEffectDbDetail reads at render time -- same
// "bake it into the cache once per rebuild, not once per frame" shape as
// BuildOccurrencesJson right above. Two arrays under one object so the
// tree's render side can tell "guids this one swept up when it opened a
// group" apart from "groups this one got swept into" without a second
// lookup. Moved here alongside BuildOccurrencesJson -- see its comment.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildGroupsJson(const std::string& guid_b64)
{
    nlohmann::ordered_json groups;

    nlohmann::ordered_json started = nlohmann::ordered_json::array();
    for (const auto& inst : EffectDb_GetGroupsStarted(guid_b64))
    {
        nlohmann::ordered_json s;
        s["duration"] = inst.duration;
        s["a4"]       = inst.a4;
        s["members"]  = inst.memberGuids;
        started.push_back(std::move(s));
    }
    groups["started"] = std::move(started);

    nlohmann::ordered_json memberOf = nlohmann::ordered_json::array();
    for (const auto& m : EffectDb_GetGroupsMemberOf(guid_b64))
    {
        nlohmann::ordered_json mo;
        mo["starter_guid_b64"] = m.starterGuid_b64;
        mo["duration"]         = m.duration;
        mo["a4"]               = m.a4;
        memberOf.push_back(std::move(mo));
    }
    groups["member_of"] = std::move(memberOf);

    return groups;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateDbCategory
//--------------------------------------------------------------------------------
// Same shape as installed_tree_overlay.cpp's FindOrCreateDiffCategory --
// deliberately not shared with it, since that function lives in a "dead
// code once the DB tab ships" file (see the handoff) and this one is
// meant to outlive it. Tags every materialized category
// "__vfxd_virtual" so RenderCategoryTree's existing gate suppresses
// JSON-only editing affordances (drag, delete, create, right-click) on
// it for free -- there is no JSON position for any DB tab category to
// have, ever, not just until it's promoted.
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindOrCreateDbCategory(nlohmann::ordered_json& root, const std::vector<std::string>& path)
{
    nlohmann::ordered_json* cursor = &root;
    for (const auto& segment : path)
    {
        if (!cursor->contains("categories") || !(*cursor)["categories"].is_array())
            (*cursor)["categories"] = nlohmann::ordered_json::array();

        nlohmann::ordered_json* next = nullptr;
        for (auto& sub : (*cursor)["categories"])
            if (sub.value("name", std::string()) == segment) { next = &sub; break; }

        if (!next)
        {
            nlohmann::ordered_json newCat;
            newCat["name"]           = segment;
            newCat["categories"]     = nlohmann::ordered_json::array();
            newCat["effects"]        = nlohmann::ordered_json::array();
            newCat["__vfxd_virtual"] = true;   //. no JSON position, ever -- see comment above
            (*cursor)["categories"].push_back(std::move(newCat));
            next = &(*cursor)["categories"].back();
        }
        cursor = next;
    }
    return cursor;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SortTreeRecursive
//--------------------------------------------------------------------------------
// Alphabetical by name, categories then effects, at every level -- the
// "deterministic, no drag-to-reorder" decision from the handoff. Effects
// grouped under one effect_id have no per-guid ordering concept either;
// nothing here needs one, since a node's "guids" array is just a set,
// never displayed as an ordered list the user controls.
//--------------------------------------------------------------------------------
void SortTreeRecursive(nlohmann::ordered_json& node)
{
    if (node.contains("categories") && node["categories"].is_array())
    {
        auto& cats = node["categories"];
        std::sort(cats.begin(), cats.end(), [](const nlohmann::ordered_json& a, const nlohmann::ordered_json& b)
        {
            return a.value("name", std::string()) < b.value("name", std::string());
        });
        for (auto& sub : cats)
            SortTreeRecursive(sub);
    }
    if (node.contains("effects") && node["effects"].is_array())
    {
        auto& effs = node["effects"];
        std::sort(effs.begin(), effs.end(), [](const nlohmann::ordered_json& a, const nlohmann::ordered_json& b)
        {
            return a.value("name", std::string()) < b.value("name", std::string());
        });
    }
}

} //. anonymous namespace

nlohmann::ordered_json BuildDbTree(const std::vector<EffectDbEffect>& dbEffects,
                                    const std::unordered_set<std::string>& installedGuids)
{
    nlohmann::ordered_json root;
    root["categories"] = nlohmann::ordered_json::array();
    root["effects"]    = nlohmann::ordered_json::array();

    //_ Group by effect_id first -- every guid sharing one effect_id folds
    // into a single node's "guids" array, rather than each guid getting
    // its own leaf. name/category_path/description/behavior* are
    // identical across every guid in a group by construction (they all
    // come from the one effect_meta row the join attached to each of
    // them), so it's correct to read them off any single member of the
    // group -- picking the first is arbitrary but never wrong.
    std::map<int64_t, std::vector<const EffectDbEffect*>> byEffectId;
    for (const auto& e : dbEffects)
        byEffectId[e.effect_id].push_back(&e);

    for (const auto& [effectId, members] : byEffectId)
    {
        const EffectDbEffect& rep = *members.front();  //. representative -- see comment above

        nlohmann::ordered_json* dest = FindOrCreateDbCategory(root, rep.categoryPath);
        //. rep.categoryPath.empty() naturally lands at `root` itself here,
        // which is exactly the "Uncategorized" bucket's contents -- see
        // below for why it's wrapped in its own named category instead
        // of left loose at the top level.

        nlohmann::ordered_json node;
        node["name"]            = rep.name;
        node["description"]     = rep.description;
        node["__vfxd_db_effect"] = true;   //. NOT the old "__vfxd_db_only" shape -- see db_tree_view.h
        node["effect_id"]        = effectId;
        node["in_json"]          = std::any_of(members.begin(), members.end(),
                                                [&installedGuids](const EffectDbEffect* m)
                                                { return installedGuids.count(m->guid_b64) != 0; });
        //. any() rather than all(): a partially-promoted effect (some
        // sibling guids already in JSON, some not) still counts as
        // "also in JSON" for display purposes -- this is a presence
        // badge, not a completeness claim, see the handoff. Cross-
        // referenced against the live `installedGuids` snapshot, not
        // EffectDbEffect::in_json -- see this function's header comment.

        nlohmann::ordered_json guids = nlohmann::ordered_json::array();
        for (const auto* m : members)
            guids.push_back(m->guid_b64);
        node["guids"] = std::move(guids);

        //_ Same "__vfxd_db_by_guid" shape installed_tree_overlay.cpp's
        // now-retired BuildEffectDbOverlayTree used to build for the JSON
        // tab's "for science data" section -- moving it here (instead of
        // that section just disappearing) is the whole point: this data
        // still needs a home, and the DB tab is now it. RenderEffectDbDetail
        // reads this shape unchanged, no render-side work needed.
        nlohmann::ordered_json byGuid = nlohmann::ordered_json::object();
        for (const auto* m : members)
        {
            nlohmann::ordered_json detail;
            detail["block_group"]  = m->blockGroup;
            detail["block_member"] = m->blockMember;
            detail["type"]         = m->type;
            detail["occurrences"]  = BuildOccurrencesJson(m->guid_b64);
            detail["groups"]       = BuildGroupsJson(m->guid_b64);
            byGuid[m->guid_b64] = std::move(detail);
        }
        node["__vfxd_db_by_guid"] = std::move(byGuid);

        if (!rep.behaviorType.empty())
        {
            nlohmann::ordered_json behavior;
            behavior["type"]   = rep.behaviorType;
            behavior["caster"] = rep.behaviorCaster;
            if (rep.behaviorType == "SetDuration")
                behavior["duration"] = rep.behaviorDuration;
            node["behaviors"] = nlohmann::ordered_json::array({ std::move(behavior) });
        }
        else
        {
            node["behaviors"] = nlohmann::ordered_json::array();
        }

        (*dest)["effects"].push_back(std::move(node));
    }

    //_ Effects with an empty category_path all landed directly on `root`
    // above (FindOrCreateDbCategory with an empty path is a no-op cursor
    // walk, returning &root itself) -- move them into a real named
    // bucket rather than rendering loose at the tab's top level, same
    // idea as the old overlay's "Unrecognized (for science)" bucket.
    if (!root["effects"].empty())
    {
        nlohmann::ordered_json uncategorized;
        uncategorized["name"]           = "Uncategorized";
        uncategorized["categories"]     = nlohmann::ordered_json::array();
        uncategorized["effects"]        = std::move(root["effects"]);
        uncategorized["__vfxd_virtual"] = true;
        root["effects"] = nlohmann::ordered_json::array();
        root["categories"].push_back(std::move(uncategorized));
    }

    SortTreeRecursive(root);
    return root;
}