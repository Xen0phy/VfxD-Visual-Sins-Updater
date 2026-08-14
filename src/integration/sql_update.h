//################################################################################
// sql_update.h
//--------------------------------------------------------------------------------
// Implements TODO_B.md item 8 ("SQL as the real update source (replaces
// GitHub)"), design settled in EFFECT_DB_D1_HANDOFF.md's companion TODO.
// Deliberately mirrors github_update.h's public contract (check -> diff ->
// apply, ESinUpdateState/EDiffStatus/SinDiffInfo all reused as-is rather
// than redefined) closely enough that a call site barely has to change --
// but every function here is SYNCHRONOUS. There's no network call
// anywhere in this path, only local SQLite reads (via effect_db.h,
// already open or opened here via EffectDb_EnsureOpenForBrowsing) and
// in-memory JSON generation (sin_generator.h) -- none of
// github_update.cpp's background-thread/atomic-in-flight-flag/shutdown-
// hook machinery exists here because nothing here can hang.
//
// "Latest version" for this path is NOT a downloaded release's filename
// version in the naive per-sin sense -- verified against real installed
// files (see tools/verify_sql_update.py's history and the conversation
// that produced this fix): a real GitHub release stamps ALL THREE sin
// filenames with the SAME shared number for a given release --
// SinGenerator_CountEmittedGuids(Gluttony), the master pool size --
// even though Sloth's own body is smaller once its Caution-category
// guids are filtered out (Pride never diverges from Gluttony's own
// count, so it can't reveal this; Sloth can and does -- a real
// VfxD_Sloth-vNNNN.json tested against a real db came back with its
// filename number 257 higher than its own body's actual guid count).
// Comparing/stamping with each sin's OWN filtered count instead would
// silently under-report available Sloth updates against any
// pre-existing GitHub-downloaded install, specifically whenever the
// Caution-tagged share of newly added guids grows faster than the
// non-Caution share -- not hypothetical, since GitHub-downloaded Sloth
// installs are exactly what this module has to interoperate with during
// the side-by-side transition period (GitHub isn't going away until D1
// is fully integrated).
//
// So: kSharedVersion (below) == SinGenerator_CountEmittedGuids(Gluttony)
// is what every sin's installed filename is compared against AND what
// every sin's newly-written filename is stamped with -- Sloth/Pride
// included, even though their own bodies may hold fewer guids than that
// number says. This matches the real GitHub convention observed, not
// just "the guid count SinGenerator_Generate would actually emit for
// that sin" read literally per-sin.
//
// A SQL count that's lower than or equal to the installed version reads
// as UpToDate, never a negative/downgrade state -- the obvious reading
// of ESinUpdateState's existing semantics, per the handoff doc.
//
// The in-file {"version": {"major", "minor"}} object generated files
// carry is a separate concept (VfxDenoiser's own compatibility version,
// currently the same {1, 10} placeholder item 7's test button already
// uses) -- unrelated to the filename-suffix version this module checks
// against. See sin_generator.h.
//
// InstallSqlSin below is this module's own addition, not spelled out in
// EFFECT_DB_D1_HANDOFF.md item 8 (which focuses on the update path) --
// added by direct symmetry with github_update.h's StartInstallSin, since
// CheckSqlUpdates reuses ESinUpdateState wholesale and that enum includes
// NotInstalled. Flagged here in case that symmetry wasn't actually
// wanted yet.
//--------------------------------------------------------------------------------

#pragma once

#include "github_update.h" //. reuses ESinUpdateState / EDiffStatus / SinDiffInfo as-is
#include "merge.h"

#include <string>
#include <vector>

//********************************************************************************
// SqlSinUpdateInfo
//--------------------------------------------------------------------------------
// sinName             "Gluttony" / "Pride" / "Sloth"
// installedPath       full path to the file currently on disk; empty if
//                     state == NotInstalled
// installedVersion    -1 if not installed
// latestVersion       SinGenerator_CountEmittedGuids(Gluttony) -- the
//                     shared master count, NOT this sin's own filtered
//                     count, so it's directly comparable to what a real
//                     GitHub release stamps into every sin's filename
//                     (see this header's top comment). Always populated
//                     (no "unknown" case, unlike github_update.h's
//                     network-dependent latestVersion), since this is a
//                     synchronous local computation.
// state               see ESinUpdateState (github_update.h, reused)
//--------------------------------------------------------------------------------
// Same shape as SinUpdateInfo minus latestDownloadUrl -- nothing is
// downloaded in this path, ApplySqlUpdate/InstallSqlSin generate content
// directly instead.
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
// Synchronous. Ensures the effect db is open for browsing (never enables
// capture -- see EffectDb_EnsureOpenForBrowsing), then for each of
// kSinNames compares SinGenerator_CountEmittedGuids against
// ScanInstalledSinFiles's parsed version for that sin, producing one
// entry per kSinNames regardless of whether it's actually installed
// (same as GetSinUpdateInfo's own contract), so an "Install" action can
// be offered for one the user doesn't have yet.
//
// outError is filled and an empty vector returned only if the effect db
// itself couldn't be opened -- there's no partial-failure case the way a
// flaky network call has, since every input here is local.
//--------------------------------------------------------------------------------
std::vector<SqlSinUpdateInfo> CheckSqlUpdates(const std::string& denoiserAddonDir, std::string& outError);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LoadSqlDiff
//--------------------------------------------------------------------------------
// Synchronous. For sinName: loads the installed file, checks it for
// duplicate guids (EDiffStatus::Blocked if any -- same guard
// StartLoadDiff applies before ever trusting a guid-first merge, see
// merge.h), generates the SQL-sourced file via
// SinGenerator_Generate(variant, 1, 10), and resolves a MergePlan against
// the installed file -- exactly the same ResolveMergePlan/ApplyMergePlan
// mechanics github_update.cpp uses, since merge.h is source-agnostic
// (neither function knows or cares whether newFile came from GitHub or
// SinGenerator_Generate).
//
// Caches everything ApplySqlUpdate needs (the loaded old file, the
// resolved plan, the shared master guid count to use as the new
// filename version -- see this header's top comment for why it's the
// master count and not this sin's own filtered count) keyed by sinName,
// so Apply applies exactly what this call resolved and displayed -- never re-generates or re-decides anything,
// same reasoning as StartApplyUpdate consuming StartLoadDiff's cache
// rather than the network response a second time.
//
// Returns a SinDiffInfo (github_update.h, reused as-is) for display;
// status is never NotLoaded/Loading in the return value since this
// always completes before returning -- those two only make sense for a
// caller's own "haven't asked yet" placeholder state.
//--------------------------------------------------------------------------------
SinDiffInfo LoadSqlDiff(const std::string& denoiserAddonDir, const std::string& sinName);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetSqlDiffInfo
//--------------------------------------------------------------------------------
// Last cached result of LoadSqlDiff for sinName -- status ==
// EDiffStatus::NotLoaded (empty plan) if LoadSqlDiff was never called
// for it this session. Cheap; safe to call every frame, same as
// GetSinDiffInfo.
//--------------------------------------------------------------------------------
SinDiffInfo GetSqlDiffInfo(const std::string& sinName);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetSqlDiffInfo (no-arg overload)
//--------------------------------------------------------------------------------
// Same no-arg shape as github_update.h's own GetSinDiffInfo() -- every
// sin with a cached (non-NotLoaded) diff, all at once. Added because
// RenderJsonTabContent (installed_tree_view.cpp) builds its diff overlay
// off exactly that shape for the GitHub path already, and originally had
// no way to find out a SQL diff existed at all: GetSqlDiffInfo(sinName)
// alone can't be iterated over without already knowing which sin names
// to ask for, so the installed-effects tree's green/orange overlay was
// silently never applied for a SQL-sourced diff -- it only ever showed
// the "N new, M refreshed" text under the SQL button itself (that text
// reads this same cache, just by single-sin key), never the tree below
// it. Found by actually running the addon, not by inspection.
//--------------------------------------------------------------------------------
std::vector<SinDiffInfo> GetSqlDiffInfo();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ApplySqlUpdate
//--------------------------------------------------------------------------------
// Applies the already-Ready cached diff for exactly one sin -- owner-
// confirmed to work exactly like a GitHub update, including the backup
// step: backs up the installed file (same .bak-then-overwrite pattern
// StartApplyUpdate/SaveInstalledSinFile already use), applies the cached
// MergePlan to the cached old-file json (both captured back when
// LoadSqlDiff ran, not regenerated here), writes to a .tmp file, and
// renames it over "VfxD_<Sin>-v<N>.json" where N is the shared master
// guid count LoadSqlDiff cached (see this header's top comment -- not
// this sin's own filtered count) -- same tmp-then-rename write-safety
// pattern as every other write in this addon.
//
// Same "for science"/apply mutual exclusion as StartApplyUpdate
// (EffectDb_IsEnabled() must be false) -- capture could be independently
// toggled on elsewhere in the options panel while a diff sits loaded, so
// this re-checks rather than trusting the state at LoadSqlDiff time.
//
// Returns false (outMessage filled with why) if that sin's cached diff
// isn't Ready, or if capture is currently enabled. Returns true
// (outMessage filled with a human-readable summary) on success. No-op on
// the cache either way beyond a successful apply, which erases it -- so
// the caller's options panel stops offering to re-apply an already-
// applied diff, mirroring StartApplyUpdate's own cache-erase-on-success.
//--------------------------------------------------------------------------------
bool ApplySqlUpdate(const std::string& denoiserAddonDir, const std::string& sinName, std::string& outMessage);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// InstallSqlSin
//--------------------------------------------------------------------------------
// NOT part of EFFECT_DB_D1_HANDOFF.md item 8's own writeup -- added here
// by direct symmetry with github_update.h's StartInstallSin, since
// CheckSqlUpdates reuses ESinUpdateState (which includes NotInstalled)
// unchanged. Writes a sin file that isn't currently installed: no merge,
// no backup, nothing to preserve, same as StartInstallSin. Generates via
// SinGenerator_Generate(variant, 1, 10) and writes straight to
// "VfxD_<Sin>-v<N>.json" where N = SinGenerator_CountEmittedGuids(Gluttony)
// -- the shared master count, same as every other write in this module
// (see this header's top comment) -- via the same tmp-then-rename path
// every other write here uses.
//
// Returns false (outMessage filled) if sinName isn't one of kSinNames,
// if it's already installed (use ApplySqlUpdate instead), or if the
// effect db can't be opened. Returns true (outMessage filled with a
// summary) on success.
//--------------------------------------------------------------------------------
bool InstallSqlSin(const std::string& denoiserAddonDir, const std::string& sinName, std::string& outMessage);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SetSqlUpdateLogger
//--------------------------------------------------------------------------------
// Same shape/purpose as SetUpdaterLogger (github_update.h) -- a failure
// on ApplySqlUpdate/InstallSqlSin's own write path is writing the user's
// actual VfxDenoiser file, surfaced loudly to Nexus's log, not just via
// outMessage. Call once from Addon_Load; pass nullptr to disable. Not
// reassigned after that, same no-lock-needed reasoning as the GitHub
// module's own copy.
//--------------------------------------------------------------------------------
void SetSqlUpdateLogger(AddonAPI_t* aApi);
