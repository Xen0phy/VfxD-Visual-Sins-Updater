#pragma once
#include <string>

// ---------------------------------------------------------------------------
// "Backups" options-panel section, split out of addon.cpp. Pairs with
// backup.h the same way
// addon.cpp used to: backup.h/.cpp own scanning/restoring .bak files,
// this file owns the list UI and per-entry "Roll back" button.
// ---------------------------------------------------------------------------

// Draws the "Backups" collapsing header's contents. Lazily loads the
// installed-effects tree via installed_tree_store.h if it isn't loaded yet
// (needed so a rollback can find the sin's *current* on-disk filename, in
// case it differs from the backup's after an applied update).
//
// denoiserAddonDir: passed in rather than read from a global, same
// convention as RenderReportSection -- addon.cpp remains the sole owner
// of s_denoiserAddonDir.
void RenderBackupsSection(const std::string& denoiserAddonDir);
