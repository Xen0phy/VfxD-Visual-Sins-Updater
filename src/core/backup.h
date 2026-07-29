//################################################################################
// backup.h
//--------------------------------------------------------------------------------
// BackupInfo      one .bak file's metadata (sin name, paths, size)
// ScanBackups()   finds all .bak files under the addon's data dir
// RestoreBackup() restores a .bak back to its live path
//--------------------------------------------------------------------------------
// Every write this addon makes (applied update, saved edit, category
// rename/move) copies its target to a .bak first. Those .bak files are
// never touched again on their own -- this module is what finally does
// something with them: list them, or restore one.
//
// Only a single .bak generation ever exists per sin file (each new write
// overwrites the previous backup), so there is at most one BackupInfo per
// sin name at any given time -- this module doesn't try to keep more.
//--------------------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <string>
#include <vector>

//********************************************************************************
// BackupInfo
//--------------------------------------------------------------------------------
// sinName       which sin this backup belongs to, e.g. "Gluttony"
// bakPath       full path to the .bak file itself
// bakFileName   just the filename, e.g. "VfxD_Gluttony-v4176.json.bak"
// restorePath   full path the .bak would be restored to (bakPath minus ".bak")
// fileSize      size in bytes, for display ("Rollback (2.1 KB)")
//--------------------------------------------------------------------------------
struct BackupInfo
{
    std::string sinName;
    std::string bakPath;
    std::string bakFileName;
    std::string restorePath;
    std::uintmax_t fileSize = 0;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ScanBackups
//--------------------------------------------------------------------------------
// Scans denoiserAddonDir for .bak files belonging to any VfxD_<Name>.json
// Visual Sins file -- not limited to the update-tracked kSinNames subset
// (see sin_files.h) -- matched the same way ScanInstalledSinFiles matches
// the live files, plus a trailing ".bak". Also picks up the .bak of any
// other .json file SaveInstalledSinFile has ever written a backup for
// (i.e. any file ScanInstalledSinFiles accepted by content, not just
// name -- see its own comment); by the time a .bak exists its .json
// sibling already passed that content check once, so ScanBackups itself
// doesn't re-open the file, it just reuses the same name/version
// fallback (ExtractNameAndVersion). Does filesystem stat() calls -- call
// on demand, not every frame.
//--------------------------------------------------------------------------------
std::vector<BackupInfo> ScanBackups(const std::string& denoiserAddonDir);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// RestoreBackup
//--------------------------------------------------------------------------------
// Restores backup to backup.restorePath, using the same backup-then-tmp-
// then-rename write-safety pattern as every other write in this addon --
// a crash mid-rollback can't corrupt anything either. restorePath, if it
// currently exists, is itself backed up over the same .bak -- so a
// rollback is really a swap; running it again brings back the
// pre-rollback content.
//
// currentInstalledPath, if non-empty and different from restorePath (the
// case after an applied update, which writes under a new version-stamped
// filename), is removed best-effort once the restore succeeds. Pass an
// empty string if there's no such file.
//--------------------------------------------------------------------------------
bool RestoreBackup(const BackupInfo& backup, const std::string& currentInstalledPath, std::string& outError);