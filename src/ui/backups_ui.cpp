// backups_ui.cpp
//
// "Backups" options-panel section. Extracted from addon.cpp -- a
// mechanical move, no behavior change. See backups_ui.h for what's
// exposed and why.
#include "ui/backups_ui.h"
#include "core/backup.h"
#include "imgui.h"
#include "core/tree/installed_tree_store.h"
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Rollback. Every write this addon makes -- applied
// update, saved edit, category rename/move -- leaves a ".bak" of what was
// there before. At most one exists per sin at a
// time (each new write overwrites the previous one), so there's never more
// than 3 total -- not enough to need a "clean up old backups" action, just
// something worth being able to undo. This lists whatever backup.h's
// ScanBackups finds and offers a per-file "Roll back". Only one backup
// generation is ever kept, which is why RestoreBackup -- and the wording
// below -- describe rollback as a swap rather than a one-way trip.
// ---------------------------------------------------------------------------
static std::string s_backupsActionMessage; // last rollback outcome, shown until the next action

void RenderBackupsSection(const std::string& denoiserAddonDir)
{
    // Same lazy-load-if-needed pattern as RenderReportSection -- this
    // section can be opened without ever expanding "Installed Effects"
    // first, but a rollback needs to know the sin's *current* on-disk
    // filename (it may differ from the backup's, after an applied update)
    // to clean up the stale file once the restore succeeds.
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    std::vector<BackupInfo> backups = ScanBackups(denoiserAddonDir);

    if (backups.empty())
    {
        ImGui::TextDisabled("No backup files -- nothing to roll back.");
        return;
    }

    ImGui::TextWrapped(
        "Every applied update, saved edit, or category rename leaves a "
        "\".bak\" of what was there just before. Rolling one back swaps it "
        "back in -- pressing \"Roll back\" again on the same entry undoes "
        "that swap, since only one backup generation is ever kept.");

    ImGui::Separator();

    for (const auto& backup : backups)
    {
        ImGui::PushID(backup.bakPath.c_str());

        ImGui::TextWrapped("%s -- backup of %s (%.1f KB)",
            backup.sinName.c_str(),
            fs::path(backup.restorePath).filename().string().c_str(),
            static_cast<double>(backup.fileSize) / 1024.0);

        if (ImGui::Button("Roll back"))
        {
            // The sin's live file might currently sit at a different path
            // than backup.restorePath (an applied update bumps the
            // version-stamped filename) -- look up whatever's actually
            // installed for this sin right now so RestoreBackup can clean
            // up the stale one after restoring. Empty if nothing's
            // currently installed for this sin at all.
            std::string currentPath;
            for (const auto& sin : GetInstalledSins())
            {
                if (sin.sinName == backup.sinName)
                {
                    currentPath = sin.fullPath;
                    break;
                }
            }

            std::string error;
            if (RestoreBackup(backup, currentPath, error))
            {
                s_backupsActionMessage = "Rolled back " + backup.sinName + ".";
                InvalidateInstalledTree(); // disk changed -- reload next expand
            }
            else
            {
                s_backupsActionMessage = "Rollback failed for " + backup.sinName + ": " + error;
            }
        }

        ImGui::PopID();
    }

    if (!s_backupsActionMessage.empty())
        ImGui::TextWrapped("%s", s_backupsActionMessage.c_str());
}
