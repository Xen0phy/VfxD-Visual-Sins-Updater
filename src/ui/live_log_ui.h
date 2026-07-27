#pragma once
#include "Nexus.h"
#include <string>

// ---------------------------------------------------------------------------
// "Live Log (VfxDenoiser)" options-panel section, split out of addon.cpp.
// Pairs with live_log.h the same way addon.cpp used to: live_log.h/.cpp
// owns capture/storage of incoming events, this file owns the per-type
// filter checkboxes and the entry list UI, including the "report new"
// button that hands off to report_ui.h's AddReportRowFromLiveLogEntry.
// ---------------------------------------------------------------------------

// Draws the "Live Log (VfxDenoiser)" collapsing header's contents. Lazily
// loads the installed-effects tree via installed_tree_store.h if it isn't
// loaded yet, needed to resolve an incoming guid to a sin effect name.
//
// aApi: needed for LiveLog_SetListening's Nexus event-raise call.
// denoiserAddonDir: passed in rather than read from a global, same
// convention as RenderReportSection/RenderBackupsSection -- addon.cpp
// remains the sole owner of s_api/s_denoiserAddonDir.
void RenderLiveLogSection(AddonAPI_t* aApi, const std::string& denoiserAddonDir);
