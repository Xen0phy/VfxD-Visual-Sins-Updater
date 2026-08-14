#include "db_tree_view.h"

#include "game_state.h"          //. GameState_ProfessionName
#include "specialization_info.h" //. SpecializationProfession

#include <algorithm>
#include <map>
#include <set>
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
// JoinKey
//--------------------------------------------------------------------------------
// Local map key only -- doesn't need to match effect_db.cpp's private
// category-path delimiter, since this is never written to SQL, only used
// to look an already-split EffectDbCategory::categoryPath up by value.
// Same shape as sin_generator.cpp's own JoinKey, deliberately not shared
// with it (different translation unit, trivial enough not to warrant a
// shared header).
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
// Same shape as installed_tree_overlay.cpp's FindOrCreateDiffCategory --
// deliberately not shared with it, since that function lives in a "dead
// code once the DB tab ships" file (see the handoff) and this one is
// meant to outlive it. Tags every materialized category
// "__vfxd_virtual" so RenderCategoryTree's existing gate suppresses
// JSON-only editing affordances (drag, delete, create, right-click) on
// it for free -- there is no JSON position for any DB tab category to
// have, ever, not just until it's promoted.
//
// Also stamps "__vfxd_sort_order" (the `categories` table's own
// sort_order, looked up by `sortOrderByKey` -- see BuildDbTree) on every
// node it creates, so SortTreeRecursive can mirror the curated JSON's
// authored category order instead of always alphabetizing -- omitted
// (not zeroed) when a segment isn't in the table, so SortTreeRecursive
// can tell "really unordered" apart from "curated position 0".
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
            newCat["__vfxd_virtual"] = true;   //. no JSON position, ever -- see comment above

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
// Level 1 of the Uncategorized sort (TODO_C_uncategorized_class_sort.md):
// union the professions implied by every occurrence of every guid on this
// node, read off the node's own "__vfxd_db_by_guid" -> "occurrences" ->
// "specialization_ids" JSON that BuildOccurrencesJson already built --
// deliberately NOT a second EffectDb_GetOccurrences(guid_b64) call per
// guid, since that data is already in hand by the time this runs.
// specialization_ids are already unpacked raw ids (1..127, see
// EffectDb_SpecOrCoreIdsInMask); resolved here to a profession via
// SpecializationProfession() for a real spec id or
// EffectDb_ProfessionFromCoreOnlyId() for a core-only pseudo-id
// (>= kEffectDbCoreOnlyIdFloor), same split DecodeSpecOrCoreId uses
// elsewhere. One profession -> that profession's folder; more than one
// (the same guid cast by more than one class over time) -> "Multiple",
// no per-combination buckets. An empty result shouldn't happen -- every
// guid reaching Uncategorized arrived via EffectDb_RecordEvent, which
// always writes a matching occurrence row alongside it -- but folds into
// "Multiple" rather than a third bucket if it ever does.
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
// One-segment version of FindOrCreateDbCategory's materialize-if-missing
// logic -- find `name` among `parent`'s "categories", or create it (tagged
// "__vfxd_virtual", same reasoning as FindOrCreateDbCategory) if absent.
// Kept separate from FindOrCreateDbCategory rather than reusing it: that
// function's contract is "split a category_path string", and the
// Uncategorized class/type folders below aren't derived from one -- see
// TODO_C_uncategorized_class_sort.md.
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
// Materializes Uncategorized/<class>/type N lazily, one segment at a time,
// via FindOrCreateVirtualChild -- so a class/type combination nothing
// lands in never appears as an empty folder, same rule the rest of the DB
// tab already follows. `classFolder` is one of the 9 profession names or
// "Multiple"; `typeFolder` is "type N" for effects.type (0-11), used
// directly with no aggregation -- type is a fixed per-guid attribute
// (confirmed it can only appear once per guid), never ambiguous the way
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
// Categories: mirrors the curated JSON's own authored order via each
// node's "__vfxd_sort_order" (stamped by FindOrCreateDbCategory from the
// `categories` table -- see BuildDbTree) when present -- ascending, same
// as SinGenerator_Generate's own effect ordering. A node without one
// (every Uncategorized/<class>/type N folder, since those are never
// curated -- see FindOrCreateUncategorizedBucket; also any real category
// segment the seed hasn't given a sort_order to yet) sorts after every
// node that has one, alphabetically among themselves -- same fallback
// the tree used exclusively before this. Effects: still always
// alphabetical by name -- sort_order isn't being extended to effects,
// only categories.
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
                return aHas;   //. the one with a curated position sorts first
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

nlohmann::ordered_json BuildDbTree(const std::vector<EffectDbEffect>& dbEffects,
                                    const std::unordered_set<std::string>& installedGuids)
{
    nlohmann::ordered_json root;
    root["categories"] = nlohmann::ordered_json::array();
    root["effects"]    = nlohmann::ordered_json::array();

    //_ Categories table's own sort_order, keyed by joined path -- see
    // FindOrCreateDbCategory's use of it and SortTreeRecursive's
    // "mirror the curated JSON order" comment. Same EffectDb_GetAllCategories
    // call SinGenerator_Generate already makes for the same table; not on
    // a hot path here either (once per tree rebuild, not per frame).
    std::map<std::string, int> sortOrderByKey;
    for (const auto& c : EffectDb_GetAllCategories())
        sortOrderByKey[JoinKey(c.categoryPath)] = c.sortOrder;

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
        //_ Destination resolved from `byGuid` before it's moved into
        // `node`, not from categoryPath alone: an empty categoryPath's
        // destination (Uncategorized's class/type folders) is derived
        // from the occurrences data just assembled above -- see
        // ClassFolderNameFromByGuidJson. A real categoryPath still goes
        // through FindOrCreateDbCategory exactly as before.
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