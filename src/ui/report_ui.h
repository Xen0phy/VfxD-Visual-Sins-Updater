//################################################################################
// report_ui.h
//--------------------------------------------------------------------------------
// RenderReportSection(dir)          draws the "Report an Effect" section
// AddReportRowFromLiveLogEntry(e)   appends a form row from a live-log entry
//--------------------------------------------------------------------------------
// "Report an Effect" options-panel section, split out of addon.cpp. Pairs
// with report.h the same way addon.cpp used to: report.h/.cpp own
// validation and the actual network send, this file owns the form widgets
// and composing each GUID's human-readable display block.
//--------------------------------------------------------------------------------

#pragma once

#include "live_log.h"

#include <string>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderReportSection
//--------------------------------------------------------------------------------
// Draws the "Report an Effect" collapsing header's contents. Lazily loads
// the installed-effects tree via installed_tree_store.h if it isn't loaded
// yet (same pattern as RenderBackupsSection/RenderLiveLogSection), so
// per-GUID display names have something to check against even if the user
// opens this section before ever expanding "Installed Effects".
//
// denoiserAddonDir is passed in rather than read from a global, same
// convention installed_tree_store.h's LoadInstalledEffectsTree already
// established -- addon.cpp is still the sole owner of s_denoiserAddonDir.
//--------------------------------------------------------------------------------
void RenderReportSection(const std::string& denoiserAddonDir);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// AddReportRowFromLiveLogEntry
//--------------------------------------------------------------------------------
// Appends a report-form row auto-filled from a live-log entry, and
// (re-)syncs the reporter-identity name fields from GameState. This is
// what the Live Log section's per-entry "report" button calls --
// exposed here (rather than kept file-local) specifically so
// live_log_ui.cpp, a separate translation unit, can reach it.
//--------------------------------------------------------------------------------
void AddReportRowFromLiveLogEntry(const LiveLogEntry& entry);