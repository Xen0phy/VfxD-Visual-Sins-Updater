//################################################################################
// installed_tree_view.cpp
//--------------------------------------------------------------------------------
// RenderInstalledEffects(dir)   draws the whole Installed Effects section
//--------------------------------------------------------------------------------
// Extracted from addon.cpp: RenderCategoryTree (the recursive tree renderer),
// RenderInstalledEffects (the section wrapper), and their supporting leaf
// renderers (GuidList/GuidDiff/JsonValue/Behavior/ConflictSources/GroupInfo/
// EffectDbDetail, among others) are all file-local -- RenderInstalledEffects
// is the only symbol this module exposes. Reaches the editing/store/overlay/
// search modules only through their own accessor headers, never through
// another module's statics.
//
// Three things are cached rather than rebuilt every frame, since doing so
// caused a reported scrolling stall on a large tree with an overlay open:
// s_overlayCache (one built, duplicate-guid-/pending-diff-/Greed-effect-db-
// tagged copy of each sin file's tree, rebuilt only when its generation or
// diff status changes -- NOT on effect-db generation alone, see "Deliberately
// not reacting" below), s_searchCache (per-sin search-match results, see
// SearchCacheEntry), and the search box's forced-open/forced-closed TreeNode
// state (gated by s_treeSearchQueryChanged, true for exactly one frame per
// query change; s_treeSearchQueryLower is the lowercased query every
// match/filter helper below compares against).
//
// Drag-and-drop is reorder-only: an effect or category can move among its
// current siblings, never to a different parent or sin file. Nothing that
// only exists in a pending-update overlay (__vfxd_new/__vfxd_rework/
// __vfxd_virtual) offers drag, edit, or delete -- there's no stable real
// on-disk position/identity for it yet. Any edit/rename/delete/create state
// scoped under a node that stops being drawn this frame (hidden by search,
// or collapsed) is cancelled immediately rather than left running invisibly.
//
// Two exceptions to "reorder-only", neither of which moves the node itself:
//
// - A single-GUID drag: a GUID's own bullet row, in the plain read-only
//   view only, is a drag source (see the GuidListDragContext-driven branch
//   of RenderGuidList below) -- not from inside the effect editor, which
//   is back to a single "one GUID per line" textbox (see RenderEffectEditor
//   in installed_tree_edit.cpp). Any effect row in the same sin file is a
//   valid target (except the one open for editing); writes straight to
//   disk on drop -- see QueueGuidMerge.
//
// - A DB tab node's own row (effIsDbEffect) is a drag source too, despite
//   being "__vfxd_virtual" in category terms: dragging it onto any
//   category row within the DB tab sets its effect_meta.category_path
//   via QueueDbEffectCategoryPlacement. What moves is a real database
//   row, not the rendered node -- see db_tree_view.h.
//--------------------------------------------------------------------------------

#include "db_tree_view.h"
#include "effect_db.h"
#include "github_update.h"
#include "imgui.h"
#include "installed_tree_edit.h"
#include "installed_tree_overlay.h"
#include "installed_tree_search.h"
#include "installed_tree_store.h"
#include "installed_tree_view.h"
#include "specialization_info.h"
#include "sql_update.h"
#include "ui_colors.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

//********************************************************************************
// GuidListDragContext
//--------------------------------------------------------------------------------
// sinName/path/index   the owning effect's identity (see GuidDragPayload)
// effectName            owning effect's display name, for messages
//--------------------------------------------------------------------------------
// Passed to RenderGuidList to make its rows draggable straight from the
// read-only tree (no need to open the effect's editor first) -- nullptr
// (the default) keeps the plain BulletText rendering RenderGuidDiff's
// added/removed/unchanged buckets use, where dragging wouldn't make
// sense (an "Added" bucket's GUIDs aren't actually on this effect yet).
//--------------------------------------------------------------------------------
struct GuidListDragContext
{
    std::string       sinName;
    std::vector<int>  path;
    int               index = -1;
    std::string       effectName;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderGuidList
//--------------------------------------------------------------------------------
// `color` is optional -- nullptr for the default text color (a plain guids
// list), or a color to tint every bullet (e.g. kReworkColor, to set a
// reworked effect's post-update GUIDs apart from its current ones).
// `dragContext` is optional -- see GuidListDragContext. When set, each row
// is a "VFXD_GUID" drag source (see QueueGuidMerge) instead of a plain
// bullet; gated on !AnyEditInFlight() so it can't start a new drag while
// some other edit is already open elsewhere.
//--------------------------------------------------------------------------------
void RenderGuidList(const char* label, const std::vector<std::string>& guids, const ImVec4* color = nullptr,
                     const GuidListDragContext* dragContext = nullptr)
{
    if (guids.empty())
    {
        ImGui::TextDisabled("%s: (none)", label);
        return;
    }

    ImGui::TextDisabled("%s:", label);
    ImGui::Indent();
    if (color)
        ImGui::PushStyleColor(ImGuiCol_Text, *color);

    if (!dragContext)
    {
        for (const auto& g : guids)
            ImGui::BulletText("%s", g.c_str());
    }
    else
    {
        bool dragAllowed = !AnyEditInFlight();
        for (size_t i = 0; i < guids.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BulletText("%s", guids[i].c_str());

            //_ BulletText registers no ImGui ID, so BeginDragDropSource needs
            // ImGuiDragDropFlags_SourceAllowNullID or it asserts -- including on a
            // node's very first open frame, since the tree arrow toggles on mouse-DOWN.
            if (dragAllowed && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                BeginGuidDrag(dragContext->sinName, dragContext->path, dragContext->index,
                              dragContext->effectName, guids[i]);
                ImGui::SetDragDropPayload("VFXD_GUID", &kGuidDragMarker, sizeof(kGuidDragMarker));
                ImGui::Text("Move GUID \"%s\"", guids[i].c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
    }

    if (color)
        ImGui::PopStyleColor();
    ImGui::Unindent();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderGuidDiff
//--------------------------------------------------------------------------------
// Shows only what a reworked effect's guid list would actually change to,
// rather than the full current and post-update lists side by side -- a 1c
// merge can fold in dozens of untouched guids (see BuildMergedRework in
// merge.cpp), so printing both in full mostly repeats the same lines.
// Guids are an unordered identity set (see GuidDiff in merge.cpp), so this
// is a set difference, not a positional diff; unchanged guids still get
// listed, only added/removed ones get their own highlighted section.
//--------------------------------------------------------------------------------
void RenderGuidDiff(const std::vector<std::string>& oldGuids, const std::vector<std::string>& newGuids)
{
    std::unordered_set<std::string> oldSet(oldGuids.begin(), oldGuids.end());
    std::unordered_set<std::string> newSet(newGuids.begin(), newGuids.end());

    std::vector<std::string> added, removed, unchanged;
    for (const auto& g : newGuids)
        (oldSet.count(g) ? unchanged : added).push_back(g);
    for (const auto& g : oldGuids)
        if (!newSet.count(g))
            removed.push_back(g);

    if (added.empty() && removed.empty())
    {
        RenderGuidList("GUIDs", unchanged);   //. nothing changed -- plain list
        return;
    }

    if (!unchanged.empty())
        RenderGuidList("GUIDs", unchanged);

    if (!added.empty())
        RenderGuidList(added.size() == 1 ? "Added GUID" : "Added GUIDs", added, &kNewColor);

    if (!removed.empty())
        RenderGuidList(removed.size() == 1 ? "Removed GUID" : "Removed GUIDs", removed, &kDuplicateColor);
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderJsonValue
//--------------------------------------------------------------------------------
// Prints one key/value pair outside the confirmed effect/category schema
// (name/description/guids/behaviors) -- a forward-compat fallback, e.g. for
// a field a future VfxDenoiser version adds, rendered generically by JSON
// type so it shows up as *something* rather than silently vanishing.
//--------------------------------------------------------------------------------
void RenderJsonValue(const std::string& key, const nlohmann::ordered_json& value)
{
    switch (value.type())
    {
        case nlohmann::ordered_json::value_t::string:
            ImGui::BulletText("%s: %s", key.c_str(), value.get<std::string>().c_str());
            break;
        case nlohmann::ordered_json::value_t::boolean:
            ImGui::BulletText("%s: %s", key.c_str(), value.get<bool>() ? "true" : "false");
            break;
        case nlohmann::ordered_json::value_t::number_integer:
        case nlohmann::ordered_json::value_t::number_unsigned:
            ImGui::BulletText("%s: %lld", key.c_str(), static_cast<long long>(value.get<int64_t>()));
            break;
        case nlohmann::ordered_json::value_t::number_float:
            ImGui::BulletText("%s: %g", key.c_str(), value.get<double>());
            break;
        case nlohmann::ordered_json::value_t::null:
            ImGui::BulletText("%s: (null)", key.c_str());
            break;
        case nlohmann::ordered_json::value_t::array:
        case nlohmann::ordered_json::value_t::object:
        default:
            //_ Unknown shape -- dump compactly rather than guess a
            // schema-specific rendering for a nested object/array.
            ImGui::Bullet();
            ImGui::TextWrapped("%s: %s", key.c_str(), value.dump().c_str());
            break;
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderBehavior
//--------------------------------------------------------------------------------
// Renders one entry of an effect's "behaviors" array. Confirmed real shape
// (sample Collection.json): type Hide/Show/SetDuration, caster Self/
// Others/All, plus a "duration" (ms, per VfxDenoiser's README) only when
// type is SetDuration.
//--------------------------------------------------------------------------------
void RenderBehavior(const nlohmann::ordered_json& behavior)
{
    std::string type   = behavior.value("type", std::string("?"));
    std::string caster = behavior.value("caster", std::string("?"));

    if (type == "SetDuration" && behavior.contains("duration") && behavior["duration"].is_number())
    {
        ImGui::BulletText("Set duration: %gms for %s", behavior["duration"].get<double>(), caster.c_str());
    }
    else
    {
        ImGui::BulletText("%s for %s", type.c_str(), caster.c_str());
    }

    //_ Anything beyond type/caster/duration is unexpected -- surface it
    // rather than silently dropping it.
    for (const auto& [key, value] : behavior.items())
    {
        if (key == "type" || key == "caster" || key == "duration")
            continue;
        ImGui::Indent();
        RenderJsonValue(key, value);
        ImGui::Unindent();
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderConflictSources
//--------------------------------------------------------------------------------
// Renders the discarded/disagreeing settings recorded on a merge conflict
// (see MergePlanMergeCandidate in merge.h and "__vfxd_conflict_sources" in
// installed_tree_overlay.cpp) -- one block per other matched candidate,
// naming which effect it came from and its own behaviors. This is what
// "review before applying" is asking the user to look at, shown right
// where the warning already is.
//--------------------------------------------------------------------------------
void RenderConflictSources(const nlohmann::ordered_json& effect)
{
    if (!effect.contains("__vfxd_conflict_sources") || !effect["__vfxd_conflict_sources"].is_array())
        return;

    for (const auto& src : effect["__vfxd_conflict_sources"])
    {
        std::string name     = src.value("name", std::string("(unnamed effect)"));
        std::string category = src.value("category", std::string());

        if (category.empty())
            ImGui::TextColored(kDuplicateColor, "From \"%s\":", name.c_str());
        else
            ImGui::TextColored(kDuplicateColor, "From \"%s\" (in %s):", name.c_str(), category.c_str());

        ImGui::Indent();
        if (src.contains("behaviors") && src["behaviors"].is_array() && !src["behaviors"].empty())
        {
            for (const auto& behavior : src["behaviors"])
                RenderBehavior(behavior);
        }
        else
        {
            ImGui::TextDisabled("(no behaviors configured)");
        }
        ImGui::Unindent();
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GroupMemberLabel
//--------------------------------------------------------------------------------
// A group_members row only ever stores a guid -- resolve it to the db's
// own name for display, same "prefer the db name, fall back to the raw
// guid" convention RenderLiveLogSection's for-science branch already
// uses. Every guid reaching this table has an EFFECTS row by the FK
// itself (member_guid_b64 REFERENCES effects(guid_b64)), so the only
// reason EffectDb_GetEffect would fail here is the guid never having
// been renamed -- name is then "", and this still falls back correctly.
//--------------------------------------------------------------------------------
std::string GroupMemberLabel(const std::string& guid_b64)
{
    EffectDbEffect eff{};
    if (EffectDb_GetEffect(guid_b64, eff) && !eff.name.empty())
        return eff.name;
    return guid_b64;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderGroupInfo
//--------------------------------------------------------------------------------
// The "group info" section shown here, in RenderEffectDbDetail, and
// (mirrored, reading straight from the db) live_log_ui.cpp's
// RenderForScienceDetail -- see effect_db.h's group_members doc comment
// for what this table does and doesn't let you reconstruct. `detail` is
// one guid's "__vfxd_db_by_guid" entry; this function only renders, never
// queries the db itself.
//
// Two separate lists, not merged: "started" is every (duration, a4)
// instance this guid opened; "member_of" is every OTHER guid's group it
// got swept into. A guid can appear in both.
//--------------------------------------------------------------------------------
void RenderGroupInfo(const nlohmann::ordered_json& detail)
{
    if (!detail.contains("groups") || !detail["groups"].is_object())
        return;

    const auto& groups = detail["groups"];

    bool hasStarted   = groups.contains("started")   && groups["started"].is_array()   && !groups["started"].empty();
    bool hasMemberOf  = groups.contains("member_of") && groups["member_of"].is_array() && !groups["member_of"].empty();
    //_ Never opened or been swept into a group -- nothing worth a section for.
    if (!hasStarted && !hasMemberOf)
        return;

    if (ImGui::TreeNode("groupinfo", "Group info"))
    {
        if (hasStarted)
        {
            ImGui::TextDisabled("Groups started by this effect:");
            int idx = 0;
            for (const auto& inst : groups["started"])
            {
                int          duration = inst.value("duration", 0);
                unsigned int a4       = inst.value("a4", 0u);

                ImGui::PushID(idx++);
                if (ImGui::TreeNode("startedgroup", "duration:%d  a4:%u  (%d member%s)",
                                     duration, a4, static_cast<int>(inst.value("members", nlohmann::ordered_json::array()).size()),
                                     inst.value("members", nlohmann::ordered_json::array()).size() == 1 ? "" : "s"))
                {
                    if (inst.contains("members") && inst["members"].is_array())
                        for (const auto& m : inst["members"])
                            ImGui::BulletText("%s", GroupMemberLabel(m.get<std::string>()).c_str());
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }

        if (hasMemberOf)
        {
            ImGui::TextDisabled("Also seen as a member of other groups:");
            for (const auto& mo : groups["member_of"])
            {
                std::string starter  = mo.value("starter_guid_b64", std::string());
                int         duration = mo.value("duration", 0);
                unsigned int a4      = mo.value("a4", 0u);
                ImGui::BulletText("%s  (duration:%d  a4:%u)", GroupMemberLabel(starter).c_str(), duration, a4);
            }
        }

        ImGui::TreePop();
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// DecodeSpecOrCoreId
//--------------------------------------------------------------------------------
// One raw id from EffectDb_SpecOrCoreIdsInMask (1..127, see effect_db.h's
// EffectDbSpecializationMask) -> the profession display name and
// spec/core-build label to bucket it under in the tree. A reserved
// pseudo-id (>= kEffectDbCoreOnlyIdFloor) decodes straight to its
// profession via EffectDb_ProfessionFromCoreOnlyId, with no real spec
// attached (core build, no elite spec active); anything below that decodes
// through specialization_info.h, falling back to a raw "Spec #N" label if
// the id isn't in that table yet (mirrors GetSpecializationInfo's own
// "don't guess" contract -- see that header).
//--------------------------------------------------------------------------------
void DecodeSpecOrCoreId(unsigned int id, std::string& outProfName, std::string& outSpecLabel)
{
    if (id >= kEffectDbCoreOnlyIdFloor)
    {
        outProfName  = GameState_ProfessionName(EffectDb_ProfessionFromCoreOnlyId(id));
        outSpecLabel = "(core build)";
        return;
    }

    const SpecializationInfo info = GetSpecializationInfo(id);
    outProfName  = GameState_ProfessionName(info.profession);
    outSpecLabel = info.name ? std::string(info.name) : ("Spec #" + std::to_string(id));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ClassesSeenLabel
//--------------------------------------------------------------------------------
// A guid's captured occurrences (EffectDb_GetOccurrences), collapsed down
// to just the set of profession names that have ever triggered it --
// specialization-level detail is what RenderEffectDbDetail's full
// breakdown is for; this is a one-line "which classes" summary for
// RenderDbTabGroupsView, where the point is predicting a starter's likely
// category from its members at a glance, not a full occurrence dump.
// std::set for dedupe + stable alphabetical order (profession appears once
// no matter how many specs/occurrences produced it). Empty return means
// no self-caused occurrence has been captured for this guid yet -- for a
// group member specifically, that's expected whenever it was only ever
// seen as *target* of the starter's effect, never caster itself (see
// EffectDbOccurrence's self_mask -- this reads every mask, not just
// caster, so it still surfaces those too).
//--------------------------------------------------------------------------------
std::string ClassesSeenLabel(const std::string& guid_b64)
{
    std::set<std::string> professions;
    for (const auto& occ : EffectDb_GetOccurrences(guid_b64))
        for (unsigned int id : EffectDb_SpecOrCoreIdsInMask(occ.specializationMask))
        {
            std::string profName, specLabel;
            DecodeSpecOrCoreId(id, profName, specLabel);
            professions.insert(profName);
        }

    if (professions.empty())
        return "";

    std::string label;
    for (const auto& p : professions)
    {
        if (!label.empty())
            label += ", ";
        label += p;
    }
    return label;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderEffectDbDetail
//--------------------------------------------------------------------------------
// The "for science" expanded view -- full capture detail for a DB tab
// node. Reads "__vfxd_db_by_guid" (guid -> {block_group, block_member,
// type, occurrences, groups}), embedded per rebuild by db_tree_view.cpp's
// BuildDbTree -- usually one guid; a merged effect gets one header+group
// per guid. Used only from the DB tab now; the JSON tab no longer carries
// this data at all -- see EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md.
//
// Occurrences group as duration/a4/a6/self_mask -> profession ->
// specialization, races as siblings (see EffectDbSpecializationMask).
// "groups" renders separately, via RenderGroupInfo.
//--------------------------------------------------------------------------------
void RenderEffectDbDetail(const nlohmann::ordered_json& effect)
{
    if (!effect.contains("__vfxd_db_by_guid") || !effect["__vfxd_db_by_guid"].is_object()
        || effect["__vfxd_db_by_guid"].empty())
    {
        ImGui::TextDisabled("No data.");
        return;
    }

    static const char* kSelfMaskLabels[] = { "none", "target", "caster", "both" };

    //_ Almost always exactly one guid -- a merged effect can have
    // more than one entry if capture data exists for more than one
    // of its guids separately (see BuildDbTree).
    int guidIdx = 0;
    for (const auto& [guid, detail] : effect["__vfxd_db_by_guid"].items())
    {
        if (guidIdx > 0)
            ImGui::Separator();
        ImGui::PushID(guidIdx++);

        std::string blockGroup  = detail.value("block_group", std::string());
        std::string blockMember = detail.value("block_member", std::string());
        int         type        = detail.value("type", 0);

        if (!blockGroup.empty() || !blockMember.empty())
            ImGui::Text("Block: %s.%s   Type: %d", blockGroup.c_str(), blockMember.c_str(), type);
        else
            ImGui::TextDisabled("Block: (none on this line)   Type: %d", type);
        ImGui::TextDisabled("GUID: %s", guid.c_str());

        if (!detail.contains("occurrences") || !detail["occurrences"].is_array() || detail["occurrences"].empty())
        {
            ImGui::TextDisabled("No occurrences recorded yet.");
            //_ Still worth checking for group data below even with no
            // occurrences -- doesn't happen in practice today (see
            // EffectDb_RecordEvent), but costs nothing if empty.
            RenderGroupInfo(detail);
            ImGui::PopID();
            continue;
        }

        //_ (duration, a4, a6, self_mask) -> { profession -> specs seen,
        // races seen } -- siblings, not nested (see this function's doc
        // comment). std::map keeps TreeNode open state stable across frames.
        struct SignatureGroup
        {
            std::map<std::string, std::set<std::string>> specsByProfession;
            std::set<std::string> racesSeen;
        };
        std::map<std::tuple<int, unsigned int, std::string, int>, SignatureGroup> groups;

        for (const auto& occ : detail["occurrences"])
        {
            int          duration = occ.value("duration", 0);
            unsigned int a4       = occ.value("a4", 0u);
            std::string  a6       = occ.value("a6", std::string());
            int          selfMask = occ.value("self_mask", 0);

            auto raceMask = static_cast<EffectDbRaceMask>(occ.value("race_mask", 0u));
            auto specIds  = occ.value("specialization_ids", std::vector<unsigned int>());

            SignatureGroup& group = groups[{ duration, a4, a6, selfMask }];

            for (unsigned int id : specIds)
            {
                std::string profName, specLabel;
                DecodeSpecOrCoreId(id, profName, specLabel);
                group.specsByProfession[profName].insert(specLabel);
            }
            for (Mumble::ERace race : EffectDb_RacesInMask(raceMask))
                group.racesSeen.insert(GameState_RaceName(race));
        }

        //_ Loop index, not a pointer/address, for PushID below -- `groups`
        // is rebuilt fresh every frame, so an address-based ID would
        // reset every nested TreeNode's open state next frame; index stays stable.
        int groupIdx = 0;
        for (const auto& [sig, group] : groups)
        {
            const auto& [duration, a4, a6, selfMask] = sig;
            const char* selfLabel = (selfMask >= 0 && selfMask <= 3) ? kSelfMaskLabels[selfMask] : "?";

            ImGui::PushID(groupIdx++);
            if (ImGui::TreeNode("occgroup", "duration:%d  a4:%u  a6:%s  self:%s",
                                 duration, a4, a6.empty() ? "null" : a6.c_str(), selfLabel))
            {
                for (const auto& [profName, specs] : group.specsByProfession)
                {
                    if (ImGui::TreeNode(profName.c_str(), "%s", profName.c_str()))
                    {
                        for (const auto& specLabel : specs)
                            ImGui::BulletText("%s", specLabel.c_str());
                        ImGui::TreePop();
                    }
                }

                std::string raceList;
                for (const auto& r : group.racesSeen)
                {
                    if (!raceList.empty()) raceList += ", ";
                    raceList += r;
                }
                ImGui::TextDisabled("Races seen: %s", raceList.empty() ? "(none)" : raceList.c_str());

                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        RenderGroupInfo(detail);

        ImGui::PopID();
    }
}

//********************************************************************************
// OverlayCacheEntry
//--------------------------------------------------------------------------------
// generation           tree generation this copy was built from
// diffStatus           diff status this copy was built from
// file                 the built (dupe/diff-tagged) copy of the
//                      installed tree
// contentVersion       bumped whenever `file` is rebuilt -- lets the search
//                      cache tell a real rebuild apart from a cache-hit frame
//--------------------------------------------------------------------------------
// Per-sin cache entry backing s_overlayCache -- see the file header for why
// this cache exists. Invalidated on GetInstalledTreeGeneration() changing
// (file reloaded/edited) or the sin's own EDiffStatus changing (a diff
// produces one MergePlan per Ready transition; a reload always passes
// through NotLoaded/Loading first, which this also catches).
//--------------------------------------------------------------------------------
struct OverlayCacheEntry
{
    int         generation = -1;
    EDiffStatus diffStatus = EDiffStatus::NotLoaded;
    //_ Which diff source diffStatus/file were built from -- 0 = none,
    // 1 = GitHub (github_update.h's GetSinDiffInfo), 2 = SQL
    // (sql_update.h's GetSqlDiffInfo). diffStatus alone can't tell a
    // switch from one source's Ready plan to the other's Ready plan for
    // the same sin apart from "no change" (both read as Ready), which
    // would leave the tree showing a stale plan from whichever source
    // loaded first -- found once SQL gave every sin a second diff source
    // that could independently go Ready. See RenderJsonTabContent's own
    // "SQL wins the tie" comment for the pointer-selection half of this.
    int         diffSource = 0;
    nlohmann::ordered_json file;
    int contentVersion = 0;
};
//_ Per-sin cache of built overlay trees, keyed by sin name.
std::unordered_map<std::string, OverlayCacheEntry> s_overlayCache;

//********************************************************************************
// SearchCacheEntry
//--------------------------------------------------------------------------------
// query            lowercased search query this cache was last built for
// treeVersion      tree-content version this cache was last built for (see
//                  fileTreeVersion below)
// categoryCache    per-category subtree-match results (see
//                  CategoryMatchCache in installed_tree_search.h)
// effectCache      per-effect match results (see EffectMatchCache)
// anyMatchInFile   whether any category/effect in this sin matched at all
//--------------------------------------------------------------------------------
// Per-sin CategoryMatchCache/EffectMatchCache (see installed_tree_search.h),
// persisted across frames instead of rebuilt every frame the search box is
// non-empty -- rebuilding meant a full ContainsCI scan (allocating +
// lowercasing a string per name/description/GUID) over every effect,
// every frame, on a ~20k-effect tree -- the same stall noted in the file
// header. `query`/`treeVersion` gate the rebuild (see `searchStale`
// below); unchanged, a lookup costs one hashmap read per sin instead of
// an O(effect count) rescan.
//--------------------------------------------------------------------------------
struct SearchCacheEntry
{
    std::string        query;
    long long           treeVersion = -1;   //. see fileTreeVersion below
    CategoryMatchCache  categoryCache;
    EffectMatchCache    effectCache;
    bool                anyMatchInFile = false;
};
//_ Per-sin cache of search-match results, keyed by sin name.
std::unordered_map<std::string, SearchCacheEntry> s_searchCache;

//_ Raw ImGui input buffer for the installed-tree search box.
char        s_treeSearchBuf[256] = {};
//_ Lowercased from s_treeSearchBuf once per frame; every match/filter
// helper below compares against this, not the raw buffer.
std::string s_treeSearchQueryLower;

//_ Search starts only once this many characters are typed -- a shorter
// query matches too much to be useful and costs a needless per-keystroke
// tree walk.
constexpr size_t kMinTreeSearchLength = 3;

//_ True for exactly one frame, when s_treeSearchQueryLower just changed
// (see file header for why this gates forced-open/closed calls).
bool s_treeSearchQueryChanged = false;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderCategoryTree
//--------------------------------------------------------------------------------
// Recursively renders one category node -- a TreeNode per category, with
// effects and subcategories nested underneath, mirroring the JSON shape
// merge.cpp walks. `pathSoFar`/`myIndex`/`sinName` key edit-state lookup
// (see EditState::originalIndex); caller must PushID a stable per-sibling
// key first. `namePathSoFar` mirrors `pathSoFar` by name -- the shape
// EffectDb_SetCategoryPath keys placement by, since it survives reordering
// (used by the db-only drag target below). `forceShow` is true once an
// ancestor already matched the search box. `matchCache`/`effectMatchCache`
// (rebuilt fresh per frame, see BuildCategoryMatchCache) turn what would be
// an O(size) walk per node into an O(1) lookup here.
//--------------------------------------------------------------------------------
void RenderCategoryTree(const std::string& sinName, const nlohmann::ordered_json& category,
                         std::vector<int>& pathSoFar, std::vector<std::string>& namePathSoFar,
                         int myIndex, const CategoryMatchCache& matchCache, const EffectMatchCache& effectMatchCache,
                         bool forceShow = false)
{
    std::string name = category.value("name", std::string("(unnamed category)"));
    pathSoFar.push_back(myIndex);
    namePathSoFar.push_back(name);

    bool searchActive = !s_treeSearchQueryLower.empty();

    //_ An unmatched subtree isn't drawn at all (not even collapsed). Cancel
    // any edit scoped under here first -- nothing should keep running
    // invisibly (see file header).
    if (searchActive && !forceShow && !CachedSubtreeMatches(category, s_treeSearchQueryLower, matchCache))
    {
        if (IsCategoryRenameUnderPath(sinName, pathSoFar))
            CancelCategoryEdit();
        if (IsEffectEditUnderPath(sinName, pathSoFar))
            CancelEdit();
        if (IsCategoryCreateUnderPath(sinName, pathSoFar))
            CancelCreateCategory();
        if (IsDeleteConfirmUnderPath(sinName, pathSoFar))
            CancelDeleteConfirm();

        //_ Not visited this frame -- on a query change, reset any
        // force-opened state an earlier query left here (see
        // SilentlyCloseSubtree's own comment).
        if (s_treeSearchQueryChanged)
            SilentlyCloseSubtree(category);

        pathSoFar.pop_back();
        namePathSoFar.pop_back();
        return;
    }

    //_ Whether THIS category matched directly, vs. only containing a match
    // below -- decides whether its children get filtered individually or
    // shown in full.
    bool categoryMatchesDirectly = !searchActive || forceShow ||
        ContainsCI(name, s_treeSearchQueryLower) ||
        ContainsCI(category.value("description", std::string()), s_treeSearchQueryLower);

    //_ Forced open only if collapsing would hide a match not already
    // visible on the row itself -- own description, or a descendant match
    // (see function header for the query-change gating).
    if (s_treeSearchQueryChanged)
    {
        bool categoryNeedsForceOpen = searchActive &&
            (CategoryDescriptionMatches(category, s_treeSearchQueryLower) ||
             CachedHasDescendantMatch(category, s_treeSearchQueryLower, matchCache));
        ImGui::SetNextItemOpen(categoryNeedsForceOpen, ImGuiCond_Always);
    }

    bool isRenamingThis = IsCategoryBeingRenamed(sinName, pathSoFar);
    bool isDeletingThisCategory = IsDeletingThisCategory(sinName, pathSoFar);
    bool isCreatingHere = IsCreatingCategoryAt(sinName, pathSoFar);

    bool categoryHasDupe     = category.value("__vfxd_hasdupe", false);
    bool categoryHasConflict = category.value("__vfxd_hasconflict", false);
    bool categoryHasRework   = category.value("__vfxd_hasrework", false);
    bool categoryHasNew      = category.value("__vfxd_hasnew", false);
    bool categoryVirtual     = category.value("__vfxd_virtual", false);

    //_ Priority: dupe/conflict (red) needs a second look before trusting
    // which effect is which; failing that, rework (orange) outranks new
    // (green) as the thing worth double-checking.
    const ImVec4* categoryTint = nullptr;
    if (categoryHasDupe || categoryHasConflict)
        categoryTint = &kDuplicateColor;
    else if (categoryHasRework)
        categoryTint = &kReworkColor;
    else if (categoryHasNew)
        categoryTint = &kNewColor;

    if (categoryTint)
        ImGui::PushStyleColor(ImGuiCol_Text, *categoryTint);
    bool categoryOpen = ImGui::TreeNode(name.c_str());
    if (categoryTint)
        ImGui::PopStyleColor();

    //_ Closed on the query-change frame -> descendants won't be visited to
    // reset their own force-open state, so do it here instead (same
    // reasoning as the search-skip branch above).
    if (!categoryOpen && s_treeSearchQueryChanged)
    {
        //_ TreeNode only pushes its ID scope when open; enter it manually
        // here to match the IDs the real render pass would use.
        ImGui::PushID(name.c_str());
        SilentlyCloseChildren(category);
        ImGui::PopID();
    }

    //_ Drop target for an effect dragged from elsewhere, a category
    // reorder, or a db-only node's placement -- attaches to the row so
    // it works whether open or collapsed; db-only accepts skip the !categoryVirtual gate.
    if (ImGui::BeginDragDropTarget())
    {
        if (!categoryVirtual && ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
        {
            //_ Real payload lives in GetEffectDragPayload(), not the drop bytes
            // (see EffectDragPayload). Guard the sin match (same-sin moves only);
            // skip if already last in this category to avoid a pointless no-op rewrite+.bak.
            const EffectDragPayload& dragPayload = GetEffectDragPayload();
            bool sameCategory = dragPayload.originalPath == pathSoFar;
            bool alreadyLast  = sameCategory && category.contains("effects") && category["effects"].is_array() &&
                                dragPayload.originalIndex == static_cast<int>(category["effects"].size()) - 1;
            if (dragPayload.sinName == sinName && !alreadyLast)
            {
                EffectMoveJob job;
                job.sinName          = dragPayload.sinName;
                job.originalPath     = dragPayload.originalPath;
                job.effectName       = dragPayload.effectName;
                job.originalIndex    = dragPayload.originalIndex;
                job.destinationPath  = pathSoFar;
                job.destinationIndex = -1; // append -- see EffectMoveJob's comment

                QueueEffectMove(std::move(job));
            }
        }

        if (!categoryVirtual && ImGui::AcceptDragDropPayload("VFXD_CATEGORY"))
        {
            //_ Reorder-only (see file header): dropped here means "append to
            // my children" if I'm the dragged category's parent, or "insert
            // above me" if we share a parent; a different parent would be reparenting -- ignored.
            if (GetCategoryDragSinName() == sinName && !GetCategoryDragPath().empty())
            {
                const std::vector<int>& dragPath = GetCategoryDragPath();
                std::vector<int> draggedParentPath(dragPath.begin(), dragPath.end() - 1);
                int              draggedIndex = dragPath.back();

                std::vector<int> myParentPath = pathSoFar;
                myParentPath.pop_back();

                if (pathSoFar == draggedParentPath)
                {
                    //_ Same no-op check as the effect target's append case.
                    bool alreadyLast = category.contains("categories") && category["categories"].is_array() &&
                                       draggedIndex == static_cast<int>(category["categories"].size()) - 1;
                    if (!alreadyLast)
                        QueueCategoryMove(GetCategoryDragSinName(), dragPath, -1);
                }
                else if (myParentPath == draggedParentPath)
                {
                    //_ Insert above this category; skip dropping on itself
                    // or on the sibling right after it (post-erase-shift,
                    // that would land it right back where it started).
                    bool noOp = (draggedIndex == myIndex) || (draggedIndex == myIndex - 1);
                    if (!noOp)
                        QueueCategoryMove(GetCategoryDragSinName(), dragPath, myIndex);
                }
                //_ else: different parent = reparenting, not offered
            }
        }

        //_ DB tab's own category placement. Not gated on !categoryVirtual --
        // a virtual category here only exists because this drop makes it
        // real, scoped to the DB tab's sentinel sinName.
        if (sinName == kDbTabSinName && ImGui::AcceptDragDropPayload("VFXD_DB_EFFECT"))
        {
            const DbEffectDragPayload& dbEffectDragPayload = GetDbEffectDragPayload();
            QueueDbEffectCategoryPlacement(dbEffectDragPayload.effectId, dbEffectDragPayload.guids, namePathSoFar);
        }

        ImGui::EndDragDropTarget();
    }

    //_ Reorder among current siblings only (see file header). Gated on
    // AnyEditInFlight so a drag can't start mid-edit elsewhere.
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginDragDropSource())
    {
        BeginCategoryDrag(sinName, pathSoFar);
        ImGui::Text("Move \"%s\"", name.c_str());
        ImGui::EndDragDropSource();
    }

    //_ Only offered when no other edit is in flight anywhere, and never on
    // a "__vfxd_virtual" overlay category (see BuildDiffOverlayTree).
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginPopupContextItem("category_ctx"))
    {
        if (ImGui::MenuItem("Edit"))
            BeginCategoryEdit(sinName, pathSoFar, name, category.value("description", std::string()));
        ImGui::EndPopup();
    }

    //_ Rendered after the context menu above so they don't steal "last
    // item" from the TreeNode. Delete stays disabled while the category has
    // content; "+" only shows once open, since it adds *inside* what's shown.
    if (!categoryVirtual)
    {
        bool categoryEmpty = (!category.contains("effects") || category["effects"].empty()) &&
                              (!category.contains("categories") || category["categories"].empty());
        bool deleteDisabled = !categoryEmpty || (AnyEditInFlight() && !isDeletingThisCategory);

        ImGui::SameLine();
        if (deleteDisabled)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        bool deleteClicked = ImGui::SmallButton("-##delcat");
        if (deleteDisabled)
            ImGui::PopStyleVar();
        if (deleteClicked && !deleteDisabled)
            BeginDeleteConfirm(sinName, pathSoFar, -1, /*isCategory=*/true, name);

        if (categoryOpen)
        {
            bool createDisabled = AnyEditInFlight() && !isCreatingHere;
            ImGui::SameLine();
            if (createDisabled)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            bool createClicked = ImGui::SmallButton("+##addcat");
            if (createDisabled)
                ImGui::PopStyleVar();
            if (createClicked && !createDisabled)
                BeginCreateCategory(sinName, pathSoFar);
        }
    }

    //_ Sits right below this row, not nested inside the TreeNode's
    // collapsible content -- visible whether the node is open or collapsed.
    if (isDeletingThisCategory)
        RenderDeleteConfirm();

    if (categoryOpen)
    {
        if (isRenamingThis)
            RenderCategoryEditor();
        if (isCreatingHere)
            RenderCreateCategoryEditor();

        if (category.contains("description") && category["description"].is_string())
        {
            std::string desc = category["description"].get<std::string>();
            if (!desc.empty())
                ImGui::TextWrapped("%s", desc.c_str());
        }

        if (category.contains("effects") && category["effects"].is_array())
        {
            int i = 0;
            for (const auto& effect : category["effects"])
            {
                const int effIndex = i++;

                //_ Hidden by search -- not matched by category or effect.
                // Cancel any edit in flight (see file header) since it
                // won't be drawn at all this frame.
                if (searchActive && !categoryMatchesDirectly && !CachedEffectMatches(effect, s_treeSearchQueryLower, effectMatchCache))
                {
                    bool isEditingThisHidden = IsEffectBeingEdited(sinName, pathSoFar, effIndex);
                    if (isEditingThisHidden)
                        CancelEdit();

                    bool isDeletingThisHidden = IsDeletingThisEffect(sinName, pathSoFar, effIndex);
                    if (isDeletingThisHidden)
                        CancelDeleteConfirm();

                    //_ A DB tab node's rename is keyed by effect_id, not
                    // (sinName, path, index) -- see db_tree_view.h.
                    if (effect.value("__vfxd_db_effect", false) &&
                        IsDbEffectBeingRenamed(effect.value("effect_id", int64_t(0))))
                        CancelDbEffectRename();

                    //_ Not visited this frame -- on a query change, reset
                    // this effect's own stored open state too.
                    if (s_treeSearchQueryChanged)
                    {
                        ImGui::PushID(effIndex);
                        ImGui::GetStateStorage()->SetInt(ImGui::GetID("effect"), 0);
                        ImGui::PopID();
                    }

                    continue;
                }

                ImGui::PushID(effIndex);

                std::string effName = effect.value("name", std::string("(unnamed effect)"));
                //_ Identity is (sinName, path, index) -- NOT name. Siblings can
                // share a name (VfxDenoiser doesn't require uniqueness); matching
                // by name would make every same-named sibling think it was the one being edited.
                bool isEditingThis = IsEffectBeingEdited(sinName, pathSoFar, effIndex);
                bool isDeletingThisEffect = IsDeletingThisEffect(sinName, pathSoFar, effIndex);

                bool effIsDupe     = effect.value("__vfxd_dupe_guid", false);
                bool effIsNew      = effect.value("__vfxd_new", false);
                bool effIsRework   = effect.value("__vfxd_rework", false);
                bool effIsConflict = effect.value("__vfxd_conflict", false);
                bool effIsDbEffect = effect.value("__vfxd_db_effect", false);

                //_ A DB tab node's identity is effect_id, not a single guid --
                // see db_tree_view.h. guids here can be more than one.
                int64_t dbEffectId = effIsDbEffect ? effect.value("effect_id", int64_t(0)) : 0;
                std::vector<std::string> dbEffectGuids;
                if (effIsDbEffect && effect.contains("guids") && effect["guids"].is_array())
                    for (const auto& g : effect["guids"])
                        if (g.is_string())
                            dbEffectGuids.push_back(g.get<std::string>());
                bool isDbEffectRenamingThis = effIsDbEffect && IsDbEffectBeingRenamed(dbEffectId);

                //_ Hollowed out by a GUID drag-merge, or by deleting the
                // last GUID by hand -- see "Delete Empty" below. Alpha-dimmed
                // rather than text-colored, so it layers with the tints below.
                bool effIsEmptyGuids = !effect.contains("guids") || !effect["guids"].is_array() || effect["guids"].empty();
                if (effIsEmptyGuids)
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);

                //_ A settings conflict is just as much a "review before
                // applying" situation as a duplicate guid -- same red tint,
                // same top priority.
                if (effIsDupe || effIsConflict)
                    ImGui::PushStyleColor(ImGuiCol_Text, kDuplicateColor);
                else if (effIsNew)
                    ImGui::PushStyleColor(ImGuiCol_Text, kNewColor);
                else if (effIsRework)
                    ImGui::PushStyleColor(ImGuiCol_Text, kReworkColor);
                else if (effIsDbEffect)
                    ImGui::PushStyleColor(ImGuiCol_Text, kDbOnlyColor);

                //_ Forced open only if it matched through hidden content
                // (description/GUID) -- a name match is already visible on
                // the row. Query-change gating as above (see file header).
                bool effectNeedsForceOpen = searchActive && CachedEffectHiddenMatches(effect, s_treeSearchQueryLower, effectMatchCache);
                if (s_treeSearchQueryChanged)
                    ImGui::SetNextItemOpen(effectNeedsForceOpen, ImGuiCond_Always);
                bool nodeOpen = ImGui::TreeNode("effect", "%s%s%s%s", effName.c_str(),
                                                isEditingThis ? " (editing)" : "",
                                                isDbEffectRenamingThis ? " (renaming)" : "",
                                                effIsEmptyGuids ? " (empty)" : "");

                if (effIsDupe || effIsNew || effIsRework || effIsConflict || effIsDbEffect)
                    ImGui::PopStyleColor();
                if (effIsEmptyGuids)
                    ImGui::PopStyleVar();

                //_ Places a dragged effect immediately above this row --
                // complements the category-row target above so together
                // they cover every position; skipped for an overlay-only effect.
                if (!effIsNew && !effIsRework && !effIsDbEffect && ImGui::BeginDragDropTarget())
                {
                    if (ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
                    {
                        //_ Same same-sin guard as the category target, plus two
                        // no-op cases: dropped on itself, or on the effect right
                        // after it (post-erase-shift, that would land it back where it started).
                        const EffectDragPayload& dragPayload = GetEffectDragPayload();
                        bool sameCategory = dragPayload.originalPath == pathSoFar;
                        bool noOp = sameCategory && (dragPayload.originalIndex == effIndex ||
                                                      dragPayload.originalIndex == effIndex - 1);
                        if (dragPayload.sinName == sinName && !noOp)
                        {
                            EffectMoveJob job;
                            job.sinName          = dragPayload.sinName;
                            job.originalPath     = dragPayload.originalPath;
                            job.effectName       = dragPayload.effectName;
                            job.originalIndex    = dragPayload.originalIndex;
                            job.destinationPath  = pathSoFar;
                            job.destinationIndex = effIndex;

                            QueueEffectMove(std::move(job));
                        }
                    }

                    //_ Every effect row is a valid target except the one
                    // currently being edited (see QueueGuidMerge) and itself.
                    if (!isEditingThis && ImGui::AcceptDragDropPayload("VFXD_GUID"))
                    {
                        const GuidDragPayload& guidPayload = GetGuidDragPayload();
                        bool sameEffect = guidPayload.originalPath == pathSoFar && guidPayload.originalIndex == effIndex;
                        if (guidPayload.sinName == sinName && !sameEffect)
                        {
                            GuidMergeJob job;
                            job.sinName               = guidPayload.sinName;
                            job.originalPath          = guidPayload.originalPath;
                            job.originalIndex         = guidPayload.originalIndex;
                            job.effectName            = guidPayload.effectName;
                            job.guid                  = guidPayload.guid;
                            job.destinationPath       = pathSoFar;
                            job.destinationIndex      = effIndex;
                            job.destinationEffectName = effName;

                            QueueGuidMerge(std::move(job));
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                //_ Not offered on an overlay-only effect, same reasoning as
                // the drop target above. Gated on AnyEditInFlight so a drag
                // can't start mid-edit elsewhere.
                if (!effIsNew && !effIsRework && !effIsDbEffect && !AnyEditInFlight() && ImGui::BeginDragDropSource())
                {
                    BeginEffectDrag(sinName, pathSoFar, effName, effIndex);
                    ImGui::SetDragDropPayload("VFXD_EFFECT", &kEffectDragMarker, sizeof(kEffectDragMarker));
                    ImGui::Text("Move \"%s\"", effName.c_str());
                    ImGui::EndDragDropSource();
                }

                //_ The DB tab's own drag source -- see kDbTabSinName's
                // accept in this function's category-level drop target above.
                if (effIsDbEffect && !dbEffectGuids.empty() && !AnyEditInFlight() && ImGui::BeginDragDropSource())
                {
                    BeginDbEffectDrag(dbEffectId, dbEffectGuids, effName);
                    ImGui::SetDragDropPayload("VFXD_DB_EFFECT", &kDbEffectDragMarker, sizeof(kDbEffectDragMarker));
                    ImGui::Text("Move \"%s\"", effName.c_str());
                    ImGui::EndDragDropSource();
                }

                //_ Only offered when no edit is in flight anywhere, and
                // never on an overlay-only effect -- nothing at
                // pathSoFar/effIndex is guaranteed to be it until applied.
                if (!effIsNew && !effIsRework && !effIsDbEffect && !AnyEditInFlight() && ImGui::BeginPopupContextItem("effect_ctx"))
                {
                    if (ImGui::MenuItem("Edit"))
                        BeginEdit(sinName, pathSoFar, effIndex, effect);
                    ImGui::EndPopup();
                }

                //_ The DB tab's own context menu -- no "Add to JSON": SQL
                // is the source of truth here, there's nothing to promote
                // (see EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md).
                if (effIsDbEffect && !dbEffectGuids.empty() && !AnyEditInFlight() && ImGui::BeginPopupContextItem("dbeffect_ctx"))
                {
                    if (ImGui::MenuItem("Rename"))
                        BeginDbEffectRename(dbEffectId, dbEffectGuids, effName);
                    ImGui::EndPopup();
                }

                //_ Never grayed out for emptiness (unlike a category) --
                // only while some other edit/delete/create/rename is in
                // flight elsewhere. Not offered on an overlay-only effect.
                if (!effIsNew && !effIsRework && !effIsDbEffect)
                {
                    bool deleteDisabled = AnyEditInFlight() && !isDeletingThisEffect;
                    ImGui::SameLine();
                    if (deleteDisabled)
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                    bool deleteClicked = ImGui::SmallButton("-##deleff");
                    if (deleteDisabled)
                        ImGui::PopStyleVar();
                    if (deleteClicked && !deleteDisabled)
                        BeginDeleteConfirm(sinName, pathSoFar, effIndex, /*isCategory=*/false, effName);
                }

                //_ Sits below the row, visible whether nodeOpen or not.
                if (isDeletingThisEffect)
                    RenderDeleteConfirm();

                if (nodeOpen)
                {
                    if (isEditingThis)
                    {
                        RenderEffectEditor();
                    }
                    else if (isDbEffectRenamingThis)
                    {
                        RenderDbEffectRenameEditor();
                    }
                    else if (effIsDbEffect)
                    {
                        if (effect.value("in_json", false))
                            ImGui::TextDisabled("Also installed (exists in a loaded sin file).");

                        if (effect.contains("description") && effect["description"].is_string())
                        {
                            std::string desc = effect["description"].get<std::string>();
                            if (!desc.empty())
                                ImGui::TextWrapped("%s", desc.c_str());
                        }

                        //_ No GuidListDragContext -- per-guid drag/merge is a
                        // JSON-tab concept (see QueueGuidMerge); the DB tab
                        // only ever moves a whole effect_id at once.
                        RenderGuidList("guids", dbEffectGuids);

                        //_ Requires a non-empty array, not just presence --
                        // BuildDbTree always sets "behaviors" to *some*
                        // array (see its own comment), an empty one when
                        // the seed had none, so contains()+is_array() alone
                        // was true for every DB-tab node regardless of
                        // whether there was anything to show.
                        //
                        // Wording deliberately drops "(owned by VfxDenoiser)"
                        // here, unlike the JSON tab's version of this same
                        // header just below -- this value is a one-time
                        // snapshot seed_effect_db.py copied in from a Greed
                        // file's own "behaviors" at import time (see
                        // EffectDbEffect::behaviorType's doc comment), not
                        // read live from any sin file. VfxDenoiser never
                        // reads this database at all, so calling it
                        // "owned by VfxDenoiser" here would claim a live
                        // relationship that doesn't exist, on a field the
                        // user couldn't edit through this tab regardless.
                        if (effect.contains("behaviors") && effect["behaviors"].is_array()
                            && !effect["behaviors"].empty())
                        {
                            ImGui::TextDisabled("Behaviors (seeded snapshot, not read by VfxDenoiser):");
                            for (const auto& behavior : effect["behaviors"])
                                RenderBehavior(behavior);
                        }
                        // BuildDbTree for where "__vfxd_db_by_guid" comes from.
                        // This is the whole reason the DB tab exists: every
                        // guid's block/type/occurrence/group history, not just
                        // its curated name/category.
                        if (effect.contains("__vfxd_db_by_guid") && effect["__vfxd_db_by_guid"].is_object()
                            && !effect["__vfxd_db_by_guid"].empty())
                        {
                            if (ImGui::TreeNode("effectdb_detail", "Capture data"))
                            {
                                RenderEffectDbDetail(effect);
                                ImGui::TreePop();
                            }
                        }
                    }
                    else
                    {
                        if (effIsDupe)
                            ImGui::TextColored(kDuplicateColor,
                                "One or more of this effect's GUIDs is also used by another installed effect. "
                                "This shouldn't normally happen and updates are blocked for this file until it's resolved.");
                        else if (effIsNew)
                            ImGui::TextColored(kNewColor, "New from a pending update -- not yet applied.");
                        else if (effIsRework)
                        {
                            int mergedCount = effect.value("__vfxd_merged_count", 0);
                            bool renamed    = effect.contains("__vfxd_old_name");
                            bool movedCat   = effect.contains("__vfxd_old_category");

                            if (mergedCount > 0)
                            {
                                std::string msg = "This effect and " + std::to_string(mergedCount) +
                                                   (mergedCount == 1 ? " other effect" : " other effects") +
                                                   " would be merged into this one by a pending update.";
                                ImGui::TextColored(effIsConflict ? kDuplicateColor : kReworkColor, "%s", msg.c_str());
                                if (effIsConflict)
                                    ImGui::TextColored(kDuplicateColor,
                                        "The merged effect(s) had different settings -- shown below, review before applying.");
                            }
                            else if (effIsConflict)
                            {
                                //_ A GUID this effect absorbed used to belong to
                                // another effect surviving under its own separate
                                // update -- still worth a second look even though nothing's lost.
                                ImGui::TextColored(kDuplicateColor,
                                    "This effect absorbed a GUID from another effect with different settings -- shown below, review before applying.");
                            }
                            else if (renamed || movedCat)
                            {
                                ImGui::TextColored(kReworkColor,
                                    "This effect's GUIDs, name, and/or category would be updated by a pending update.");
                            }
                            else
                            {
                                ImGui::TextColored(kReworkColor,
                                    "GUIDs would be refreshed by a pending update -- name/category/settings stay as they are.");
                            }

                            if (renamed)
                                ImGui::TextColored(kReworkColor, "Renamed from \"%s\".",
                                    effect.value("__vfxd_old_name", std::string()).c_str());
                            if (movedCat)
                                ImGui::TextColored(kReworkColor, "Moved from \"%s\".",
                                    effect.value("__vfxd_old_category", std::string()).c_str());

                            //_ What the conflict warning above asks to review.
                            if (effIsConflict)
                                RenderConflictSources(effect);
                        }

                        if (effect.contains("description") && effect["description"].is_string())
                        {
                            std::string desc = effect["description"].get<std::string>();
                            if (!desc.empty())
                                ImGui::TextWrapped("%s", desc.c_str());
                        }

                        std::vector<std::string> guids;
                        if (effect.contains("guids") && effect["guids"].is_array())
                            for (const auto& g : effect["guids"])
                                if (g.is_string())
                                    guids.push_back(g.get<std::string>());

                        if (effIsRework)
                        {
                            //_ Current (default color) and pending-update
                            // (kReworkColor) stacked, so both are visible
                            // without needing to apply first.
                            std::vector<std::string> newGuids;
                            if (effect.contains("__vfxd_new_guids") && effect["__vfxd_new_guids"].is_array())
                                for (const auto& g : effect["__vfxd_new_guids"])
                                    if (g.is_string())
                                        newGuids.push_back(g.get<std::string>());

                            RenderGuidDiff(guids, newGuids);
                        }
                        else
                        {
                            //_ effIsNew still needs checking here -- an overlay-only
                            // effect has no stable on-disk position (same reasoning
                            // as the row's own guards above), so its GUIDs can't drag.
                            if (effIsNew)
                            {
                                RenderGuidList("guids", guids);
                            }
                            else
                            {
                                GuidListDragContext dragContext;
                                dragContext.sinName    = sinName;
                                dragContext.path       = pathSoFar;
                                dragContext.index      = effIndex;
                                dragContext.effectName = effName;
                                RenderGuidList("guids", guids, nullptr, &dragContext);
                            }
                        }

                        //_ Requires non-empty, not just presence -- see the
                        // DB tab's version of this same check above for why
                        // contains()+is_array() alone isn't enough.
                        if (effect.contains("behaviors") && effect["behaviors"].is_array()
                            && !effect["behaviors"].empty())
                        {
                            ImGui::TextDisabled("Behaviors (owned by VfxDenoiser):");
                            for (const auto& behavior : effect["behaviors"])
                                RenderBehavior(behavior);
                        }

                        //_ Anything beyond the confirmed schema is
                        // unexpected -- surface it rather than drop it.
                        for (const auto& [key, value] : effect.items())
                        {
                            if (key == "name" || key == "description" || key == "guids" || key == "behaviors"
                                || key == "__vfxd_new" || key == "__vfxd_rework" || key == "__vfxd_new_guids"
                                || key == "__vfxd_hasnew" || key == "__vfxd_hasrework"
                                || key == "__vfxd_dupe_guid" || key == "__vfxd_hasdupe"
                                || key == "__vfxd_old_name" || key == "__vfxd_old_category"
                                || key == "__vfxd_merged_count" || key == "__vfxd_conflict"
                                || key == "__vfxd_conflict_sources")
                                continue;
                            RenderJsonValue(key, value);
                        }
                    }

                    ImGui::TreePop();
                }
                else if (isEditingThis)
                {
                    //_ Collapsing mid-edit cancels it, same as the category case.
                    CancelEdit();
                }

                ImGui::PopID();
            }
        }

        if (category.contains("categories") && category["categories"].is_array())
        {
            int i = 0;
            for (const auto& sub : category["categories"])
            {
                ImGui::PushID(i);
                RenderCategoryTree(sinName, sub, pathSoFar, namePathSoFar, i, matchCache, effectMatchCache, categoryMatchesDirectly);
                ImGui::PopID();
                ++i;
            }
        }

        ImGui::TreePop();
    }
    else
    {
        //_ Collapsed -- nothing inside (rename UI, create prompt, effect editor)
        // is drawn this frame, so cancel rather than let it run invisibly.
        // DeleteConfirmState is excluded -- its row sits outside this content.
        if (IsCategoryRenameUnderPath(sinName, pathSoFar))
            CancelCategoryEdit();
        if (IsEffectEditUnderPath(sinName, pathSoFar))
            CancelEdit();
        if (IsCategoryCreateUnderPath(sinName, pathSoFar))
            CancelCreateCategory();
    }

    pathSoFar.pop_back();
    namePathSoFar.pop_back();
}

} //. namespace
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderJsonTabContent
//--------------------------------------------------------------------------------
// The JSON tab's content -- one top-level TreeNode per installed sin file, each
// expanding into that file's real category tree via RenderCategoryTree. Split out
// of RenderInstalledEffects so it can sit inside its own tab item, alongside
// RenderDbTabContent's "Database" tab -- see EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md.
//--------------------------------------------------------------------------------
void RenderJsonTabContent()
{
    if (GetInstalledSins().empty())
    {
        ImGui::TextDisabled("No Visual Sins effect files found in VfxDenoiser's folder.");
        return;
    }

    //_ A Ready diff plan overlays pending-update coloring onto this same
    // tree (BuildDiffOverlayTree) rather than a separate list. Sins with
    // no plan yet just render the plain on-disk tree. Two independent
    // sources can each have a Ready diff cached for the same sin now
    // (GitHub's own StartLoadDiff, and sql_update.cpp's LoadSqlDiff --
    // see sql_update.h's GetSqlDiffInfo() doc comment for why this
    // lookup needed adding) -- SQL wins the tie if somehow both are
    // Ready at once, since it's the path this addon is moving toward;
    // GitHub only shown when SQL has nothing loaded for that sin.
    std::vector<SinDiffInfo> diffs    = GetSinDiffInfo();
    std::vector<SinDiffInfo> sqlDiffs = GetSqlDiffInfo();
    bool anyOverlayShown  = false;
    bool anyConflictShown = false;

    for (const auto& sin : GetInstalledSins())
    {
        ImGui::PushID(sin.sinName.c_str());

        const nlohmann::ordered_json* installedFile = FindInstalledJson(sin.sinName);
        if (!installedFile)
        {
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s: couldn't read %s", sin.sinName.c_str(), sin.fileName.c_str());
            ImGui::PopID();
            continue;
        }

        const SinDiffInfo* diff = nullptr;
        int diffSource = 0; //. 0 = none, 1 = GitHub, 2 = SQL -- see OverlayCacheEntry::diffSource
        for (const auto& d : sqlDiffs)
            if (d.sinName == sin.sinName && d.status != EDiffStatus::NotLoaded)
                { diff = &d; diffSource = 2; }
        if (!diff)
        {
            for (const auto& d : diffs)
                if (d.sinName == sin.sinName)
                    { diff = &d; diffSource = 1; }
        }

        bool hasOverlay = diff && diff->status == EDiffStatus::Ready && !diff->plan.IsEmpty();

        const auto& duplicateGuidsBySin = GetDuplicateGuidsBySin();
        auto        dupIt    = duplicateGuidsBySin.find(sin.sinName);
        bool        hasDupes = dupIt != duplicateGuidsBySin.end() && !dupIt->second.empty();

        //_ Duplicate-guid tagging first (a property of the file itself), then
        // the pending-update diff on the same copy -- both can coexist; RenderCategoryTree
        // picks red over orange/green. Only ever a copy -- see OverlayCacheEntry.
        const nlohmann::ordered_json* fileToRender = installedFile;

        //_ Identity token for what fileToRender points at, consumed by the
        // search cache below. Top-bit-tagged so a plain tree's generation
        // counter can never collide with an overlay's contentVersion.
        long long fileTreeVersion = (2LL << 32) | static_cast<unsigned int>(GetInstalledTreeGeneration());

        if (hasDupes || hasOverlay)
        {
            EDiffStatus statusForCache = diff ? diff->status : EDiffStatus::NotLoaded;
            OverlayCacheEntry& cached = s_overlayCache[sin.sinName];

            bool stale = cached.generation != GetInstalledTreeGeneration()
                || cached.diffStatus != statusForCache
                || cached.diffSource != diffSource;

            if (stale)
            {
                nlohmann::ordered_json built = *installedFile;
                if (hasDupes)
                    built = BuildDuplicateOverlayTree(built, dupIt->second);
                if (hasOverlay)
                    built = BuildDiffOverlayTree(built, diff->plan);

                cached.generation = GetInstalledTreeGeneration();
                cached.diffStatus = statusForCache;
                cached.diffSource = diffSource;
                cached.file       = std::move(built);
                ++cached.contentVersion;
            }

            //_ Rebuilt or cache-hit, `cached.contentVersion` reflects the
            // content actually behind `fileToRender` now -- what the
            // search cache below needs to decide if it can reuse its work.
            fileTreeVersion = (1LL << 32) | static_cast<unsigned int>(cached.contentVersion);

            fileToRender = &cached.file;
            if (hasOverlay)
            {
                anyOverlayShown = true;
                for (const auto& rw : diff->plan.reworks)
                    if (rw.behaviorsConflict) { anyConflictShown = true; break; }
            }
        }

        if (hasDupes)
        {
            ImGui::TextColored(kDuplicateColor,
                "%s: duplicate GUID(s) detected in this file -- updates are blocked until this is resolved (see red entries below).",
                sin.sinName.c_str());
        }

        //_ Whether this file has any match at all -- lets the root row force
        // itself open. Built fresh once per sin per frame, O(size) bottom-up
        // (see BuildCategoryMatchCache), and reused below by RenderCategoryTree.
        bool searchActive   = !s_treeSearchQueryLower.empty();
        bool anyMatchInFile = false;

        //_ Unused, never-written fallbacks for the !searchActive case --
        // RenderCategoryTree takes matchCache/effectMatchCache by reference
        // unconditionally but only reads them when searchActive is true.
        static const CategoryMatchCache s_emptySearchCategoryCache;
        static const EffectMatchCache   s_emptySearchEffectCache;

        const CategoryMatchCache* matchCachePtr       = &s_emptySearchCategoryCache;
        const EffectMatchCache*   effectMatchCachePtr = &s_emptySearchEffectCache;

        if (searchActive)
        {
            SearchCacheEntry& searchCached = s_searchCache[sin.sinName];

            //_ Only rebuild when the query text or tree content this cache
            // was built against (see fileTreeVersion above) actually
            // changed -- otherwise a hit, O(1) instead of O(effect count).
            bool searchStale = searchCached.query != s_treeSearchQueryLower ||
                                searchCached.treeVersion != fileTreeVersion;

            if (searchStale)
            {
                searchCached.categoryCache.clear();
                searchCached.effectCache.clear();

                bool anyMatch = false;
                if (fileToRender->contains("categories") && (*fileToRender)["categories"].is_array())
                    for (const auto& cat : (*fileToRender)["categories"])
                    {
                        BuildCategoryMatchCache(cat, s_treeSearchQueryLower, searchCached.categoryCache, searchCached.effectCache);
                        if (searchCached.categoryCache[&cat].subtreeMatches)
                            anyMatch = true;
                    }

                searchCached.query          = s_treeSearchQueryLower;
                searchCached.treeVersion    = fileTreeVersion;
                searchCached.anyMatchInFile = anyMatch;
            }

            anyMatchInFile      = searchCached.anyMatchInFile;
            matchCachePtr       = &searchCached.categoryCache;
            effectMatchCachePtr = &searchCached.effectCache;
        }

        const CategoryMatchCache& matchCache       = *matchCachePtr;
        const EffectMatchCache&   effectMatchCache = *effectMatchCachePtr;

        //_ Same query-change force-open/shut gating as RenderCategoryTree.
        if (s_treeSearchQueryChanged)
            ImGui::SetNextItemOpen(anyMatchInFile, ImGuiCond_Always);

        bool rootOpen = ImGui::TreeNode("root", "%s (%s)", sin.sinName.c_str(), sin.fileName.c_str());

        //_ Same fix as RenderCategoryTree's collapsed branch: closed on a
        // query-change frame, so reset descendants' state here instead.
        // "root" is TreeNode's str_id, pushed to match its real ID scope.
        if (!rootOpen && s_treeSearchQueryChanged)
        {
            ImGui::PushID("root");
            if (fileToRender->contains("categories") && (*fileToRender)["categories"].is_array())
            {
                int i = 0;
                for (const auto& cat : (*fileToRender)["categories"])
                {
                    ImGui::PushID(i);
                    SilentlyCloseSubtree(cat);
                    ImGui::PopID();
                    ++i;
                }
            }
            ImGui::PopID();
        }

        if (rootOpen)
        {
            std::vector<int>         path;      //. this sin's top-level path
            std::vector<std::string> namePath;  //. name-based counterpart

            //_ Reorder-only (see file header): this root row is the
            // "shared parent's own row" a top-level category doesn't
            // otherwise have. Only offered within the same sin file.
            if (ImGui::BeginDragDropTarget())
            {
                if (ImGui::AcceptDragDropPayload("VFXD_CATEGORY"))
                {
                    if (GetCategoryDragSinName() == sin.sinName && GetCategoryDragPath().size() == 1)
                    {
                        const std::vector<int>& dragPath = GetCategoryDragPath();
                        int  draggedIndex = dragPath.back();
                        bool alreadyLast  = fileToRender->contains("categories") && (*fileToRender)["categories"].is_array() &&
                                            draggedIndex == static_cast<int>((*fileToRender)["categories"].size()) - 1;
                        if (!alreadyLast)
                            QueueCategoryMove(GetCategoryDragSinName(), dragPath, -1);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool isCreatingAtTopLevel = IsCreatingCategoryAt(sin.sinName, path);

            bool createDisabled = AnyEditInFlight() && !isCreatingAtTopLevel;
            ImGui::SameLine();
            if (createDisabled)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            bool createClicked = ImGui::SmallButton("+##addcat_top");
            if (createDisabled)
                ImGui::PopStyleVar();
            if (createClicked && !createDisabled)
                BeginCreateCategory(sin.sinName, path);

            if (isCreatingAtTopLevel)
                RenderCreateCategoryEditor();

            const nlohmann::ordered_json& file = *fileToRender;
            if (file.contains("categories") && file["categories"].is_array())
            {
                if (searchActive && !anyMatchInFile)
                {
                    ImGui::TextDisabled("(no matches in this file)");
                }
                else
                {
                    int i = 0;
                    for (const auto& cat : file["categories"])
                    {
                        ImGui::PushID(i);
                        RenderCategoryTree(sin.sinName, cat, path, namePath, i, matchCache, effectMatchCache);
                        ImGui::PopID();
                        ++i;
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("(no categories in this file)");
            }
            ImGui::TreePop();
        }
        else if (IsCreatingCategoryAt(sin.sinName, std::vector<int>()))
        {
            //_ Collapsing hides the "+" button and prompt, so cancel it.
            CancelCreateCategory();
        }

        ImGui::PopID();
    }

    if (anyOverlayShown)
    {
        ImGui::Spacing();
        ImGui::TextColored(kNewColor,    "* New effect from a pending update.");
        ImGui::TextColored(kReworkColor, "* GUIDs would be refreshed or merged or the name or category has changed.");
        if (anyConflictShown)
            ImGui::TextColored(kDuplicateColor, "* Merged effects had different settings -- review before applying");
    }
    if (!GetDuplicateGuidsBySin().empty())
    {
        bool anyDupes = false;
        for (const auto& [name, guids] : GetDuplicateGuidsBySin())
            if (!guids.empty()) { anyDupes = true; break; }
        if (anyDupes)
        {
            ImGui::Spacing();
            ImGui::TextColored(kDuplicateColor, "* Duplicate GUID shared with another installed effect -- resolve before updating");
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderDbTabGroupsView
//--------------------------------------------------------------------------------
// The DB tab's "Groups" toggle content -- a flat, alphabetical-by-guid
// list of every type:1/11 group starter (EffectDb_GetAllGroupStarters),
// filtered to unnamed-only by default. Clicking one shows its members
// (EffectDb_GetGroupsStarted, unchanged existing order -- no reordering
// needed, a starter can't meaningfully be "discovered before" a group it
// opens the way a whole *group* could, and sort_order is for SQL->JSON
// generation only, not this).
//
// Deliberately a separate, smaller renderer rather than folded into
// RenderCategoryTree -- this isn't a category tree at all, it's a flat
// browse-then-drill-in list, same "own smaller renderer" reasoning
// effect_db_tab_view.h originally gave for the DB tab as a whole.
//
// Renaming reuses the DB tab's existing effect_id-keyed rename state
// machine unchanged (BeginDbEffectRename/IsDbEffectBeingRenamed/
// RenderDbEffectRenameEditor, see installed_tree_edit.h) -- a starter or
// member clicked here is the same effect_id-backed row the main tree
// already knows how to rename; this view is just a different way of
// finding it, not a new write path.
//
// Both the starter and every member also get a "Classes seen" line (see
// ClassesSeenLabel) -- an unnamed starter's own guid carries no hint of
// what it is, but a quick glance at which professions triggered its
// members is often enough to guess the category it belongs in without
// opening the full per-occurrence breakdown RenderEffectDbDetail gives
// in the main tree.
//--------------------------------------------------------------------------------
void RenderDbTabGroupsView(const std::vector<EffectDbEffect>& starters, bool& showAllStarters,
                            std::string& selectedGuid)
{
    ImGui::TextDisabled("Every type:1 / type:11 group starter the effect database has ever seen.\n"
                         "Click one to see its members -- often the easiest way to guess what an unnamed starter is.");

    ImGui::Checkbox("Show all (including already-named)##db_tab_groups_show_all", &showAllStarters);

    //_ Alphabetical by guid_b64 -- deliberately not a "discovery order":
    // that would need borrowing effect_meta.sort_order, which is for
    // SQL->JSON generation only (see its own doc comment) and doesn't
    // mean "first captured" in any sense this view could rely on. Just a
    // stable, no-claimed-meaning sort, same spirit as SortTreeRecursive
    // elsewhere in this tab.
    std::vector<const EffectDbEffect*> visible;
    for (const auto& s : starters)
        if (showAllStarters || s.name.empty())
            visible.push_back(&s);
    std::sort(visible.begin(), visible.end(), [](const EffectDbEffect* a, const EffectDbEffect* b)
    {
        return a->guid_b64 < b->guid_b64;
    });

    ImGui::Text("%zu starter%s%s", visible.size(), visible.size() == 1 ? "" : "s",
                showAllStarters ? "" : " (unnamed)");

    //_ Fixed at kVisibleRows -- previously grew/shrank with the starter
    // list's own row count (clamped between a floor and this same cap),
    // but that tied the details pane's height to the wrong thing: its
    // actual content (header, rename controls, classes-seen line, then
    // every duration/a4 instance and its members) is often taller than
    // even a generous starter-count-driven floor, regardless of how many
    // starters happen to be in the list. Just a flat height now; ImGui's
    // own child scrollbar still takes over past this for either side
    // (the starter list via its Selectable loop, the details pane via its
    // own content) exactly as before.
    constexpr size_t kVisibleRows = 20;

    float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    float listHeight = kVisibleRows * lineHeight + ImGui::GetStyle().FramePadding.y * 2.0f;

    ImGui::BeginChild("db_tab_group_starters", ImVec2(320.0f, listHeight), true);
    if (visible.empty())
    {
        ImGui::TextDisabled(showAllStarters ? "No group starters captured yet."
                                             : "No unnamed group starters.");
    }
    for (const EffectDbEffect* s : visible)
    {
        //_ Bare guid when unnamed -- appearing as a raw guid rather than a
        // name already implies "unnamed" on its own (the default filter
        // above is unnamed-only anyway), so an explicit "(unnamed)" prefix
        // here was saying the same thing twice. Same convention now used
        // in the details pane on the right.
        const std::string& label = s->name.empty() ? s->guid_b64 : s->name;
        ImGui::PushID(s->guid_b64.c_str());
        if (ImGui::Selectable(label.c_str(), selectedGuid == s->guid_b64))
            selectedGuid = s->guid_b64;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    //_ Looked up from the full `starters` list, not just `visible` --
    // the selection can outlive the filter (renamed while selected, or
    // "show all" toggled off) without being cleared, and its members are
    // still worth showing either way.
    const EffectDbEffect* selected = nullptr;
    for (const auto& s : starters)
        if (s.guid_b64 == selectedGuid) { selected = &s; break; }

    ImGui::BeginChild("db_tab_group_details", ImVec2(0.0f, listHeight), true);

    if (selectedGuid.empty())
    {
        ImGui::TextDisabled("Select a starter to see its members.");
    }
    else if (!selected)
    {
        ImGui::TextDisabled("Selected starter is no longer known to the database.");
    }
    else
    {
        ImGui::PushID(static_cast<int>(selected->effect_id));

        //_ No "(unnamed starter)"/"Starter:" framing -- the left list
        // selection already establishes "this is the selected starter",
        // and a name-less guid on its own already implies unnamed (same
        // reasoning as dropping "(unnamed)" from the member list below).
        // If it has a name, show it; either way the guid+type line always
        // shows so there's no ambiguity about which one this is.
        if (!selected->name.empty())
            ImGui::Text("%s", selected->name.c_str());
        ImGui::TextDisabled("%s -- type %d", selected->guid_b64.c_str(), selected->type);

        std::string starterClasses = ClassesSeenLabel(selected->guid_b64);
        ImGui::TextDisabled("Classes seen: %s", starterClasses.empty() ? "(no data)" : starterClasses.c_str());

        if (!AnyEditInFlight() && ImGui::SmallButton("Rename starter"))
            BeginDbEffectRename(selected->effect_id, { selected->guid_b64 }, selected->name);
        if (IsDbEffectBeingRenamed(selected->effect_id))
            RenderDbEffectRenameEditor();

        ImGui::Spacing();
        ImGui::TextDisabled("Members, by (duration, a4) instance:");

        std::vector<EffectDbGroupInstance> instances = EffectDb_GetGroupsStarted(selectedGuid);
        if (instances.empty())
            ImGui::TextDisabled("(no instances recorded)");

        for (const auto& inst : instances)
        {
            ImGui::PushID(inst.duration);
            ImGui::PushID(static_cast<int>(inst.a4));

            //_ The starter's own row in group_members (member_guid_b64 ==
            // starter_guid_b64, see effect_db.h's group_members doc
            // comment) is filtered out of both the count and the list
            // below -- the header above already identifies the starter,
            // and this whole view exists specifically to browse type:1/11
            // starters, so re-flagging "this is the starter" a third time
            // inside its own member list added nothing.
            std::vector<std::string> otherMembers;
            for (const auto& memberGuid : inst.memberGuids)
                if (memberGuid != selectedGuid)
                    otherMembers.push_back(memberGuid);

            ImGui::Text("duration %d, a4 %u -- %zu other member%s", inst.duration, inst.a4,
                        otherMembers.size(), otherMembers.size() == 1 ? "" : "s");

            ImGui::Indent();
            for (const auto& memberGuid : otherMembers)
            {
                ImGui::PushID(memberGuid.c_str());

                EffectDbEffect member;
                if (!EffectDb_GetEffect(memberGuid, member))
                {
                    ImGui::BulletText("%s (no longer known)", memberGuid.c_str());
                }
                else
                {
                    //_ Bare guid when unnamed, same convention as the
                    // starter list on the left -- appearing as a raw guid
                    // rather than a name already implies "unnamed", no
                    // need to spell that out again here either.
                    std::string memberLabel = member.name.empty() ? memberGuid : member.name;
                    ImGui::BulletText("%s", memberLabel.c_str());

                    //_ SameLine() only fires when the button is actually
                    // about to be drawn right after it -- previously
                    // called unconditionally ahead of a button gated on
                    // !AnyEditInFlight(), so as soon as any rename was in
                    // progress (starter or otherwise), every other
                    // member's SmallButton call got short-circuited away
                    // entirely while its SameLine() had already run,
                    // leaving the cursor mid-line with nothing left to
                    // draw there -- the next real item (the classes-seen
                    // line, or the next member's own bullet) then landed
                    // on top of this line instead of starting a new one.
                    if (!AnyEditInFlight())
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Rename"))
                            BeginDbEffectRename(member.effect_id, { memberGuid }, member.name);
                    }
                    if (IsDbEffectBeingRenamed(member.effect_id))
                        RenderDbEffectRenameEditor();

                    std::string memberClasses = ClassesSeenLabel(memberGuid);
                    if (!memberClasses.empty())
                    {
                        ImGui::Indent();
                        ImGui::TextDisabled("Classes seen: %s", memberClasses.c_str());
                        ImGui::Unindent();
                    }
                }

                ImGui::PopID();
            }
            ImGui::Unindent();

            ImGui::PopID();
            ImGui::PopID();
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderDbTabContent
//--------------------------------------------------------------------------------
// The Database tab's content -- fully independent of RenderJsonTabContent (see
// EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md's "Building the DB tab"). Ensures the db
// is open for browsing on its own -- see EffectDb_EnsureOpenForBrowsing's own
// comment on why this can't just rely on EffectDb_SetEnabled (the "for science"
// capture toggle): a populated vfxd_effect_db.sqlite3 must be browsable here
// whether or not capture has ever been turned on this session.
//--------------------------------------------------------------------------------
void RenderDbTabContent(const std::string& denoiserAddonDir)
{
    {
        std::string openErr;
        if (!EffectDb_EnsureOpenForBrowsing(denoiserAddonDir, openErr))
        {
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Couldn't open the effect database: %s", openErr.c_str());
            return;
        }
    }

    //_ Own tree cache (rebuilt only when EffectDb_GetGeneration() actually
    // changed, same "deliberately not reacting to every generation bump"
    // spirit as OverlayCacheEntry above, except this IS the effect db's
    // own view, so it's the one place that's correct to react to). Reuses
    // s_searchCache under kDbTabSinName as its key -- same map, same
    // entry type, nothing DB-tab-specific needed there.
    ImGui::PushID(kDbTabSinName);
    ImGui::Spacing();
    ImGui::Separator();

    static nlohmann::ordered_json s_dbTabTree;
    static int                     s_dbTabTreeGeneration         = -1;
    static int                     s_dbTabInstalledTreeGeneration = -1;
    static int                     s_dbTabTreeContentVersion      = 0;
    static size_t                   s_dbTabEffectCount             = 0;

    //_ The Groups view's own state -- kept here (not inside
    // RenderDbTabGroupsView) so it survives across frames the same way
    // the tree cache above does, and so the starter list can share this
    // function's own generation gate rather than needing a second one.
    // See RenderDbTabGroupsView's own doc comment for what these mean.
    static bool                     s_dbTabShowGroups             = false;
    static bool                     s_dbTabGroupsShowAllStarters   = false;
    static std::string              s_dbTabSelectedStarterGuid;
    static std::vector<EffectDbEffect> s_dbTabGroupStarters;
    static int                     s_dbTabGroupStartersGeneration = -1;

    //_ Rebuilds on either generation moving -- effect_db's own (a real
    // capture/rename/placement happened) or the installed tree's (the
    // "in_json" badge below needs the freshly-loaded guid set, since it's
    // cross-referenced live rather than trusted off effects.in_json --
    // see BuildDbTree's header comment). GetInstalledTreeGeneration()
    // only ever moves on a load RenderInstalledEffects already guarantees
    // happened before this function runs (first-open lazy load, or the
    // shared "Refresh" button both tabs sit under), so no separate
    // "when does this recompute" trigger is needed here.
    int dbGenNow        = EffectDb_GetGeneration();
    int installedGenNow = GetInstalledTreeGeneration();

    //_ Only depends on effect_db's own generation (a rename or a fresh
    // capture), never the installed tree's -- group starters have no
    // in_json-style badge to refresh. Gated separately from the tree
    // rebuild below so toggling into the Groups view for the first time
    // doesn't wait on an installed-tree generation bump that may never
    // come.
    if (dbGenNow != s_dbTabGroupStartersGeneration)
    {
        s_dbTabGroupStarters           = EffectDb_GetAllGroupStarters();
        s_dbTabGroupStartersGeneration = dbGenNow;
    }

    if (dbGenNow != s_dbTabTreeGeneration || installedGenNow != s_dbTabInstalledTreeGeneration)
    {
        std::vector<EffectDbEffect> allEffects     = EffectDb_GetAllEffects();
        std::unordered_set<std::string> installedGuids = CollectInstalledGuids();
        s_dbTabEffectCount              = allEffects.size();
        s_dbTabTree                     = BuildDbTree(allEffects, installedGuids);
        s_dbTabTreeGeneration           = dbGenNow;
        s_dbTabInstalledTreeGeneration  = installedGenNow;
        ++s_dbTabTreeContentVersion;
    }

    if (ImGui::Button(s_dbTabShowGroups ? "Back to Tree##db_tab_groups_toggle" : "Groups (T1/T11 starters)##db_tab_groups_toggle"))
        s_dbTabShowGroups = !s_dbTabShowGroups;

    if (s_dbTabShowGroups)
    {
        RenderDbTabGroupsView(s_dbTabGroupStarters, s_dbTabGroupsShowAllStarters, s_dbTabSelectedStarterGuid);

        //_ Normally deferred to this function's very end, alongside the
        // JSON tab's own edit jobs (see the Apply block below) -- but
        // that block sits after this branch's early return, and the
        // Groups view's own "Rename"/"Rename starter" buttons queue
        // through this exact same job. Without this, a rename made from
        // here would sit pending and silently never get written unless
        // the user happened to switch back to the tree view afterward.
        ApplyPendingDbEffectRename();

        ImGui::PopID();
        return;
    }

    long long dbTreeVersion = (3LL << 32) | static_cast<unsigned int>(s_dbTabTreeContentVersion);

    bool searchActive   = !s_treeSearchQueryLower.empty();
    bool anyMatchInFile = false;

    static const CategoryMatchCache s_emptySearchCategoryCache;
    static const EffectMatchCache   s_emptySearchEffectCache;
    const CategoryMatchCache* matchCachePtr       = &s_emptySearchCategoryCache;
    const EffectMatchCache*   effectMatchCachePtr = &s_emptySearchEffectCache;

    if (searchActive)
    {
        SearchCacheEntry& searchCached = s_searchCache[kDbTabSinName];
        bool searchStale = searchCached.query != s_treeSearchQueryLower ||
                            searchCached.treeVersion != dbTreeVersion;

        if (searchStale)
        {
            searchCached.categoryCache.clear();
            searchCached.effectCache.clear();

            bool anyMatch = false;
            if (s_dbTabTree.contains("categories") && s_dbTabTree["categories"].is_array())
                for (const auto& cat : s_dbTabTree["categories"])
                {
                    BuildCategoryMatchCache(cat, s_treeSearchQueryLower, searchCached.categoryCache, searchCached.effectCache);
                    if (searchCached.categoryCache[&cat].subtreeMatches)
                        anyMatch = true;
                }

            searchCached.query          = s_treeSearchQueryLower;
            searchCached.treeVersion    = dbTreeVersion;
            searchCached.anyMatchInFile = anyMatch;
        }

        anyMatchInFile      = searchCached.anyMatchInFile;
        matchCachePtr       = &searchCached.categoryCache;
        effectMatchCachePtr = &searchCached.effectCache;
    }

    const CategoryMatchCache& matchCache       = *matchCachePtr;
    const EffectMatchCache&   effectMatchCache = *effectMatchCachePtr;

    if (s_treeSearchQueryChanged)
        ImGui::SetNextItemOpen(anyMatchInFile, ImGuiCond_Always);

    bool dbRootOpen = ImGui::TreeNode("db_root", "Effect Database (%d effects)",
                                       static_cast<int>(s_dbTabEffectCount));

    if (!dbRootOpen && s_treeSearchQueryChanged)
    {
        ImGui::PushID("db_root");
        if (s_dbTabTree.contains("categories") && s_dbTabTree["categories"].is_array())
        {
            int i = 0;
            for (const auto& cat : s_dbTabTree["categories"])
            {
                ImGui::PushID(i);
                SilentlyCloseSubtree(cat);
                ImGui::PopID();
                ++i;
            }
        }
        ImGui::PopID();
    }

    if (dbRootOpen)
    {
        ImGui::TextDisabled("Read from the effect database directly -- no JSON file involved.\n"
                             "Right-click an effect to rename it; drag it onto a category to move it.");

        std::vector<int>         path;
        std::vector<std::string> namePath;

        if (s_dbTabTree.contains("categories") && s_dbTabTree["categories"].is_array())
        {
            if (searchActive && !anyMatchInFile)
            {
                ImGui::TextDisabled("(no matches)");
            }
            else
            {
                int i = 0;
                for (const auto& cat : s_dbTabTree["categories"])
                {
                    ImGui::PushID(i);
                    RenderCategoryTree(kDbTabSinName, cat, path, namePath, i, matchCache, effectMatchCache);
                    ImGui::PopID();
                    ++i;
                }
            }
        }
        else
        {
            ImGui::TextDisabled("(no effects known to the database yet)");
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderInstalledEffects
//--------------------------------------------------------------------------------
// Draws the "Installed Effects" section: a shared header (Refresh, Delete
// Empty, search box, edit-result message), then a tab bar splitting the
// content into "JSON" (RenderJsonTabContent -- one top-level TreeNode per
// installed sin file) and "Database" (RenderDbTabContent -- the effect_id
// tree read straight from effect_db, see EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md).
// The two tabs are fully independent; neither reads or writes the other.
//--------------------------------------------------------------------------------
void RenderInstalledEffects(const std::string& denoiserAddonDir)
{
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    if (ImGui::Button("Refresh##installed_tree"))
        LoadInstalledEffectsTree(denoiserAddonDir);

    ImGui::SameLine();
    //_ Greyed out while another edit's in flight, same as the per-effect
    // "-" delete button -- not gated on there being anything empty yet,
    // that's re-checked when the confirm itself renders (see below).
    {
        bool deleteEmptyDisabled = AnyEditInFlight() && !IsDeleteEmptyConfirmActive();
        if (deleteEmptyDisabled)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        bool deleteEmptyClicked = ImGui::Button("Delete Empty##installed_tree");
        if (deleteEmptyDisabled)
            ImGui::PopStyleVar();
        if (deleteEmptyClicked && !deleteEmptyDisabled)
            BeginDeleteEmptyConfirm();
    }
    if (IsDeleteEmptyConfirmActive())
        RenderDeleteEmptyConfirm();

    ImGui::TextDisabled("Drag an effect onto a category to move it to the end of that category,\n"
                         "or onto another effect to place it just above that one.\n"
                         "Categories can be dragged only to reorder them in the same parent category.\n"
                         "GUIDs can be dragged onto other effects.\n"
                         "Right-click unfolded effects or categories to edit them.");

    //_ Recomputes the lowercased query RenderCategoryTree's matching
    // helpers compare against; the actual filtering happens down there.
    ImGui::InputTextWithHint("##installed_tree_search", "Search name / category / description / GUID...",
                              s_treeSearchBuf, sizeof(s_treeSearchBuf));
    if (s_treeSearchBuf[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##installed_tree_search"))
            s_treeSearchBuf[0] = '\0';
    }
    std::string typedLower = s_treeSearchBuf;
    std::transform(typedLower.begin(), typedLower.end(), typedLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (!typedLower.empty() && typedLower.size() < kMinTreeSearchLength)
        ImGui::TextDisabled("Keep typing... (search starts at %zu characters)", kMinTreeSearchLength);

    //_ Below the minimum, treat the query as empty (no filtering/expansion).
    std::string newQueryLower = (typedLower.size() >= kMinTreeSearchLength) ? typedLower : std::string();

    //_ See s_treeSearchQueryChanged's own comment for why this matters.
    s_treeSearchQueryChanged = (newQueryLower != s_treeSearchQueryLower);
    s_treeSearchQueryLower   = std::move(newQueryLower);

    //_ Search cleared -- drop the per-sin match caches (see
    // SearchCacheEntry) rather than let them sit around unused; they'll
    // rebuild fresh, lazily, next time a query goes active.
    if (s_treeSearchQueryChanged && s_treeSearchQueryLower.empty())
        s_searchCache.clear();

    if (!GetEditResultMessage().empty())
        ImGui::TextWrapped("%s", GetEditResultMessage().c_str());

    if (!ImGui::BeginTabBar("installed_effects_tabs"))
        return;

    if (ImGui::BeginTabItem("JSON"))
    {
        RenderJsonTabContent();
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Database"))
    {
        RenderDbTabContent(denoiserAddonDir);
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

    //_ Deferred to here, after every category/effect array has finished
    // iterating, so nothing is mutated mid-walk. All eight called
    // unconditionally.
    ApplyPendingEdit();
    ApplyPendingCategoryRename();
    ApplyPendingMove();
    ApplyPendingCategoryMove();
    ApplyPendingDelete();
    ApplyPendingCreateCategory();
    ApplyPendingGuidMerge();
    ApplyPendingDbEffectRename();
    ApplyPendingDbEffectCategoryPlacement();
}