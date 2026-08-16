//################################################################################
// live_log_ui.cpp   (see: live_log_ui.h)
//--------------------------------------------------------------------------------

#include "effect_db.h"
#include "game_state.h"
#include "imgui.h"
#include "installed_tree_store.h"
#include "live_log_ui.h"
#include "live_log.h"
#include "report_ui.h"
#include "specialization_info.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

//_ UI-only, not persisted -- same convention as live_log.cpp's s_typeEnabled.
bool        s_forScienceView = false;
//_ Which entry (if any) has its inline rename editor open; only one at a time.
std::string s_quickEditGuid;
char        s_quickEditBuf[128] = "";   //. holds s_quickEditGuid's in-progress name

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ForScienceGroupMemberLabel
//--------------------------------------------------------------------------------
// Same "prefer the db's own name, fall back to the raw guid" resolution
// installed_tree_view.cpp's GroupMemberLabel uses -- kept as its own copy here
// instead of shared, same reasoning RenderForScienceDetail's own comment already
// gives for reading EffectDb_Get* directly instead of a json node: this panel has
// no tree cache to share it through.
//--------------------------------------------------------------------------------
std::string ForScienceGroupMemberLabel(const std::string& guid_b64)
{
    EffectDbEffect eff{};
    if (EffectDb_GetEffect(guid_b64, eff) && !eff.name.empty())
        return eff.name;
    return guid_b64;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// DecodeSpecOrCoreId
//--------------------------------------------------------------------------------
// One raw id from EffectDb_SpecOrCoreIdsInMask (1..127, see effect_db.h's
// EffectDbSpecializationMask) to the profession display name and spec/core-build
// label to bucket it under. A reserved pseudo-id (>= kEffectDbCoreOnlyIdFloor)
// decodes straight to its profession via EffectDb_ProfessionFromCoreOnlyId, with
// no real spec attached (core build, no elite spec active); anything below that
// decodes through specialization_info.h, falling back to a raw "Spec #N" label if
// the id isn't in that table yet (mirrors GetSpecializationInfo's "don't guess"
// contract). Same helper installed_tree_view.cpp's RenderEffectDbDetail uses;
// same copy reasoning as ForScienceGroupMemberLabel above.
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
// RenderForScienceDetail
//--------------------------------------------------------------------------------
// Same grouping RenderEffectDbDetail uses in installed_tree_view.cpp:
// duration/a4/a6/self_mask -> profession -> specialization, races as a sibling
// annotation since one occurrences row carries every race and profession+spec
// seen (see effect_db.h's EffectDbSpecializationMask). Reads
// EffectDb_GetEffect/EffectDb_GetOccurrences directly, since the Live Log panel
// has no tree json node for a guid not yet in any sin file. Omits the "GUID: ..."
// line and installedBehavior/"Configured behavior" text the tree's version and
// the ordinary branch show -- the row header already carries identity, and
// for-science data isn't this user's configured behavior.
//--------------------------------------------------------------------------------
void RenderForScienceDetail(const std::string& guid_b64)
{
    EffectDbEffect eff{};
    if (!EffectDb_GetEffect(guid_b64, eff))
    {
        ImGui::TextDisabled("No data.");
        return;
    }

    if (!eff.blockGroup.empty() || !eff.blockMember.empty())
        ImGui::Text("Block: %s.%s   Type: %d", eff.blockGroup.c_str(), eff.blockMember.c_str(), eff.type);
    else
        ImGui::TextDisabled("Block: (none on this line)   Type: %d", eff.type);

    std::vector<EffectDbOccurrence> occs = EffectDb_GetOccurrences(guid_b64);
    if (occs.empty())
    {
        ImGui::TextDisabled("No occurrences recorded yet.");
        return;
    }

    static const char* kSelfMaskLabels[] = { "none", "target", "caster", "both" };

    //_ Keyed by (duration, a4) -- group rows share an occurrence's own signature.
    std::vector<EffectDbGroupInstance>   started  = EffectDb_GetGroupsStarted(guid_b64);   //. maps below point into this
    std::vector<EffectDbGroupMembership> memberOf = EffectDb_GetGroupsMemberOf(guid_b64);  //. maps below point into this

    std::map<std::pair<int, unsigned int>, const EffectDbGroupInstance*> startedByDurationA4;
    for (const auto& inst : started)
        startedByDurationA4[{ inst.duration, inst.a4 }] = &inst;

    std::multimap<std::pair<int, unsigned int>, const EffectDbGroupMembership*> memberOfByDurationA4;
    for (const auto& m : memberOf)
        memberOfByDurationA4.emplace(std::make_pair(m.duration, m.a4), &m);

    //_ std::map for stable order -- TreeNode open/closed state must persist.
    struct SignatureGroup
    {
        std::map<std::string, std::set<std::string>> specsByProfession;   //. profession -> specs seen
        std::set<std::string> racesSeen;                                  //. races seen under signature
    };
    std::map<std::tuple<int, unsigned int, std::string, int>, SignatureGroup> groups;

    for (const auto& occ : occs)
    {
        SignatureGroup& group = groups[{ occ.duration, occ.a4, occ.a6, static_cast<int>(occ.self_mask) }];

        for (unsigned int id : EffectDb_SpecOrCoreIdsInMask(occ.specializationMask))
        {
            std::string profName, specLabel;
            DecodeSpecOrCoreId(id, profName, specLabel);
            group.specsByProfession[profName].insert(specLabel);
        }
        for (Mumble::ERace race : EffectDb_RacesInMask(occ.raceMask))
            group.racesSeen.insert(GameState_RaceName(race));
    }

    int groupIdx = 0;
    for (const auto& [sig, group] : groups)
    {
        const auto& [duration, a4, a6, selfMask] = sig;
        const char* selfLabel = (selfMask >= 0 && selfMask <= 3) ? kSelfMaskLabels[selfMask] : "?";

        ImGui::PushID(groupIdx++);
        if (ImGui::TreeNode("occgroup", "duration:%d  a4:%u  a6:%s  self:%s",
                             duration, a4, a6.empty() ? "null" : a6.c_str(), selfLabel))
        {
            //_ Starter and member rows merge here -- a guid can match both.
            auto startedIt     = startedByDurationA4.find({ duration, a4 });
            auto memberOfRange = memberOfByDurationA4.equal_range({ duration, a4 });
            bool hasStarted     = startedIt != startedByDurationA4.end();
            bool hasMemberOf    = memberOfRange.first != memberOfRange.second;

            if (hasStarted || hasMemberOf)
            {
                int memberCount = (hasStarted ? static_cast<int>(startedIt->second->memberGuids.size()) : 0)
                    + static_cast<int>(std::distance(memberOfRange.first, memberOfRange.second));

                if (ImGui::TreeNode("groupmembers", "Group members (%d)", memberCount))
                {
                    if (hasStarted)
                    {
                        for (const auto& member : startedIt->second->memberGuids)
                            ImGui::BulletText("%s", ForScienceGroupMemberLabel(member).c_str());
                    }

                    //_ Labeled by starter guid -- only ID a member_of row has.
                    for (auto it = memberOfRange.first; it != memberOfRange.second; ++it)
                        ImGui::BulletText("%s (started this group)",
                                           ForScienceGroupMemberLabel(it->second->starterGuid_b64).c_str());

                    ImGui::TreePop();
                }
            }

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
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GroupStripColor
//--------------------------------------------------------------------------------
// groupId < 0 (not currently part of a group, see live_log.h/.cpp) gets a dim
// neutral gray; otherwise cycles through a small fixed palette. Colors are reused
// once groupId wraps past the palette length -- fine, since only groups visible
// in the list at the same time need to read as distinct, and this is a per-row
// hint, not a rigorous unique-ID color.
//--------------------------------------------------------------------------------
ImVec4 GroupStripColor(int groupId)
{
    if (groupId < 0)
        return ImVec4(0.35f, 0.35f, 0.38f, 1.0f);

    static const ImVec4 kPalette[] = {
        ImVec4(0.31f, 0.72f, 0.79f, 1.0f),
        ImVec4(0.70f, 0.54f, 0.91f, 1.0f),
        ImVec4(0.91f, 0.63f, 0.31f, 1.0f),
        ImVec4(0.50f, 0.79f, 0.37f, 1.0f),
        ImVec4(0.91f, 0.44f, 0.60f, 1.0f),
        ImVec4(0.44f, 0.57f, 0.91f, 1.0f),
        ImVec4(0.79f, 0.65f, 0.31f, 1.0f),
        ImVec4(0.37f, 0.88f, 0.75f, 1.0f),
    };
    return kPalette[groupId % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderGroupMatchStrip
//--------------------------------------------------------------------------------
// The colored strip in the left margin, one segment per distinct group `entry`
// recently belonged to (oldest left, newest right, capped at
// kLiveLogGroupHistoryCap), plus its hover tooltip. Shared by both branches since
// entry->recentGroupIds/groupId are populated identically for both stores
// (ApplyGroupHistory, shared by IngestLogLine's ordinary entry and
// UpdateForScienceEntry -- see live_log.cpp). Returns the strip's pixel width;
// both callers use it to decide how much to Indent by.
//--------------------------------------------------------------------------------
float RenderGroupMatchStrip(const LiveLogEntry& entry)
{
    constexpr float kSegW   = 3.0f;
    constexpr float kSegGap = 1.0f;
    const auto&     history = entry.recentGroupIds;
    int segCount = history.empty() ? 1 : static_cast<int>(history.size());
    float stripWidth = segCount * kSegW + (segCount - 1) * kSegGap;

    ImVec2 stripMin  = ImGui::GetCursorScreenPos();
    float  rowHeight = ImGui::GetFrameHeight();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    if (history.empty())
    {
        drawList->AddRectFilled(
            stripMin, ImVec2(stripMin.x + kSegW, stripMin.y + rowHeight),
            ImGui::ColorConvertFloat4ToU32(GroupStripColor(-1)));
    }
    else
    {
        float x = stripMin.x;
        for (int gid : history)
        {
            drawList->AddRectFilled(
                ImVec2(x, stripMin.y), ImVec2(x + kSegW, stripMin.y + rowHeight),
                ImGui::ColorConvertFloat4ToU32(GroupStripColor(gid)));
            x += kSegW + kSegGap;
        }
    }

    if (ImGui::IsMouseHoveringRect(stripMin, ImVec2(stripMin.x + stripWidth, stripMin.y + rowHeight)))
    {
        if (history.empty())
        {
            ImGui::SetTooltip("Not part of a group");
        }
        else
        {
            std::string tip = "Groups: ";
            for (size_t i = 0; i < history.size(); ++i)
            {
                if (i) tip += " -> ";
                tip += std::to_string(history[i]);
            }
            if (entry.groupId < 0)
                tip += " (currently ungrouped)";
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }

    return stripWidth;
}

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderLiveLogSection
//--------------------------------------------------------------------------------
// Own collapsible header, separate from Installed Effects / Backups / Report an
// Effect -- this is live incoming data over the Nexus event bridge
// (live_log.h/vfxd_sins_bridge.h), not anything read off disk.
//
// The per-type "log this at all" filters below are checked at ingestion and are
// independent of the listen toggle and "hide known". Not persisted -- they reset
// to built-in defaults every reload (see live_log.cpp), since they're exploratory
// filters for characterizing each numeric type, not settings meant to stick.
//--------------------------------------------------------------------------------
void RenderLiveLogSection(AddonAPI_t* aApi, const std::string& denoiserAddonDir)
{
    //_ Same lazy-load pattern as RenderReportSection/RenderBackupsSection
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    //_ Cached and gated on tree generation, same cost concern as s_overlayCache.
    static std::unordered_map<std::string, std::string> s_guidNameCache;
    static std::unordered_map<std::string, std::string> s_guidBehaviorCache;
    static int s_guidCacheGeneration = -1;
    if (s_guidCacheGeneration != GetInstalledTreeGeneration())
    {
        s_guidNameCache        = CollectGuidNameMap();
        s_guidBehaviorCache    = CollectGuidBehaviorMap();
        s_guidCacheGeneration  = GetInstalledTreeGeneration();
    }
    LiveLog_SetKnownGuidNames(s_guidNameCache);
    LiveLog_SetKnownGuidBehaviors(s_guidBehaviorCache);

    //_ Tooltip text per log type; rendered in two rows of 6 below.
    static const char* const kTypeTooltips[kLiveLogTypeCount] = {
        "Type 0: never visible, sometimes linked to sounds.",
        "Type 1: a group -- hiding this hides all the effects of that group.",
        "Type 2: most times invisible; only one occasion found related to a visible effect.",
        "Type 3: only one encounter so far, as a follow-up effect.",
        "Type 4: tether between 2 entities.",
        "Type 5: effects that change the color of the body (infusions, stealth, etc).",
        "Type 6: most effects use this.",
        "Type 7: never encountered yet.",
        "Type 8: only invisible so far, rare.",
        "Type 9: only invisible so far, most times at the end of projectile effects.",
        "Type 10: most times weapon trails.",
        "Type 11: same as type 1, maybe a newer implementation.",
    };

    ImGui::TextDisabled("Types logged:");
    for (int t = 0; t < kLiveLogTypeCount; ++t)
    {
        ImGui::PushID(t);
        bool enabled = LiveLog_GetTypeEnabled(t);
        char label[8];
        std::snprintf(label, sizeof(label), "%d", t);
        if (ImGui::Checkbox(label, &enabled))
            LiveLog_SetTypeEnabled(t, enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", kTypeTooltips[t]);
        ImGui::PopID();
        if (t != kLiveLogTypeCount - 1 && (t % 6) != 5)
            ImGui::SameLine();
    }
    ImGui::Separator();

    bool listening = LiveLog_IsListening();
    if (ImGui::Checkbox("Capture live (VfxDenoiser)", &listening))
        LiveLog_SetListening(aApi, listening);

    bool hideKnown = LiveLog_GetHideKnown();
    if (ImGui::Checkbox("Hide effects already in a sin file", &hideKnown))
        LiveLog_SetHideKnown(hideKnown);

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        LiveLog_Clear();

    if (!listening)
        ImGui::TextDisabled("Not capturing -- toggle \"Capture live\" above while VfxDenoiser is running.");

    ImGui::Separator();

    //_ Gated on VfxD_Greed.json existing by hand -- never auto-created.
    bool greedExists = EffectDb_GreedFileExists(denoiserAddonDir);
    if (!greedExists)
    {
        ImGui::TextDisabled("For science (excessive logging) -- unavailable");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Create VfxD_Greed.json by hand next to your other sin files to enable this.\n"
                "Not created automatically -- this is for people who know exactly what they're doing.");
    }
    else
    {
        bool forScience = EffectDb_IsEnabled();
        if (ImGui::Checkbox("For science (excessive logging)", &forScience))
            EffectDb_SetEnabled(forScience, denoiserAddonDir);

        if (forScience)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                "Recording every self-caused effect to a permanent local database -- all classes, "
                "durations, and other fields, not just what's shown below. This accumulates across "
                "every session; it's not cleared by \"Clear\" above.");
            if (!listening)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "\"Capture live\" above is off -- nothing is actually being recorded right now.");
        }

        //_ Db capture stream (self, all types); hidden while background-only is on.
        bool backgroundOnly = LiveLog_GetForScienceOnly();
        if (!backgroundOnly)
        {
            ImGui::Checkbox("Show for-science log (self only, all types, tree-style data)", &s_forScienceView);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "The list below becomes the effect db's own capture stream: every self-cast "
                    "event regardless of the type filters or \"hide known\" above, with the same "
                    "block/occurrence detail the Installed Effects tree shows for \"for science\" data.");
        }

        ImGui::Spacing();

        //_ Toggles capture+listening+ForScienceOnly; that skips, not hides, s_entries.
        if (ImGui::Checkbox("Background only -- capture silently, skip the live log entirely", &backgroundOnly))
        {
            LiveLog_SetForScienceOnly(backgroundOnly);
            LiveLog_SetListening(aApi, backgroundOnly);
            EffectDb_SetEnabled(backgroundOnly, denoiserAddonDir);
            s_forScienceView = false; //. unused while background-only
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Turns on \"Capture live\" and \"For science\" together and records every "
                "self-caused effect, every type, straight to the permanent database -- but "
                "the on-screen list below (either version) is skipped entirely, so it never "
                "accumulates entries or gets walked every frame. Turn this off to stop both.");

        if (backgroundOnly)
        {
            ImGui::TextDisabled("Capturing silently in the background -- nothing shown below.");
            return;
        }
    }

    ImGui::Separator();

    const bool forScienceView = greedExists && s_forScienceView;
    const auto& entries = forScienceView ? LiveLog_GetForScienceEntries() : LiveLog_GetEntries();
    if (entries.empty())
    {
        ImGui::TextDisabled("Nothing captured yet.");
        return;
    }

    //_ Sorted by firstSeenSeq so an updating entry doesn't reshuffle the list.
    std::vector<const LiveLogEntry*> sorted;
    sorted.reserve(entries.size());
    for (const auto& [guid, entry] : entries)
        sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(), [](const LiveLogEntry* a, const LiveLogEntry* b)
    {
        return a->firstSeenSeq < b->firstSeenSeq;
    });

    for (const LiveLogEntry* entry : sorted)
    {
        ImGui::PushID(entry->guid_b64.c_str());

        if (forScienceView)
        {
            float stripWidth = RenderGroupMatchStrip(*entry);
            ImGui::Indent(stripWidth + 4.0f);

            //_ Db name if quick-edited, else raw guid -- never sin JSON name here.
            EffectDbEffect eff{};
            bool dbKnown = EffectDb_GetEffect(entry->guid_b64, eff);
            std::string label = (dbKnown && !eff.name.empty()) ? eff.name : entry->guid_b64;

            bool open = ImGui::TreeNode(label.c_str());

            //_ Replaces "report" in this branch -- shown only while !knownInSin.
            if (!entry->knownInSin)
            {
                ImGui::SameLine();
                if (s_quickEditGuid == entry->guid_b64)
                {
                    ImGui::SetNextItemWidth(160);
                    ImGui::InputText("##quickEditName", s_quickEditBuf, sizeof(s_quickEditBuf));
                    ImGui::SameLine();
                    if (ImGui::SmallButton("apply"))
                    {
                        EffectDb_SetName(entry->guid_b64, s_quickEditBuf);
                        s_quickEditGuid.clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x"))
                        s_quickEditGuid.clear();
                }
                else if (ImGui::SmallButton("quick edit"))
                {
                    s_quickEditGuid = entry->guid_b64;
                    std::snprintf(s_quickEditBuf, sizeof(s_quickEditBuf), "%s",
                                  dbKnown ? eff.name.c_str() : "");
                }
            }

            if (open)
            {
                RenderForScienceDetail(entry->guid_b64);
                ImGui::TreePop();
            }

            ImGui::Unindent(stripWidth + 4.0f);
        }
        else
        {
            float stripWidth = RenderGroupMatchStrip(*entry);

            ImGui::Indent(stripWidth + 4.0f);
            bool open = ImGui::TreeNode(entry->displayName.c_str());

            //_ Offered even if knownInSin -- dedup is server-side; this sin may be stale.
            ImGui::SameLine();
            if (ImGui::SmallButton("report"))
                AddReportRowFromLiveLogEntry(*entry);

            if (open)
            {
                //_ Shown for knownInSin only -- unknowns already show guid as their label.
                if (entry->knownInSin)
                {
                    ImGui::Text("GUID: %s", entry->guid_b64.c_str());
                    //_ From the sin JSON -- VfxDenoiser no longer resolves this itself.
                    ImGui::Text("Configured behavior: %s",
                                 entry->installedBehavior.empty() ? "(not configured)" : entry->installedBehavior.c_str());
                }

                //_ a4/a6 stay internal-only -- only Type/Duration/Target/Caster show here.
                if (ImGui::TreeNode("Data"))
                {
                    ImGui::Text("Type: %d", entry->type);
                    ImGui::Text("Duration: %d", entry->duration);
                    ImGui::Text("Target: %s", entry->target.c_str());
                    ImGui::Text("Caster: %s", entry->caster.c_str());
                    ImGui::TreePop();
                }

                //_ Persists once self-seen (IngestLogLine); spec may show a raw id at first.
                if (entry->hasSelfContext && ImGui::TreeNode("Self (last seen)"))
                {
                    ImGui::Text("MapID: %u", entry->mapID);
                    ImGui::Text("Race: %s", GameState_RaceName(entry->race));
                    ImGui::Text("Profession: %s", GameState_ProfessionName(entry->profession));
                    if (const char* specName = SpecializationName(entry->specialization))
                        ImGui::Text("Specialization: %s", specName);
                    else
                        ImGui::Text("Specialization: %u", entry->specialization);
                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }

            ImGui::Unindent(stripWidth + 4.0f);
        }

        ImGui::PopID();
    }
}