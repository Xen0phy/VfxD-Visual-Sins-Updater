// live_log_ui.cpp
//
// "Live Log (VfxDenoiser)" options-panel section. Extracted from
// addon.cpp -- a mechanical move, no
// behavior change. See live_log_ui.h for what's exposed and why.
#include "ui/live_log_ui.h"
#include "core/live_log.h"
#include "ui/report_ui.h"
#include "imgui.h"
#include "core/tree/installed_tree_store.h"
#include "core/game_state.h"
#include "core/specialization_names.h"
#include <algorithm>
#include <cstdio>
#include <vector>

// Own collapsible header, separate from Installed Effects / Backups /
// Report an Effect -- this is live incoming data over the Nexus event
// bridge (live_log.h/vfxd_sins_bridge.h), not anything read off disk.
void RenderLiveLogSection(AddonAPI_t* aApi, const std::string& denoiserAddonDir)
{
    // Same lazy-load-if-needed pattern as RenderReportSection/
    // RenderBackupsSection -- resolving an incoming guid to a sin effect
    // name needs whatever's actually installed right now.
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);
    LiveLog_SetKnownGuidNames(CollectGuidNameMap());
    LiveLog_SetKnownGuidBehaviors(CollectGuidBehaviorMap());

    // Per-type "log this at all" filters, checked at ingestion (drop
    // before ever becoming/updating an entry) -- independent of the
    // listen toggle and "hide known" below. Deliberately not persisted:
    // resets to the built-in defaults every addon reload (see
    // live_log.cpp), since these are exploratory filters for
    // characterizing what each numeric type actually is, not settings
    // meant to stick. Wrapped to two rows of 6 rather than one long row.
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
        ImGui::TextDisabled("Not capturing -- toggle \"Capture live\" above while VfxDenoiser's patch (or the test stub) is running.");

    const auto& entries = LiveLog_GetEntries();
    if (entries.empty())
    {
        ImGui::TextDisabled("Nothing captured yet.");
        return;
    }

    // Rendered in the order each guid was first received (LiveLogEntry::
    // firstSeenSeq, assigned once on first sight and never touched again
    // by later "latest wins" updates) -- deliberately not alphabetical or
    // any other re-derived order, so the list doesn't reshuffle every time
    // an already-seen entry's fields update.
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
        bool open = ImGui::TreeNode(entry->displayName.c_str());

        // Hidden for a GUID already in an installed sin file (knownInSin)
        // -- unchanged existing rule. Shown next to the row regardless of
        // whether it's expanded, since a whole-row action shouldn't
        // require opening the tree first. No "already added to this
        // pending report" guard needed here -- report.cpp's
        // reject-whole-submission-on-duplicate-guid check (client-side,
        // at send time) already covers accidentally adding the same GUID
        // twice.
        if (!entry->knownInSin)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("report new"))
                AddReportRowFromLiveLogEntry(*entry);
        }

        if (open)
        {
            // Shown either way once unfolded, just secondary when a name
            // already exists -- unknown entries already show the guid as
            // their collapsed-row label.
            if (entry->knownInSin)
            {
                ImGui::Text("GUID: %s", entry->guid_b64.c_str());
                // Looked up independently against *this user's* installed
                // sin JSON -- not read off the incoming event. This is the
                // trustworthy one for a known guid; VfxDenoiser's own
                // event-side resolution is gone entirely (see live_log.h).
                ImGui::Text("Configured behavior: %s",
                             entry->installedBehavior.empty() ? "(not configured)" : entry->installedBehavior.c_str());
            }

            // a4/a6 stay internal-only (semantically opaque, never
            // rendered) -- only Type/Duration/Target/Caster show here.
            if (ImGui::TreeNode("Data"))
            {
                ImGui::Text("Type: %d", entry->type);
                ImGui::Text("Duration: %d", entry->duration);
                ImGui::Text("Target: %s", entry->target.c_str());
                ImGui::Text("Caster: %s", entry->caster.c_str());
                ImGui::TreePop();
            }

            // Only built once this GUID has ever had a self-event
            // (hasSelfContext), not based on the *current* caster/target --
            // "last seen" means it stays visible even if this same effect
            // is later logged by/against someone else. mapID/race/
            // profession/spec are only ever written on a self-event to
            // begin with (see IngestLogLine's isSelfEvent branch) and are
            // never cleared afterward, so once true this section always
            // has real data to show. Race/Profession are named directly
            // from Mumble.h's own enum (see game_state.cpp) -- always a
            // real name. Specialization falls back to the raw numeric id
            // until specialization_names.cpp's table is filled in (see
            // that file for why it's still empty).
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
        ImGui::PopID();
    }
}
