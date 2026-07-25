#pragma once
#include "merge.h"
#include "Nexus.h"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Checks Xen0phy/VfxD_Visual_Sins' latest GitHub release for newer versions
// of whichever Visual Sins effect files the user has installed in
// <GW2>/addons/VfxDenoiser, and (on request) downloads + merges them.
//
// Same overall shape as the GW2 API poller this project was modeled on:
//   - all networking happens on a short-lived detached background thread,
//     never the render thread.
//   - an atomic in-flight flag prevents overlapping requests.
//   - a failed check never wipes out a previously-known-good result; it
//     just leaves whatever was already known alone, so the button never
//     flickers because of a transient network hiccup.
//   - registered as a shutdown hook so an addon unload doesn't have to
//     wait out a hung WinHTTP call.
//
// Unlike the reference project, this isn't a polling loop on a timer --
// GitHub's unauthenticated rate limit (60 requests/hour/IP) is much
// tighter than the GW2 API's, so this is meant to be triggered once on
// addon load and again via an explicit "recheck" action, not on any
// automatic interval.
// ---------------------------------------------------------------------------

enum class ESinUpdateState
{
    Unknown,         // not checked yet this session
    NotInstalled,    // no VfxD_<Sin>*.json found in denoiserAddonDir at all
    UpToDate,        // installed version >= latest release version
    UpdateAvailable, // installed version < latest release version
};

struct SinUpdateInfo
{
    std::string     sinName;             // "Gluttony" / "Pride" / "Sloth"
    // installedPath/installedVersion are only meaningful when state isn't
    // NotInstalled -- a successful check now always produces one entry per
    // kSinNames (see sin_files.h), regardless of whether that sin is
    // actually present in denoiserAddonDir, so the options panel can offer
    // an "Install" action for one the user doesn't have yet.
    std::string     installedPath;       // full path to the file currently on disk
    int             installedVersion = -1;
    int             latestVersion    = -1; // -1 until a successful check lands
    std::string     latestDownloadUrl;     // asset's browser_download_url, for StartApplyUpdate/StartInstallSin
    ESinUpdateState state = ESinUpdateState::Unknown;
};

enum class ECheckStatus
{
    Idle,
    Checking,
    Done,
    Error, // last check failed outright (network/parse) -- previous Done data, if any, is left as-is
};

enum class EApplyStatus
{
    Idle,
    Applying,
    Done,
    Error,
};

// Kicks off a background check against GitHub for every sin file found in
// `denoiserAddonDir`. No-op if a check or an apply is already in flight.
// Never blocks the calling (render) thread.
//
// This only hits the lightweight GitHub releases-list API call -- it does
// NOT download any effect-file JSON. If alsoLoadDiff is true, a
// StartLoadDiff call is chained on once the check completes and finds at
// least one update; pass false (the default) for checks that should stay
// cheap and silent, e.g. the one on addon load, where the options panel's
// top-of-panel action row should only show "Update available" (letting the
// user's own click on that button trigger the actual download) rather than
// eagerly downloading anything.
void StartUpdateCheck(const std::string& denoiserAddonDir, bool alsoLoadDiff = false);

// Render-thread-safe snapshot of the last completed (or in-progress) check.
// Cheap -- a mutex-guarded copy of a handful of small structs, not a
// network call. Safe to call every frame.
ECheckStatus GetCheckStatus();
std::vector<SinUpdateInfo> GetSinUpdateInfo();

// True if at least one installed sin file has a newer version available --
// exactly the condition that should show/hide the
// "Visual Sins update available" button.
bool IsAnyUpdateAvailable();

// ---------------------------------------------------------------------------
// Diff loading -- downloads the new file for every currently
// UpdateAvailable sin, loads the corresponding local file, and resolves a
// MergePlan for each (see merge.h) WITHOUT writing anything to disk.
//
// Only ever runs when explicitly asked for -- a user clicking the
// top-of-panel "Update available" button for that sin (which also covers
// its own retry: Error/Blocked states relabel to a clickable retry, same
// button). The GitHub asset download itself is a CDN
// redirect, not a GitHub REST call, so it doesn't cost anything against
// the 60/hour API rate limit -- the reason this stays opt-in per call site
// is to avoid burning the user's bandwidth/disk-write budget silently on
// every addon load, not to protect the rate limit.
//
// No-op if a check/apply/diff-load is already running. Safe to call again
// later -- it always replaces whatever was cached for each sin it touches.
//
// onlySinName, if non-empty, restricts this call to just that one sin
// (still only if it's currently UpdateAvailable) instead of every
// currently-outdated one -- lets a single sin's "load the diff" action
// (the top-of-panel action row) avoid re-downloading/re-resolving every
// other outdated sin's diff too.
// ---------------------------------------------------------------------------
enum class EDiffStatus
{
    NotLoaded, // no diff requested yet for this sin this session
    Loading,
    Ready,     // plan below is safe to display and to pass to StartApplyUpdate
    Error,
    Blocked,   // installed file has a duplicate guid (see merge.h's
               // FindDuplicateGuids) -- StartLoadDiff deliberately skips the
               // network call entirely for this sin rather than downloading
               // anything it can't safely resolve a trustworthy plan
               // against. Never Ready; StartApplyUpdate's own Ready-only
               // check already refuses this sin as a result.
};

struct SinDiffInfo
{
    std::string  sinName;
    EDiffStatus  status = EDiffStatus::NotLoaded;
    MergePlan    plan;      // meaningful only when status == Ready
};

void StartLoadDiff(const std::string& denoiserAddonDir, const std::string& onlySinName = "");

// Render-thread-safe snapshot, like GetSinUpdateInfo(). Cheap copy of
// small structs (MergePlan is just names/guid strings/paths), safe every
// frame.
std::vector<SinDiffInfo> GetSinDiffInfo();

// Applies the already-Ready diff for exactly one sin: backs up the
// installed file, applies the cached MergePlan to the cached old-file json
// (both captured back when StartLoadDiff ran -- not re-downloaded or
// re-decided here), writes to a .tmp file, and renames it over the final
// versioned name. No-op if that sin's diff isn't currently Ready, or if a
// check/apply/diff-load is already running.
//
// After this completes, call StartUpdateCheck again to confirm everything
// settled at UpToDate -- this function does not flip the cached update
// state itself, to keep "what's actually on disk" and "what the UI shows"
// verified via the same code path rather than trusted blindly.
void StartApplyUpdate(const std::string& denoiserAddonDir, const std::string& sinName);

// Downloads and writes a sin file that ISN'T currently installed -- no
// merge, no backup, nothing to preserve, since there's no existing local
// file to reconcile against. Only proceeds if the last completed check
// has this sinName in state NotInstalled with a known latestDownloadUrl;
// no-op otherwise (including if a check/apply/diff-load is already in
// flight -- same single in-flight guard as every other Start* here).
// Writes straight to a versioned "VfxD_<Sin>-v<N>.json" via the same
// tmp-file-then-rename safety path StartApplyUpdate uses, then triggers a
// StartUpdateCheck so the options panel re-verifies against what's
// actually on disk rather than trusting the write blindly.
void StartInstallSin(const std::string& denoiserAddonDir, const std::string& sinName);

EApplyStatus GetApplyStatus();

// The most recent one-line human-readable outcome, e.g. an error message
// or "Updated Gluttony, Pride." Empty if nothing to report yet.
std::string GetLastApplyMessage();

// Registered as a shutdown hook (see entry.cpp) so an addon unload/game
// close doesn't have to wait out a hung WinHTTP call.
void CancelInFlightUpdateRequest();

// Hands this file the Nexus AddonAPI so failures that happen on the
// background thread (network errors, and disk-write failures while
// applying an update) can also go to Nexus's own log, not just to
// GetLastApplyMessage()/the options panel. Call once from Addon_Load,
// before any of the Start* functions above can run. Pass nullptr to
// disable logging (e.g. if Addon_Load never got an aApi).
//
// The stored pointer is only ever read, never reassigned concurrently with
// a Start* call, so no locking guards it -- same assumption s_denoiserAddonDir
// already relies on elsewhere in this addon.
void SetUpdaterLogger(AddonAPI_t* aApi);
