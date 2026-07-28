//################################################################################
// github_update.h
//--------------------------------------------------------------------------------
// Checks Xen0phy/VfxD_Visual_Sins' latest GitHub release for newer
// versions of whichever Visual Sins effect files the user has installed
// in <GW2>/addons/VfxDenoiser, and (on request) downloads + merges them.
//
// Same overall shape as the GW2 API poller this project was modeled on:
// all networking happens on a short-lived detached background thread,
// never the render thread; an atomic in-flight flag prevents overlapping
// requests; a failed check never wipes out a previously-known-good
// result, it just leaves whatever was already known alone, so the
// button never flickers because of a transient network hiccup;
// registered as a shutdown hook so an addon unload doesn't have to wait
// out a hung WinHTTP call.
//
// Unlike the reference project, this isn't a polling loop on a timer --
// GitHub's unauthenticated rate limit (60 requests/hour/IP) is much
// tighter than the GW2 API's, so this is meant to be triggered once on
// addon load and again via an explicit "recheck" action, not on any
// automatic interval.
//--------------------------------------------------------------------------------

#pragma once

#include "merge.h"
#include "Nexus.h"

#include <string>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ESinUpdateState
//--------------------------------------------------------------------------------
// Unknown until a check has run this session. NotInstalled means no
// VfxD_<Sin>*.json was found in denoiserAddonDir at all, as opposed to
// UpToDate/UpdateAvailable, which both imply a file's already there.
//--------------------------------------------------------------------------------
enum class ESinUpdateState
{
    Unknown,
    NotInstalled,
    UpToDate,
    UpdateAvailable,
};

//********************************************************************************
// SinUpdateInfo
//--------------------------------------------------------------------------------
// sinName             "Gluttony" / "Pride" / "Sloth"
// installedPath       full path to the file currently on disk
// installedVersion    -1 if not installed
// latestVersion       -1 until a successful check lands
// latestDownloadUrl   asset's browser_download_url, for
//                     StartApplyUpdate/StartInstallSin
// state               see ESinUpdateState
//--------------------------------------------------------------------------------
// installedPath/installedVersion are only meaningful when state isn't
// NotInstalled -- a successful check always produces one entry per
// kSinNames (see sin_files.h), regardless of whether that sin is
// actually present, so the options panel can offer an "Install" action
// for one the user doesn't have yet.
//--------------------------------------------------------------------------------
struct SinUpdateInfo
{
    std::string     sinName;
    std::string     installedPath;
    int             installedVersion = -1;
    int             latestVersion    = -1;
    std::string     latestDownloadUrl;
    ESinUpdateState state = ESinUpdateState::Unknown;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ECheckStatus
//--------------------------------------------------------------------------------
// Error means the last check failed outright (network/parse) --
// previous Done data, if any, is left as-is.
//--------------------------------------------------------------------------------
enum class ECheckStatus
{
    Idle,
    Checking,
    Done,
    Error,
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EApplyStatus
//--------------------------------------------------------------------------------
// Mirrors ECheckStatus's shape, for StartApplyUpdate/StartInstallSin.
//--------------------------------------------------------------------------------
enum class EApplyStatus
{
    Idle,
    Applying,
    Done,
    Error,
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// StartUpdateCheck
//--------------------------------------------------------------------------------
// Kicks off a background check against GitHub for every sin file found
// in `denoiserAddonDir`. No-op if a check or apply is already in
// flight; never blocks the calling (render) thread. Only hits the
// lightweight releases-list API call, never downloads effect-file
// json. If alsoLoadDiff is true, StartLoadDiff is chained on once the
// check completes and finds at least one update -- pass false (the
// default) for checks that should stay cheap and silent, e.g. the one
// on addon load, where the options panel should only show "Update
// available" and let the user's own click trigger the download.
//--------------------------------------------------------------------------------
void StartUpdateCheck(const std::string& denoiserAddonDir, bool alsoLoadDiff = false);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetCheckStatus / GetSinUpdateInfo
//--------------------------------------------------------------------------------
// Render-thread-safe snapshot of the last completed (or in-progress)
// check -- a mutex-guarded copy of a handful of small structs, not a
// network call. Safe to call every frame.
//--------------------------------------------------------------------------------
ECheckStatus GetCheckStatus();
std::vector<SinUpdateInfo> GetSinUpdateInfo();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IsAnyUpdateAvailable
//--------------------------------------------------------------------------------
// True if at least one installed sin file has a newer version
// available -- exactly the condition that should show/hide the
// "Visual Sins update available" button.
//--------------------------------------------------------------------------------
bool IsAnyUpdateAvailable();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EDiffStatus
//--------------------------------------------------------------------------------
// Downloads the new file for each UpdateAvailable sin, loads the
// corresponding local file, and resolves a MergePlan (see merge.h)
// WITHOUT writing anything to disk -- see StartLoadDiff for when this
// runs. Blocked means the installed file has a duplicate guid (see
// merge.h's FindDuplicateGuids); StartLoadDiff skips the network call
// entirely for a sin in this state rather than downloading something
// it can't safely resolve a plan against. Never becomes Ready --
// StartApplyUpdate's own Ready-only check refuses it too.
//--------------------------------------------------------------------------------
enum class EDiffStatus
{
    NotLoaded,
    Loading,
    Ready,
    Error,
    Blocked,
};

//********************************************************************************
// SinDiffInfo
//--------------------------------------------------------------------------------
// sinName    which sin this diff belongs to
// status     see EDiffStatus
// plan       meaningful only when status == Ready
//--------------------------------------------------------------------------------
struct SinDiffInfo
{
    std::string  sinName;
    EDiffStatus  status = EDiffStatus::NotLoaded;
    MergePlan    plan;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// StartLoadDiff
//--------------------------------------------------------------------------------
// Only ever runs when explicitly asked for -- the top-of-panel "Update
// available" button for that sin (which also covers its own retry:
// Error/Blocked relabel to a clickable retry, same button). The GitHub
// asset download is a CDN redirect, not a REST call, so it doesn't
// cost against the 60/hour API limit -- staying opt-in here is about
// the user's bandwidth/disk-write budget, not the rate limit. No-op
// if a check/apply/diff-load is already running; safe to call again
// later, always replacing whatever was cached for each sin it touches.
// onlySinName, if non-empty, restricts this to just that one sin
// (still only if UpdateAvailable) instead of every outdated one.
//--------------------------------------------------------------------------------
void StartLoadDiff(const std::string& denoiserAddonDir, const std::string& onlySinName = "");

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetSinDiffInfo
//--------------------------------------------------------------------------------
// Render-thread-safe snapshot, like GetSinUpdateInfo() -- cheap copy
// of small structs (MergePlan is just names/guid strings/paths), safe
// every frame.
//--------------------------------------------------------------------------------
std::vector<SinDiffInfo> GetSinDiffInfo();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// StartApplyUpdate
//--------------------------------------------------------------------------------
// Applies the already-Ready diff for exactly one sin: backs up the
// installed file, applies the cached MergePlan to the cached old-file
// json (both captured back when StartLoadDiff ran, not re-downloaded
// or re-decided here), writes to a .tmp file, and renames it over the
// final versioned name. No-op if that sin's diff isn't Ready, or if a
// check/apply/diff-load is already running. Doesn't flip the cached
// update state itself -- call StartUpdateCheck again afterward to
// confirm everything settled at UpToDate via the same verification
// path as any other check, not trusted blindly.
//--------------------------------------------------------------------------------
void StartApplyUpdate(const std::string& denoiserAddonDir, const std::string& sinName);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// StartInstallSin
//--------------------------------------------------------------------------------
// Downloads and writes a sin file that ISN'T currently installed --
// no merge, no backup, nothing to preserve. Only proceeds if the last
// completed check has this sinName in state NotInstalled with a known
// latestDownloadUrl; no-op otherwise (including if a check/apply/
// diff-load is already in flight). Writes straight to a versioned
// "VfxD_<Sin>-v<N>.json" via the same tmp-file-then-rename path
// StartApplyUpdate uses, then triggers a StartUpdateCheck so the
// options panel re-verifies against what's actually on disk.
//--------------------------------------------------------------------------------
void StartInstallSin(const std::string& denoiserAddonDir, const std::string& sinName);

EApplyStatus GetApplyStatus();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetLastApplyMessage
//--------------------------------------------------------------------------------
// Most recent one-line human-readable outcome, e.g. an error message
// or "Updated Gluttony, Pride." Empty if nothing to report yet.
//--------------------------------------------------------------------------------
std::string GetLastApplyMessage();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CancelInFlightUpdateRequest
//--------------------------------------------------------------------------------
// Registered as a shutdown hook (see entry.cpp) so an addon
// unload/game close doesn't have to wait out a hung WinHTTP call.
//--------------------------------------------------------------------------------
void CancelInFlightUpdateRequest();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SetUpdaterLogger
//--------------------------------------------------------------------------------
// Hands this file the Nexus AddonAPI so failures on the background
// thread (network errors, disk-write failures while applying an
// update) can also go to Nexus's own log, not just
// GetLastApplyMessage()/the options panel. Call once from Addon_Load,
// before any Start* function can run; pass nullptr to disable logging.
// The stored pointer is only ever read, never reassigned concurrently
// with a Start* call, so no locking guards it -- same assumption
// s_denoiserAddonDir already relies on elsewhere in this addon.
//--------------------------------------------------------------------------------
void SetUpdaterLogger(AddonAPI_t* aApi);