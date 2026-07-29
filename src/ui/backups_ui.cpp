//################################################################################
// backups_ui.cpp
//--------------------------------------------------------------------------------
// "Backups" options-panel section. Extracted from addon.cpp -- a mechanical
// move, no behavior change. See backups_ui.h for what's exposed and why.
//--------------------------------------------------------------------------------

#include "backup.h"
#include "backups_ui.h"
#include "imgui.h"
#include "installed_tree_store.h"

#include <filesystem>

namespace fs = std::filesystem;

static std::string s_backupsActionMessage;   //. last rollback outcome

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RenderBackupsSection
//--------------------------------------------------------------------------------
// Every write this addon makes (applied update, saved edit, category
// rename/move) leaves a ".bak" of what was there before. Only one backup
// generation is kept per sin (at most 3 total), so RestoreBackup treats a
// rollback as a swap rather than a one-way trip -- rolling back twice on
// the same entry undoes the first swap.
//--------------------------------------------------------------------------------
void RenderBackupsSection(const std::string& denoiserAddonDir)
{
    //_ Same lazy-load pattern as RenderReportSection (see backups_ui.h)
    if (!IsInstalledTreeLoaded())
        LoadInstalledEffectsTree(denoiserAddonDir);

    std::vector<BackupInfo> backups = ScanBackups(denoiserAddonDir);

    if (backups.empty())
    {
        ImGui::TextDisabled("No backup files -- nothing to roll back.");
        return;
    }

    ImGui::TextWrapped(
        "Every change is backed up once as \".bak\", so you can undo/redo just one step.");

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
            //_ Live file may sit at a different path than backup.restorePath
            // (version bump) -- find the sin's current path so RestoreBackup
            // can clean up the stale one; empty if nothing is installed.
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
                InvalidateInstalledTree();   //. disk changed, reload next expand
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