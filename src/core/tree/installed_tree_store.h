//################################################################################
// installed_tree_store.h
//--------------------------------------------------------------------------------
// LoadInstalledEffectsTree()  (re)scans and reloads every installed sin file
// InvalidateInstalledTree()   marks the cached tree stale
// GetInstalledJson()          every loaded sin's parsed JSON, keyed by name
// Find/FindMutable/Collect*   lookups and cross-file scans over that JSON
// SaveInstalledSinFile()      writes one sin's in-memory JSON back to disk
//--------------------------------------------------------------------------------
// The addon's one shared "what's actually installed" data source. Started
// out as private state inside the installed-tree UI, but it's read far
// outside that UI -- the editing subsystem, the Report/Backups/Live Log
// sections, and the diff/duplicate overlay builders all need the same
// currently-loaded sin files, not their own copies of them -- so it's its
// own module with a real API instead.
//
// Reads whatever's actually on disk via ScanInstalledSinFiles + a plain
// ifstream >> json, independent of the updater's own cached oldFile copies
// in github_update.cpp -- this is a browsing/editing concern, not part of
// the update-check/merge pipeline.
//
// Loaded lazily (first time a consumer needs it, or on an explicit
// Refresh) and cached rather than re-read from disk every frame the panel
// is open. Every mutation path funnels back through SaveInstalledSinFile
// and then calls InvalidateInstalledTree() so the next read triggers a
// clean reload from disk rather than trusting the in-memory copy.
//--------------------------------------------------------------------------------

#pragma once

#include "merge.h" //. nlohmann::ordered_json, FindDuplicateGuids
#include "Nexus.h" //. AddonAPI_t
#include "sin_files.h"

#include <string>
#include <unordered_map>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// InstalledTreeStore_SetApi
//--------------------------------------------------------------------------------
// Must be called once, with the same AddonAPI_t* entry.cpp handed to
// Addon_Init, before the first SaveInstalledSinFile call that could fail
// -- used only for aApi->Log on a write-failure path. Never reassigned
// afterward, so reading it later is safe without a lock, same as this
// module's other addon-lifetime statics.
//--------------------------------------------------------------------------------
void InstalledTreeStore_SetApi(AddonAPI_t* aApi);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LoadInstalledEffectsTree
//--------------------------------------------------------------------------------
// (Re)scans `denoiserAddonDir` and reloads every installed sin file's JSON
// into the store. A file that fails to open/parse is simply left absent
// rather than aborting the whole refresh -- callers that render a per-sin
// list already show a per-sin error line for that case, so one corrupt
// file doesn't hide the other two. Marks the tree loaded and bumps the
// generation counter (see GetInstalledTreeGeneration) on completion.
//--------------------------------------------------------------------------------
void LoadInstalledEffectsTree(const std::string& denoiserAddonDir);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// InvalidateInstalledTree
//--------------------------------------------------------------------------------
// Marks the cached tree stale, so the next IsInstalledTreeLoaded() check
// triggers a fresh LoadInstalledEffectsTree() call instead of trusting the
// in-memory copy. Called after any successful edit/apply/install, and
// after a failed save (where the in-memory copy no longer matches what's
// on disk either way) -- one named call instead of the loaded flag being
// poked directly at every call site that needs a reload.
//--------------------------------------------------------------------------------
void InvalidateInstalledTree();

bool IsInstalledTreeLoaded();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetInstalledTreeGeneration
//--------------------------------------------------------------------------------
// Bumped every time the store is (re)loaded from disk. Exists purely so
// consumers that cache derived data over the installed tree (e.g. an
// overlay painted onto a tree view) can tell "has the underlying data
// actually changed" apart from "is this the same generation I already
// built my cache from" -- tying that to one counter here covers every
// reload path for free, rather than needing an invalidation at each call
// site that mutates the tree.
//--------------------------------------------------------------------------------
int GetInstalledTreeGeneration();

const std::vector<InstalledSinFile>& GetInstalledSins();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetInstalledJson
//--------------------------------------------------------------------------------
// Every currently-loaded sin file's parsed JSON, keyed by sin name. Only
// sins that parsed OK are present -- see LoadInstalledEffectsTree.
//--------------------------------------------------------------------------------
const std::unordered_map<std::string, nlohmann::ordered_json>& GetInstalledJson();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FindInstalledJson / FindInstalledJsonMutable
//--------------------------------------------------------------------------------
// nullptr if `sinName` isn't currently loaded (either it failed to parse,
// or the tree hasn't been loaded yet at all). The mutable overload is for
// the editing subsystem, which mutates a sin's json in place (by
// re-derived category/effect path, never by a pointer carried across
// frames) before calling SaveInstalledSinFile.
//--------------------------------------------------------------------------------
const nlohmann::ordered_json* FindInstalledJson(const std::string& sinName);
nlohmann::ordered_json* FindInstalledJsonMutable(const std::string& sinName);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetDuplicateGuidsBySin
//--------------------------------------------------------------------------------
// Per-sin GUIDs that appear on more than one effect within that same file
// (see merge.h's FindDuplicateGuids doc comment for why this is checked
// against the real on-disk file, independent of whether an update is even
// available). Computed once per LoadInstalledEffectsTree call, not
// per-frame.
//--------------------------------------------------------------------------------
const std::unordered_map<std::string, std::vector<std::string>>& GetDuplicateGuidsBySin();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CollectGuidNameMap / CollectGuidBehaviorMap
//--------------------------------------------------------------------------------
// guid -> effect name / this user's own configured-behavior summary
// (Hide/Show/SetDuration, flattened to one display string per effect),
// across every currently-loaded installed sin file. A guid duplicated
// across effects (see GetDuplicateGuidsBySin) just keeps whichever value
// is visited last; that ambiguity already exists on-disk and isn't either
// map's concern to fix. Caller is responsible for the installed tree
// having been loaded at least once first.
//--------------------------------------------------------------------------------
std::unordered_map<std::string, std::string> CollectGuidNameMap();
std::unordered_map<std::string, std::string> CollectGuidBehaviorMap();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SaveInstalledSinFile
//--------------------------------------------------------------------------------
// Writes the in-memory copy of `sinName` back to the file it was loaded
// from, using a backup-then-tmp-then-rename safety pattern (never touches
// the real file directly, so a crash or failed write can't corrupt or
// lose the user's data). The one write-back path every edit operation
// funnels through. Returns false and fills outError on any failure;
// doesn't invalidate the store itself -- callers decide whether/when to
// call InvalidateInstalledTree() after this returns.
//--------------------------------------------------------------------------------
bool SaveInstalledSinFile(const std::string& sinName, std::string& outError);