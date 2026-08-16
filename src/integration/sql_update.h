//################################################################################
// sql_update.h
//--------------------------------------------------------------------------------
// Mirrors github_update.h's public contract (check -> diff -> apply,
// ESinUpdateState/EDiffStatus/SinDiffInfo reused as-is) closely enough that a
// call site barely has to change -- but every function here is SYNCHRONOUS: no
// network call anywhere, only local SQLite reads (effect_db.h) and in-memory JSON
// generation (sin_generator.h), so none of github_update.cpp's
// background-thread/atomic-in-flight-flag/shutdown-hook machinery is needed here.
//
// "Latest version" here means the SHARED master guid count
// (SinGenerator_CountEmittedGuids(Gluttony)), not each sin's own filtered count
// -- real GitHub releases stamp all three sin filenames with this same shared
// number (Pride never diverges from Gluttony's count and can't reveal this; Sloth
// does), so this module compares/stamps against it too, consistently, in
// CheckSqlUpdates, LoadSqlDiff, and ApplySqlUpdate/InstallSqlSin's filename
// stamping below. A SQL count <= the installed version reads as UpToDate, never a
// downgrade state -- the obvious reading of ESinUpdateState's semantics.
//
// The in-file {"version": {"major", "minor"}} content-version (see
// sin_generator.h) is a separate concept from this filename-suffix version --
// don't conflate them. InstallSqlSin is this module's own addition, added by
// direct symmetry with github_update.h's StartInstallSin.
//--------------------------------------------------------------------------------

#pragma once

#include "github_update.h" //. reuses ESinUpdateState/EDiffStatus/SinDiffInfo as-is

#include <string>
#include <vector>

//********************************************************************************
// SqlSinUpdateInfo
//--------------------------------------------------------------------------------
// sinName             "Gluttony" / "Pride" / "Sloth"
// installedPath       full path to the file currently on disk; empty if
//                     state == NotInstalled
// installedVersion    -1 if not installed
// latestVersion       shared master guid count, see file header --
//                     always populated (synchronous, no "unknown" case)
// state               see ESinUpdateState (github_update.h, reused)
//--------------------------------------------------------------------------------
// Same shape as SinUpdateInfo minus latestDownloadUrl -- nothing is downloaded in
// this path, ApplySqlUpdate/InstallSqlSin generate content directly instead.
//--------------------------------------------------------------------------------
struct SqlSinUpdateInfo
{
    std::string     sinName;
    std::string     installedPath;
    int             installedVersion = -1;
    int             latestVersion    = -1;
    ESinUpdateState state = ESinUpdateState::Unknown;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CheckSqlUpdates
//--------------------------------------------------------------------------------
// Synchronous. Ensures the effect db is open for browsing (never enables capture
// -- see EffectDb_EnsureOpenForBrowsing), then for each of kSinNames compares
// SinGenerator_CountEmittedGuids against ScanInstalledSinFiles's parsed version
// for that sin, producing one entry per kSinNames regardless of whether it's
// actually installed (same as GetSinUpdateInfo's own contract), so an "Install"
// action can be offered for one the user doesn't have yet.
//
// outError is filled and an empty vector returned only if the effect db itself
// couldn't be opened -- there's no partial-failure case the way a flaky network
// call has, since every input here is local.
//--------------------------------------------------------------------------------
std::vector<SqlSinUpdateInfo> CheckSqlUpdates(const std::string& denoiserAddonDir, std::string& outError);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LoadSqlDiff
//--------------------------------------------------------------------------------
// Synchronous. Loads the installed file for sinName, checks it for duplicate
// guids (EDiffStatus::Blocked if any -- see merge.h), builds the SQL-sourced file
// via SinGenerator_Generate(variant, 1, 10), and resolves a MergePlan against it
// -- same source-agnostic ResolveMergePlan/ApplyMergePlan mechanics
// github_update.cpp uses. Caches the old file, plan, and master guid count (see
// file header) keyed by sinName, so ApplySqlUpdate applies exactly what this call
// resolved.
//
// Returns a SinDiffInfo for display; status is never NotLoaded/Loading here since
// this always completes before returning.
//--------------------------------------------------------------------------------
SinDiffInfo LoadSqlDiff(const std::string& denoiserAddonDir, const std::string& sinName);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetSqlDiffInfo
//--------------------------------------------------------------------------------
// Last cached result of LoadSqlDiff for sinName -- status ==
// EDiffStatus::NotLoaded (empty plan) if LoadSqlDiff was never called for it this
// session. Cheap; safe to call every frame, same as GetSinDiffInfo.
//--------------------------------------------------------------------------------
SinDiffInfo GetSqlDiffInfo(const std::string& sinName);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetSqlDiffInfo (no-arg overload)
//--------------------------------------------------------------------------------
// Same no-arg shape as github_update.h's own GetSinDiffInfo() -- every sin with a
// cached (non-NotLoaded) diff, at once. Added because RenderJsonTabContent
// (installed_tree_view.cpp) needs that shape for its diff overlay and had no way
// to discover which sin names to ask GetSqlDiffInfo(sinName) for -- so the
// installed-effects tree's green/orange overlay was silently never applied for a
// SQL-sourced diff (only the "N new, M refreshed" text under the SQL button
// itself showed, since that reads the same cache by single-sin key). Found by
// running the addon, not by inspection.
//--------------------------------------------------------------------------------
std::vector<SinDiffInfo> GetSqlDiffInfo();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ApplySqlUpdate
//--------------------------------------------------------------------------------
// Applies the already-Ready cached diff for exactly one sin: backs up the
// installed file (.bak-then-overwrite, as elsewhere in this codebase), applies
// the cached MergePlan to the cached old-file json (both from LoadSqlDiff, not
// regenerated), and writes/renames to "VfxD_<Sin>-v<N>.json" where N is the
// cached master guid count (see file header). Blocked while "for science" capture
// is enabled (re-checked here since it can toggle after LoadSqlDiff).
//
// Returns false (outMessage filled) if the cached diff isn't Ready or capture is
// enabled; true on success, erasing the cache entry so the options panel stops
// offering to re-apply it.
//--------------------------------------------------------------------------------
bool ApplySqlUpdate(const std::string& denoiserAddonDir, const std::string& sinName, std::string& outMessage);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// InstallSqlSin
//--------------------------------------------------------------------------------
// Added by direct symmetry with github_update.h's StartInstallSin, since
// CheckSqlUpdates reuses ESinUpdateState (includes NotInstalled). Writes a sin
// file that isn't currently installed -- no merge, no backup, nothing to
// preserve. Generates via SinGenerator_Generate(variant, 1, 10) and writes to
// "VfxD_<Sin>-v<N>.json" where N is the shared master guid count (see file
// header), via the same tmp-then-rename path every write here uses.
//
// Returns false (outMessage filled) if sinName isn't known, is already installed
// (use ApplySqlUpdate instead), or the effect db can't open. Returns true
// (outMessage filled with a summary) on success.
//--------------------------------------------------------------------------------
bool InstallSqlSin(const std::string& denoiserAddonDir, const std::string& sinName, std::string& outMessage);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SetSqlUpdateLogger
//--------------------------------------------------------------------------------
// Same shape/purpose as SetUpdaterLogger (github_update.h) -- a failure on
// ApplySqlUpdate/InstallSqlSin's own write path is writing the user's actual
// VfxDenoiser file, surfaced loudly to Nexus's log, not just via outMessage. Call
// once from Addon_Load; pass nullptr to disable. Not reassigned after that, same
// no-lock-needed reasoning as the GitHub module's own copy.
//--------------------------------------------------------------------------------
void SetSqlUpdateLogger(AddonAPI_t* aApi);