//################################################################################
// db_tree_view.cpp   (see: db_tree_view.h)
//--------------------------------------------------------------------------------

#include "db_tree_view.h"

#include "effect_db.h"
#include "game_state.h"          //. GameState_ProfessionName
#include "nlohmann_json.hpp"
#include "specialization_info.h" //. SpecializationProfession

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_set>

namespace
{

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildOccurrencesJson
//--------------------------------------------------------------------------------
// EffectDb_GetOccurrences(guid), reshaped into the flat JSON array
// RenderEffectDbDetail groups at render time. Moved here from
// installed_tree_overlay.cpp's now-retired BuildEffectDbOverlayTree -- the shape
// RenderEffectDbDetail expects is unchanged, only where it's built from.
// specialization_ids is pre-unpacked (see EffectDb_SpecOrCoreIdsInMask) so the
// render-time consumer never touches EffectDbSpecializationMask's lo/hi words
// directly.
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
        //_ Flattened to raw ids 1..127 -- see effect_db.h's EffectDbSpecializationMask
        o["specialization_ids"] = EffectDb_SpecOrCoreIdsInMask(occ.specializationMask);
        occurrences.push_back(std::move(o));
    }
    return occurrences;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildGroupsJson
//--------------------------------------------------------------------------------
// EffectDb_GetGroupsStarted/EffectDb_GetGroupsMemberOf(guid), reshaped into the
// flat JSON RenderEffectDbDetail reads at render time -- same "bake it into the
// cache once per rebuild, not once per frame" shape as BuildOccurrencesJson right
// above. Two arrays under one object so the tree's render side can tell "guids
// this one swept up when it opened a group" apart from "groups this one got swept
// into" without a second lookup. Moved here alongside BuildOccurrencesJson -- see
// its comment.
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
// JoinKey
//--------------------------------------------------------------------------------
// Local map key only -- doesn't need to match effect_db.cpp's private
// category-path delimiter, since this is never written to SQL, only used to look
// an already-split EffectDbCategory::categoryPath up by value. Same shape as
// sin_generator.cpp's own JoinKey, not shared with it (different translation
// unit, trivial enough not to warrant a shared header).
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
// FindOrCreateDbCategory
//--------------------------------------------------------------------------------
// Same shape as installed_tree_overlay.cpp's FindOrCreateDiffCategory, kept
// separate since that one serves the JSON tab's overlay tree. Tags every
// materialized category "__vfxd_virtual" so RenderCategoryTree's gate suppresses
// JSON-only editing affordances (drag, delete, create, right-click) -- no JSON
// position exists for any DB tab category, ever.
//
// Also stamps "__vfxd_sort_order" (the `categories` table's sort_order, via
// `sortOrderByKey`) on every node it creates, so SortTreeRecursive can mirror the
// curated JSON's order instead of alphabetizing -- omitted when a segment isn't
// in the table so SortTreeRecursive can tell unordered from curated position 0.
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindOrCreateDbCategory(nlohmann::ordered_json& root, const std::vector<std::string>& path,
                                                const std::map<std::string, int>& sortOrderByKey)
{
    nlohmann::ordered_json* cursor = &root;
    std::vector<std::string> soFar;
    soFar.reserve(path.size());
    for (const auto& segment : path)
    {
        soFar.push_back(segment);

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
            newCat["__vfxd_virtual"] = true;

            auto it = sortOrderByKey.find(JoinKey(soFar));
            if (it != sortOrderByKey.end())
                newCat["__vfxd_sort_order"] = it->second;

            (*cursor)["categories"].push_back(std::move(newCat));
            next = &(*cursor)["categories"].back();
        }
        cursor = next;
    }
    return cursor;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ClassFolderNameFromByGuidJson
//--------------------------------------------------------------------------------
// Level 1 of the Uncategorized sort: union the professions implied by every
// occurrence of every guid on this node, read off the node's own
// "__vfxd_db_by_guid" -> "occurrences" -> "specialization_ids" JSON that
// BuildOccurrencesJson already built -- not a second
// EffectDb_GetOccurrences(guid_b64) call per guid. Ids resolve to a profession
// via SpecializationProfession() or, for a core-only pseudo-id (>=
// kEffectDbCoreOnlyIdFloor), EffectDb_ProfessionFromCoreOnlyId() (same split
// DecodeSpecOrCoreId uses elsewhere). One profession -> that folder; more than
// one, or none, -> "Multiple".
//--------------------------------------------------------------------------------
std::string ClassFolderNameFromByGuidJson(const nlohmann::ordered_json& byGuid)
{
    std::set<Mumble::EProfession> professions;
    for (const auto& [guid_b64, detail] : byGuid.items())
    {
        if (!detail.contains("occurrences"))
            continue;
        for (const auto& occ : detail["occurrences"])
        {
            if (!occ.contains("specialization_ids"))
                continue;
            for (const auto& idJson : occ["specialization_ids"])
            {
                const unsigned int id = idJson.get<unsigned int>();
                const Mumble::EProfession prof = (id >= kEffectDbCoreOnlyIdFloor)
                    ? EffectDb_ProfessionFromCoreOnlyId(id)
                    : SpecializationProfession(id);
                if (prof != Mumble::EProfession::None)
                    professions.insert(prof);
            }
        }
    }

    if (professions.size() == 1)
        return GameState_ProfessionName(*professions.begin());
    return "Multiple";
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateVirtualChild
//--------------------------------------------------------------------------------
// One-segment version of FindOrCreateDbCategory's materialize-if-missing logic --
// find `name` among `parent`'s "categories", or create it (tagged
// "__vfxd_virtual", same reasoning as FindOrCreateDbCategory) if absent. Kept
// separate from FindOrCreateDbCategory.
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindOrCreateVirtualChild(nlohmann::ordered_json& parent, const std::string& name)
{
    if (!parent.contains("categories") || !parent["categories"].is_array())
        parent["categories"] = nlohmann::ordered_json::array();

    for (auto& sub : parent["categories"])
        if (sub.value("name", std::string()) == name)
            return &sub;

    nlohmann::ordered_json newCat;
    newCat["name"]           = name;
    newCat["categories"]     = nlohmann::ordered_json::array();
    newCat["effects"]        = nlohmann::ordered_json::array();
    newCat["__vfxd_virtual"] = true;
    parent["categories"].push_back(std::move(newCat));
    return &parent["categories"].back();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindOrCreateUncategorizedBucket
//--------------------------------------------------------------------------------
// Materializes Uncategorized/<class>/type N lazily, one segment at a time, via
// FindOrCreateVirtualChild -- so a class/type combination nothing lands in never
// appears as an empty folder, same rule the rest of the DB tab already follows.
// `classFolder` is one of the 9 profession names or "Multiple"; `typeFolder` is
// "type N" for effects.type (0-11), used directly with no aggregation -- type is
// a fixed per-guid attribute, occurring exactly once, never ambiguous the way
// profession can be, so there's no third "Multiple" case at this level.
//--------------------------------------------------------------------------------
nlohmann::ordered_json* FindOrCreateUncategorizedBucket(nlohmann::ordered_json& root,
                                                          const std::string& classFolder,
                                                          const std::string& typeFolder)
{
    nlohmann::ordered_json* uncategorized = FindOrCreateVirtualChild(root, "Uncategorized");
    nlohmann::ordered_json* classNode     = FindOrCreateVirtualChild(*uncategorized, classFolder);
    return FindOrCreateVirtualChild(*classNode, typeFolder);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SortTreeRecursive
//--------------------------------------------------------------------------------
// Categories: mirrors the curated JSON's own authored order via each node's
// "__vfxd_sort_order" (stamped by FindOrCreateDbCategory -- see BuildDbTree) when
// present, ascending. A node without one (every Uncategorized/<class>/type N
// folder, or any real category segment the seed hasn't given a sort_order to yet)
// sorts after every node that has one, alphabetically among themselves. Effects:
// always alphabetical by name -- sort_order isn't extended to effects.
//--------------------------------------------------------------------------------
void SortTreeRecursive(nlohmann::ordered_json& node)
{
    if (node.contains("categories") && node["categories"].is_array())
    {
        auto& cats = node["categories"];
        std::sort(cats.begin(), cats.end(), [](const nlohmann::ordered_json& a, const nlohmann::ordered_json& b)
        {
            const bool aHas = a.contains("__vfxd_sort_order");
            const bool bHas = b.contains("__vfxd_sort_order");
            if (aHas != bHas)
                return aHas;   //. curated position sorts first
            if (aHas && bHas)
            {
                const int soA = a["__vfxd_sort_order"].get<int>();
                const int soB = b["__vfxd_sort_order"].get<int>();
                if (soA != soB)
                    return soA < soB;
            }
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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BuildDbTree
//--------------------------------------------------------------------------------
// Builds the DB tab's category tree from effect_db, grouping dbEffects by
// effect_id into one node per effect (guids merged into a "guids" array). A real
// categoryPath goes through FindOrCreateDbCategory; an empty one falls into
// Uncategorized/<class>/<type> via ClassFolderNameFromByGuidJson. installedGuids
// drives the "in_json" badge. Sorted via SortTreeRecursive before returning.
//--------------------------------------------------------------------------------
nlohmann::ordered_json BuildDbTree(const std::vector<EffectDbEffect>& dbEffects,
                                    const std::unordered_set<std::string>& installedGuids)
{
    nlohmann::ordered_json root;
    root["categories"] = nlohmann::ordered_json::array();
    root["effects"]    = nlohmann::ordered_json::array();

    //_ Categories table's sort_order, keyed by joined path
    std::map<std::string, int> sortOrderByKey;
    for (const auto& c : EffectDb_GetAllCategories())
        sortOrderByKey[JoinKey(c.categoryPath)] = c.sortOrder;

    //_ name/description/behavior* come from the group's first member
    std::map<int64_t, std::vector<const EffectDbEffect*>> byEffectId;
    for (const auto& e : dbEffects)
        byEffectId[e.effect_id].push_back(&e);

    for (const auto& [effectId, members] : byEffectId)
    {
        const EffectDbEffect& rep = *members.front();  //. representative -- see comment above

        nlohmann::ordered_json node;
        node["name"]            = rep.name;
        node["description"]     = rep.description;
        node["__vfxd_db_effect"] = true;   //. NOT the old __vfxd_db_only shape
        node["effect_id"]        = effectId;
        node["in_json"]          = std::any_of(members.begin(), members.end(),
                                                [&installedGuids](const EffectDbEffect* m)
                                                { return installedGuids.count(m->guid_b64) != 0; });

        nlohmann::ordered_json guids = nlohmann::ordered_json::array();
        for (const auto* m : members)
            guids.push_back(m->guid_b64);
        node["guids"] = std::move(guids);

        //_ Same "__vfxd_db_by_guid" shape RenderEffectDbDetail already reads.
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
        //_ Resolved before byGuid moves into node; empty categoryPath uses occurrences
        nlohmann::ordered_json* dest = nullptr;
        if (!rep.categoryPath.empty())
        {
            dest = FindOrCreateDbCategory(root, rep.categoryPath, sortOrderByKey);
        }
        else
        {
            const std::string classFolder = ClassFolderNameFromByGuidJson(byGuid);
            const std::string typeFolder  = "type " + std::to_string(rep.type);
            dest = FindOrCreateUncategorizedBucket(root, classFolder, typeFolder);
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

    SortTreeRecursive(root);
    return root;
}