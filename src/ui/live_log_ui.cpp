//################################################################################
// live_log_ui.cpp
//--------------------------------------------------------------------------------
// "Live Log (VfxDenoiser)" options-panel section. Extracted from
// addon.cpp -- a mechanical move, no behavior change. See live_log_ui.h
// for what's exposed and why.
//--------------------------------------------------------------------------------

#include "effect_db.h"
#include "game_state.h"
#include "imgui.h"
#include "installed_tree_store.h"
#include "live_log_ui.h"
#include "live_log.h"
#include "report_ui.h"
#include "specialization_names.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

//_ UI-only state, not persisted -- same "resets on reload" convention as
// live_log.cpp's own s_typeEnabled defaults. s_quickEditGuid is which
// entry (if any) currently has its inline rename editor open; only one at
// a time, same shape as the tree's own Begin/Render/Apply edit states.
bool        s_forScienceView = false;
std::string s_quickEditGuid;
char        s_quickEditBuf[128] = "";

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderForScienceDetail
//--------------------------------------------------------------------------------
// The for-science branch's expanded view -- same grouping
// RenderEffectDbDetail uses in installed_tree_view.cpp (duration/a4/a6/
// self_mask -> profession -> specialization, races folded into a "races
// seen" annotation on the leaf), but reads straight from
// EffectDb_GetEffect/EffectDb_GetOccurrences rather than the tree's
// "__vfxd_db_by_guid" json, since the Live Log panel has no json node to
// read this off of for a guid that isn't (yet) in any sin file.
//
// Deliberately leaves out the "GUID: ..." line and any
// installedBehavior/"Configured behavior" text the tree's own version and
// this panel's ordinary branch both show -- this branch's row header
// already carries the guid/name identity (see RenderLiveLogSection), and
// "for science" data has nothing to do with this user's own configured
// behavior for the guid.
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

    //_ (duration, a4, a6, self_mask) -> profession -> specialization ->
    // races seen. std::map for the same stable-iteration-order reason
    // RenderEffectDbDetail's own comment gives (TreeNode open/closed
    // state needs to persist frame to frame).
    std::map<std::tuple<int, unsigned int, std::string, int>,
             std::map<std::string, std::map<std::string, std::set<std::string>>>> groups;

    for (const auto& occ : occs)
    {
        std::string profName  = GameState_ProfessionName(occ.profession);
        std::string raceName  = GameState_RaceName(occ.race);
        const char* specName  = SpecializationName(occ.specialization);
        std::string specLabel = specName ? std::string(specName) : ("Spec #" + std::to_string(occ.specialization));

        groups[{ occ.duration, occ.a4, occ.a6, static_cast<int>(occ.self_mask) }][profName][specLabel].insert(raceName);
    }

    int groupIdx = 0;
    for (const auto& [sig, byProf] : groups)
    {
        const auto& [duration, a4, a6, selfMask] = sig;
        const char* selfLabel = (selfMask >= 0 && selfMask <= 3) ? kSelfMaskLabels[selfMask] : "?";

        ImGui::PushID(groupIdx++);
        if (ImGui::TreeNode("occgroup", "duration:%d  a4:%u  a6:%s  self:%s",
                             duration, a4, a6.empty() ? "null" : a6.c_str(), selfLabel))
        {
            for (const auto& [profName, bySpec] : byProf)
            {
                if (ImGui::TreeNode(profName.c_str(), "%s", profName.c_str()))
                {
                    for (const auto& [specLabel, races] : bySpec)
                    {
                        std::string raceList;
                        for (const auto& r : races)
                        {
                            if (!raceList.empty()) raceList += ", ";
                            raceList += r;
                        }
                        ImGui::BulletText("%s  (races seen: %s)", specLabel.c_str(), raceList.c_str());
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GroupStripColor
//--------------------------------------------------------------------------------
// groupId < 0 (not currently part of a group, see live_log.h/.cpp) gets a
// dim neutral gray; otherwise cycles through a small fixed palette.
// Colors are reused once groupId wraps past the palette length -- fine,
// since only groups visible in the list at the same time need to read as
// distinct, and this is a per-row hint, not a rigorous unique-ID color.
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

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderLiveLogSection
//--------------------------------------------------------------------------------
// Own collapsible header, separate from Installed Effects / Backups /
// Report an Effect -- this is live incoming data over the Nexus event
// bridge (live_log.h/vfxd_sins_bridge.h), not anything read off disk.
//
// The per-type "log this at all" filters below are checked at ingestion
// and are independent of the listen toggle and "hide known". Deliberately
// not persisted -- they reset to built-in defaults every reload (see
// live_log.cpp), since they're exploratory filters for characterizing
// each numeric type, not settings meant to stick.
//--------------------------------------------------------------------------------
void RenderLiveLogSection(AddonAPI_t* aApi, const std::string& denoiserAddonDir)
{
    //_ Same lazy-load pattern as RenderReportSection/RenderBackupsSection
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);
    LiveLog_SetKnownGuidNames(CollectGuidNameMap());
    LiveLog_SetKnownGuidBehaviors(CollectGuidBehaviorMap());

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

    //_ Hard-gated on VfxD_Greed.json already existing by hand -- never
    // auto-created (see effect_db.h). ImGui 1.80 has no
    // BeginDisabled/EndDisabled (added in 1.89), so "unavailable" is
    // rendered as plain disabled-looking text with a tooltip rather than
    // a real disabled widget, same as the "VfxDenoiser isn't installed"/
    // "Not capturing" messages above already do.
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

        //_ Switches the list below from the ordinary type-filtered/
        // hideKnown-filtered display into the effect db's own capture
        // stream -- self only, every type, with the same block/occurrence
        // detail the tree view shows instead of Data/Self (last seen).
        // See LiveLog_GetForScienceEntries in live_log.h.
        ImGui::Checkbox("Show for-science log (self only, all types, tree-style data)", &s_forScienceView);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "The list below becomes the effect db's own capture stream: every self-cast "
                "event regardless of the type filters or \"hide known\" above, with the same "
                "block/occurrence detail the Installed Effects tree shows for \"for science\" data.");
    }

    ImGui::Separator();

    const bool forScienceView = greedExists && s_forScienceView;
    const auto& entries = forScienceView ? LiveLog_GetForScienceEntries() : LiveLog_GetEntries();
    if (entries.empty())
    {
        ImGui::TextDisabled("Nothing captured yet.");
        return;
    }

    //_ Sorted by firstSeenSeq (order first received), not alphabetical,
    // so the list doesn't reshuffle as an already-seen entry updates.
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
            //_ Prefer the db's own name once quick-edit has set one --
            // that's the whole point of the edit control below; fall back
            // to the raw guid otherwise (never the sin JSON name here,
            // since a guid with a JSON name wouldn't have the edit
            // control offered at all -- see the knownInSin check below).
            EffectDbEffect eff{};
            bool dbKnown = EffectDb_GetEffect(entry->guid_b64, eff);
            std::string label = (dbKnown && !eff.name.empty()) ? eff.name : entry->guid_b64;

            bool open = ImGui::TreeNode(label.c_str());

            //_ Replaces "report" in this branch -- offered only while this
            // guid has no name in the installed sin JSON yet (knownInSin),
            // since a guid that already has one already shows a real name
            // from elsewhere and renaming here would only ever touch the
            // db, not what the user actually sees as its name. Every guid
            // reaching this branch already has an EFFECTS row (see
            // LiveLog_GetForScienceEntries), so EffectDb_SetName below is
            // always able to succeed.
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
        }
        else
        {
            //_ Colored strip in the left margin, one segment per distinct group
            // this guid recently belonged to (oldest left, newest right, capped
            // at kLiveLogGroupHistoryCap). Never-grouped guid gets one dim-gray segment.
            constexpr float kSegW   = 3.0f;
            constexpr float kSegGap = 1.0f;
            const auto&     history = entry->recentGroupIds;
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
                    if (entry->groupId < 0)
                        tip += " (currently ungrouped)";
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }

            ImGui::Indent(stripWidth + 4.0f);
            bool open = ImGui::TreeNode(entry->displayName.c_str());

            //_ Always offered, even when knownInSin -- dedup happens
            // server-side (Cloudflare worker), and knownInSin only reflects
            // *this* user's sin file, which may be stale or a fork missing
            // an effect the original still has.
            ImGui::SameLine();
            if (ImGui::SmallButton("report"))
                AddReportRowFromLiveLogEntry(*entry);

            if (open)
            {
                //_ Shown regardless once unfolded (secondary if a name
                // already exists) -- unknown entries show the guid as
                // their collapsed-row label already.
                if (entry->knownInSin)
                {
                    ImGui::Text("GUID: %s", entry->guid_b64.c_str());
                    //_ Looked up against *this user's* installed sin JSON, not
                    // the incoming event -- the trustworthy source now that
                    // VfxDenoiser's own event-side resolution is gone.
                    ImGui::Text("Configured behavior: %s",
                                 entry->installedBehavior.empty() ? "(not configured)" : entry->installedBehavior.c_str());
                }

                //_ a4/a6 stay internal-only (opaque, never rendered) --
                // only Type/Duration/Target/Caster show here.
                if (ImGui::TreeNode("Data"))
                {
                    ImGui::Text("Type: %d", entry->type);
                    ImGui::Text("Duration: %d", entry->duration);
                    ImGui::Text("Target: %s", entry->target.c_str());
                    ImGui::Text("Caster: %s", entry->caster.c_str());
                    ImGui::TreePop();
                }

                //_ Persists once a self-event is ever seen (see IngestLogLine);
                // stays visible even if later logged by someone else.
                // Specialization may show a raw id until its table fills in.
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