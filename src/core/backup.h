#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Every write this addon makes (applied update, saved edit, category
// rename/move) copies its target to a `.bak` first. Those `.bak` files
// are never touched again on their own;
// this module is what finally does something with them: list them, or
// restore one.
//
// Only a single `.bak` generation ever exists per sin file (each new write
// overwrites the previous backup), so there is at most one BackupInfo per
// sin name at any given time -- this module doesn't try to keep more.
// ---------------------------------------------------------------------------

struct BackupInfo
{
    std::string sinName;      // e.g. "Gluttony" -- which sin this backup belongs to
    std::string bakPath;      // full path to the .bak file itself
    std::string bakFileName;  // just the filename, e.g. "VfxD_Gluttony-v4176.json.bak"
    std::string restorePath;  // full path the .bak would be restored to (bakPath minus ".bak")
    std::uintmax_t fileSize = 0; // size in bytes, for display ("Rollback (2.1 KB)")
};

// Scans `denoiserAddonDir` for `.bak` files belonging to any of the three
// known Visual Sins files (matched the same way ScanInstalledSinFiles
// matches the live files, plus a trailing ".bak"). Does a handful of
// filesystem stat() calls -- call on demand, not every frame.
std::vector<BackupInfo> ScanBackups(const std::string& denoiserAddonDir);

// Restores `backup` back to `backup.restorePath`, using the same
// backup-then-tmp-then-rename write-safety pattern as every other write in
// this addon (see "Safety on write") -- a crash mid-rollback can't corrupt
// anything either. The .bak's content is read into memory before anything
// is touched on disk, and restorePath (if it currently exists) is itself
// backed up over the same .bak file as part of the write-safety path -- so
// a rollback is really a swap: running it again would bring back the
// pre-rollback content. That's deliberate, not a bug, and follows from
// keeping only one backup generation.
//
// `currentInstalledPath`, if non-empty and different from restorePath (the
// case after an applied GitHub update, which writes under a new
// version-stamped filename), is removed once the restore succeeds -- best
// effort, same as github_update.cpp's own old-file cleanup after applying
// an update -- since this addon and VfxDenoiser both assume exactly one
// file per sin. Pass an empty string if there's no such file (e.g. nothing
// currently installed for this sin, or it's already at restorePath).
bool RestoreBackup(const BackupInfo& backup, const std::string& currentInstalledPath, std::string& outError);
