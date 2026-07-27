// github_update.cpp
//
// Implementation notes (mirrors gw2_api.cpp's pattern from the reference
// project this was modeled on):
// - HTTP via WinHTTP (synchronous calls), always from a short-lived
//   detached background thread -- never the render thread.
// - A single atomic in-flight flag covers checking, diff-loading, and
//   applying, since they touch the same files and must never run
//   concurrently with each other (a check running mid-apply could report
//   stale info).
// - Every failure path leaves previously-cached results untouched.
// - Unlike the reference project's fixed host+path, GitHub release assets
//   redirect to a different host (objects.githubusercontent.com), so the
//   HTTP helper here takes a full URL and cracks it with WinHttpCrackUrl
//   rather than assuming one fixed host. WinHTTP follows redirects
//   automatically by default, including cross-host ones, so no extra
//   handling is needed for that -- flagging the assumption here in case a
//   future WinHTTP policy change on the user's system disables it.
#include "integration/github_update.h"
#include "core/sin_files.h"
#include "core/merge.h"
#include "nlohmann_json.hpp"
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <regex>
#include <unordered_map>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

// nlohmann::json::dump() always emits bare '\n' line endings, but every
// VfxDenoiser file shipped/edited in the wild uses CRLF. Converting here
// (rather than leaving dump()'s output as-is) keeps an applied-update file's
// line endings consistent with the convention every other VfxDenoiser file
// on disk already uses, instead of silently switching just this one file to
// LF the moment an update is applied. Mirrors addon.cpp's own ToCrlf, kept
// as a separate copy since the two files don't currently share a utility
// header.
static std::string ToCrlf(const std::string& lfText)
{
    std::string out;
    out.reserve(lfText.size() + lfText.size() / 20);
    for (char c : lfText)
    {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static std::mutex                      s_mutex;
static std::vector<SinUpdateInfo>      s_sinInfo;        // guarded by s_mutex
static std::atomic<ECheckStatus>       s_checkStatus{ECheckStatus::Idle};
static std::atomic<EApplyStatus>       s_applyStatus{EApplyStatus::Idle};
static std::atomic<bool>               s_requestInFlight{false}; // covers check, apply, AND diff-load -- none of these may overlap each other
static std::mutex                      s_messageMutex;
static std::string                     s_lastApplyMessage; // guarded by s_messageMutex

// Everything needed to display a diff AND, later, apply it without
// re-downloading or re-deciding anything. Populated by StartLoadDiff,
// consumed by StartApplyUpdate. oldFile/installedPath/latestVersion are
// deliberately not part of the public SinDiffInfo -- GetSinDiffInfo() only
// hands out the display-safe MergePlan, not the raw json this cache also
// carries.
struct DiffCacheEntry
{
    EDiffStatus status = EDiffStatus::NotLoaded;
    MergePlan   plan;
    json        oldFile;
    std::string installedPath;
    int         latestVersion = -1;
};
static std::mutex                                   s_diffMutex;
static std::unordered_map<std::string, DiffCacheEntry> s_diffCache; // keyed by sinName, guarded by s_diffMutex

static constexpr const char* kRepoOwner = "Xen0phy";
static constexpr const char* kRepoName  = "VfxD_Visual_Sins";

// Set once (see SetUpdaterLogger) before any background thread can start;
// never reassigned afterward, so reading it from a background thread
// without a lock is safe.
static AddonAPI_t* s_api = nullptr;

void SetUpdaterLogger(AddonAPI_t* aApi)
{
    s_api = aApi;
}

static void LogCritical(const std::string& msg)
{
    if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", msg.c_str());
}

// ---------------------------------------------------------------------------
// In-flight handle tracking / cancellation -- same pattern as the reference
// project: WinHTTP's documented way to cancel a blocked synchronous call is
// to close its handles from a different thread.
// ---------------------------------------------------------------------------
static std::mutex s_activeHandlesMutex;
static HINTERNET  s_activeSession = nullptr;
static HINTERNET  s_activeConnect = nullptr;
static HINTERNET  s_activeRequest = nullptr;

void CancelInFlightUpdateRequest()
{
    std::lock_guard<std::mutex> lock(s_activeHandlesMutex);
    if (s_activeRequest) { WinHttpCloseHandle(s_activeRequest); s_activeRequest = nullptr; }
    if (s_activeConnect) { WinHttpCloseHandle(s_activeConnect); s_activeConnect = nullptr; }
    if (s_activeSession) { WinHttpCloseHandle(s_activeSession); s_activeSession = nullptr; }
}

// ---------------------------------------------------------------------------
// HttpsGetToString
// ---------------------------------------------------------------------------
// Synchronous HTTPS GET against a full URL. Always called from the
// background thread. Returns false only on a transport-level failure
// (couldn't even get a response); a non-200 status is still reported via
// outStatusCode with outBody left as whatever the server sent.
// ---------------------------------------------------------------------------
static bool HttpsGetToString(const std::wstring& url, std::string& outBody, int& outStatusCode)
{
    outStatusCode = 0;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName    = hostBuf;
    uc.dwHostNameLength = _countof(hostBuf);
    uc.lpszUrlPath      = pathBuf;
    uc.dwUrlPathLength  = _countof(pathBuf);
    uc.dwSchemeLength   = (DWORD)-1;

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
        return false;

    HINTERNET hSession = WinHttpOpen(L"VfxDSinsUpdater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    { std::lock_guard<std::mutex> lock(s_activeHandlesMutex); s_activeSession = hSession; }

    // Release asset downloads can be several MB of JSON on a slow
    // connection -- more generous than a tiny API response, but still
    // bounded so a hung connection can't wedge the background thread
    // forever (CancelInFlightUpdateRequest is the other way this ends
    // early, on addon unload).
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 30000);

    HINTERNET hConnect = WinHttpConnect(hSession, hostBuf, uc.nPort, 0);
    if (!hConnect)
    {
        std::lock_guard<std::mutex> lock(s_activeHandlesMutex);
        if (s_activeSession) { WinHttpCloseHandle(s_activeSession); s_activeSession = nullptr; }
        return false;
    }
    { std::lock_guard<std::mutex> lock(s_activeHandlesMutex); s_activeConnect = hConnect; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathBuf,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest)
    {
        std::lock_guard<std::mutex> lock(s_activeHandlesMutex);
        if (s_activeConnect) { WinHttpCloseHandle(s_activeConnect); s_activeConnect = nullptr; }
        if (s_activeSession) { WinHttpCloseHandle(s_activeSession); s_activeSession = nullptr; }
        return false;
    }
    { std::lock_guard<std::mutex> lock(s_activeHandlesMutex); s_activeRequest = hRequest; }

    // GitHub's API requires a User-Agent on every request (already sent
    // above via WinHttpOpen's agent string) and returns cleaner JSON with
    // this Accept header; harmless for the non-API asset-download URL too.
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";

    bool ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hRequest, NULL);

    if (ok)
    {
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        outStatusCode = (int)statusCode;

        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), available, &read)) { ok = false; break; }
            chunk.resize(read);
            outBody += chunk;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_activeHandlesMutex);
        if (s_activeRequest) { WinHttpCloseHandle(s_activeRequest); s_activeRequest = nullptr; }
        if (s_activeConnect) { WinHttpCloseHandle(s_activeConnect); s_activeConnect = nullptr; }
        if (s_activeSession) { WinHttpCloseHandle(s_activeSession); s_activeSession = nullptr; }
    }

    return ok;
}

static std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end()); // asset names/URLs here are plain ASCII
}

// Matches "VfxD_Gluttony-v4177.json" / "VfxD_Gluttony_v4177.json" among a
// release's asset names. No-suffix assets are not expected from GitHub
// (only from a user's local, possibly-manually-renamed install), so an
// unsuffixed match here is simply ignored rather than treated as version -1.
static bool ParseAssetVersion(const std::string& assetName, const std::string& sinName, int& outVersion)
{
    std::regex pattern("^VfxD_" + sinName + R"([-_]v(\d+)\.json$)");
    std::smatch m;
    if (!std::regex_match(assetName, m, pattern)) return false;
    outVersion = std::stoi(m[1].str());
    return true;
}

static void SetLastApplyMessage(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(s_messageMutex);
    s_lastApplyMessage = msg;
}

void StartUpdateCheck(const std::string& denoiserAddonDir, bool alsoLoadDiff)
{
    bool expected = false;
    if (!s_requestInFlight.compare_exchange_strong(expected, true))
        return; // a check or apply is already running

    s_checkStatus.store(ECheckStatus::Checking);

    std::thread([denoiserAddonDir, alsoLoadDiff]()
    {
        // Keyed lookup of whatever's actually on disk right now -- but the
        // loop below always walks kSinNames (all three), not just what's
        // found here, so a sin the user doesn't have yet still gets a
        // result (state NotInstalled) with a latestVersion/download URL an
        // "Install" button can use, instead of silently not appearing at
        // all like it used to.
        auto installed = ScanInstalledSinFiles(denoiserAddonDir);
        std::unordered_map<std::string, InstalledSinFile> installedByName;
        for (const auto& f : installed)
            installedByName[f.sinName] = f;

        // Shared failure path for all three error cases below. The intent
        // (see the original comment this replaced) is to leave s_sinInfo
        // exactly as it was from any previous successful check, so a
        // rate-limit hit or network hiccup doesn't make an already-known
        // update disappear. But that only makes sense if there WAS a
        // previous successful check -- if this is the very first check
        // this session (e.g. the on-load check itself hits the network
        // hiccup) s_sinInfo is just empty, and leaving it empty makes
        // GetSinUpdateInfo() report nothing at all for any sin. The UI
        // (RenderSinActionRow) then can't tell "installed, unknown
        // version" apart from "genuinely not installed" and defaults every
        // sin to NotInstalled -- Install button on everything, even sins
        // that are sitting right there on disk. So: only in that cold-start
        // case, fall back to what ScanInstalledSinFiles already found above
        // (before the network call), reporting installed sins as installed
        // with an unknown latest version rather than as not-installed.
        auto storeFailure = [&installedByName]()
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_sinInfo.empty())
            {
                std::vector<SinUpdateInfo> fallback;
                for (int i = 0; i < kSinCount; ++i)
                {
                    SinUpdateInfo info;
                    info.sinName = kSinNames[i];

                    auto instIt = installedByName.find(info.sinName);
                    if (instIt != installedByName.end())
                    {
                        info.installedPath    = instIt->second.fullPath;
                        info.installedVersion = instIt->second.version;
                        // latestVersion stays -1 (unknown) -- UpToDate here
                        // just means "installed, can't tell if there's an
                        // update," which is what falls through to the
                        // non-actionable "Up to date" button rather than
                        // Install.
                        info.state = ESinUpdateState::UpToDate;
                    }
                    else
                    {
                        info.state = ESinUpdateState::NotInstalled;
                    }

                    fallback.push_back(std::move(info));
                }
                s_sinInfo = std::move(fallback);
            }
        };

        std::string body;
        int statusCode = 0;
        std::string apiUrl = "https://api.github.com/repos/" + std::string(kRepoOwner) + "/" + kRepoName + "/releases/latest";
        bool ok = HttpsGetToString(Widen(apiUrl), body, statusCode);

        if (!ok || statusCode != 200)
        {
            storeFailure();
            s_checkStatus.store(ECheckStatus::Error);
            s_requestInFlight.store(false);
            return;
        }

        json release;
        try { release = json::parse(body); }
        catch (...)
        {
            storeFailure();
            s_checkStatus.store(ECheckStatus::Error);
            s_requestInFlight.store(false);
            return;
        }

        if (!release.contains("assets") || !release["assets"].is_array())
        {
            storeFailure();
            s_checkStatus.store(ECheckStatus::Error);
            s_requestInFlight.store(false);
            return;
        }

        std::vector<SinUpdateInfo> results;
        for (int i = 0; i < kSinCount; ++i)
        {
            std::string sinName = kSinNames[i];

            SinUpdateInfo info;
            info.sinName = sinName;

            auto instIt = installedByName.find(sinName);
            bool isInstalled = (instIt != installedByName.end());
            if (isInstalled)
            {
                info.installedPath    = instIt->second.fullPath;
                info.installedVersion = instIt->second.version;
            }

            for (const auto& asset : release["assets"])
            {
                if (!asset.contains("name") || !asset["name"].is_string()) continue;
                std::string assetName = asset["name"].get<std::string>();

                int assetVersion = -1;
                if (!ParseAssetVersion(assetName, sinName, assetVersion)) continue;

                // A release should only ever contain one asset per sin, but
                // if it somehow doesn't, keep the highest version seen.
                if (assetVersion > info.latestVersion)
                {
                    info.latestVersion = assetVersion;
                    info.latestDownloadUrl = asset.value("browser_download_url", "");
                }
            }

            if (info.latestVersion < 0)
            {
                // GitHub's latest release doesn't have this sin at all
                // (unlikely, but possible mid-release-edit) -- report as
                // up to date if it's already installed rather than
                // guessing; if it isn't installed either, there's nothing
                // to offer, so it just reads as not-installed with no
                // usable download URL (the UI disables Install for this).
                info.state = isInstalled ? ESinUpdateState::UpToDate : ESinUpdateState::NotInstalled;
            }
            else if (!isInstalled)
            {
                info.state = ESinUpdateState::NotInstalled;
            }
            else
            {
                info.state = (info.installedVersion < info.latestVersion)
                    ? ESinUpdateState::UpdateAvailable
                    : ESinUpdateState::UpToDate;
            }

            results.push_back(std::move(info));
        }

        bool anyUpdate = false;
        for (const auto& r : results)
            if (r.state == ESinUpdateState::UpdateAvailable)
                anyUpdate = true;

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_sinInfo = std::move(results);
        }
        s_checkStatus.store(ECheckStatus::Done);
        s_requestInFlight.store(false);

        // Eagerly fetch the diff for anything outdated -- but only if the
        // caller asked for that (StartUpdateCheck(dir, true), which is
        // what the "Check now" button uses). The check on addon load
        // passes false, so a fresh update only ever shows up as a note in
        // the options panel until the user explicitly asks to see what
        // changed. Must run after s_requestInFlight is released above,
        // since StartLoadDiff acquires that same flag itself.
        if (anyUpdate && alsoLoadDiff)
            StartLoadDiff(denoiserAddonDir);
    })
    .detach();
}

ECheckStatus GetCheckStatus() { return s_checkStatus.load(); }

std::vector<SinUpdateInfo> GetSinUpdateInfo()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_sinInfo;
}

bool IsAnyUpdateAvailable()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    for (const auto& s : s_sinInfo)
        if (s.state == ESinUpdateState::UpdateAvailable)
            return true;
    return false;
}

EApplyStatus GetApplyStatus() { return s_applyStatus.load(); }

std::string GetLastApplyMessage()
{
    std::lock_guard<std::mutex> lock(s_messageMutex);
    return s_lastApplyMessage;
}

void StartLoadDiff(const std::string& denoiserAddonDir, const std::string& onlySinName)
{
    bool expected = false;
    if (!s_requestInFlight.compare_exchange_strong(expected, true))
        return; // a check, apply, or another diff-load is already running

    std::vector<SinUpdateInfo> toLoad;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const auto& s : s_sinInfo)
            if (s.state == ESinUpdateState::UpdateAvailable && (onlySinName.empty() || s.sinName == onlySinName))
                toLoad.push_back(s);
    }

    if (toLoad.empty())
    {
        s_requestInFlight.store(false);
        return;
    }

    // Mark every sin about to be loaded as Loading immediately (before the
    // background thread even starts) so the options panel can show a
    // spinner for it on the very next frame, not just after the first
    // network call lands.
    {
        std::lock_guard<std::mutex> lock(s_diffMutex);
        for (const auto& sin : toLoad)
            s_diffCache[sin.sinName].status = EDiffStatus::Loading;
    }

    std::thread([denoiserAddonDir, toLoad]()
    {
        for (const auto& sin : toLoad)
        {
            auto fail = [&sin]()
            {
                std::lock_guard<std::mutex> lock(s_diffMutex);
                s_diffCache[sin.sinName].status = EDiffStatus::Error;
            };

            if (sin.latestDownloadUrl.empty()) { fail(); continue; }

            // 1. Load the user's existing file FIRST, before spending a
            //    network call -- and check it for duplicate guids right
            //    away. Guid-first matching (see merge.h) assumes a guid
            //    never belongs to more than one effect in this file; if
            //    that's already violated here, resolving a plan against it
            //    could rework the wrong one of two same-guid effects
            //    without any way to tell. Deliberately checked before step
            //    2's download, not after -- there's no point spending
            //    bandwidth on a sin this pass can't safely use anyway, and
            //    this is a local, instant check.
            json oldFile;
            try
            {
                std::ifstream in(sin.installedPath, std::ios::binary);
                if (!in) { fail(); continue; }
                in >> oldFile;
            }
            catch (...) { fail(); continue; }

            std::vector<std::string> dupeGuids = FindDuplicateGuids(oldFile);
            if (!dupeGuids.empty())
            {
                std::lock_guard<std::mutex> lock(s_diffMutex);
                s_diffCache[sin.sinName].status = EDiffStatus::Blocked;
                continue; // no network call for this sin -- see the comment above
            }

            // 2. Download the new file.
            std::string body;
            int statusCode = 0;
            if (!HttpsGetToString(Widen(sin.latestDownloadUrl), body, statusCode) || statusCode != 200)
            {
                fail();
                continue;
            }

            json newFile;
            try { newFile = json::parse(body); }
            catch (...) { fail(); continue; }

            // 3. Resolve the plan -- read-only, nothing written yet.
            bool ok = false;
            MergePlan plan = ResolveMergePlan(oldFile, newFile, ok);
            if (!ok) { fail(); continue; }

            DiffCacheEntry entry;
            entry.status        = EDiffStatus::Ready;
            entry.plan          = std::move(plan);
            entry.oldFile       = std::move(oldFile);
            entry.installedPath = sin.installedPath;
            entry.latestVersion = sin.latestVersion;

            std::lock_guard<std::mutex> lock(s_diffMutex);
            s_diffCache[sin.sinName] = std::move(entry);
        }

        s_requestInFlight.store(false);
    })
    .detach();
}

std::vector<SinDiffInfo> GetSinDiffInfo()
{
    std::vector<SinDiffInfo> out;
    std::lock_guard<std::mutex> lock(s_diffMutex);
    out.reserve(s_diffCache.size());
    for (const auto& [name, entry] : s_diffCache)
    {
        SinDiffInfo info;
        info.sinName = name;
        info.status  = entry.status;
        if (entry.status == EDiffStatus::Ready)
            info.plan = entry.plan; // only copy the (possibly sizeable) plan when it's actually usable
        out.push_back(std::move(info));
    }
    return out;
}

void StartApplyUpdate(const std::string& denoiserAddonDir, const std::string& sinName)
{
    bool expected = false;
    if (!s_requestInFlight.compare_exchange_strong(expected, true))
        return; // a check, apply, or diff-load is already running

    DiffCacheEntry entry;
    {
        std::lock_guard<std::mutex> lock(s_diffMutex);
        auto it = s_diffCache.find(sinName);
        if (it == s_diffCache.end() || it->second.status != EDiffStatus::Ready)
        {
            s_requestInFlight.store(false);
            return;
        }
        entry = it->second; // copy out -- the background thread below owns it from here
    }

    s_applyStatus.store(EApplyStatus::Applying);

    std::thread([denoiserAddonDir, sinName, entry]()
    {
        json oldFile = entry.oldFile; // working copy; entry.oldFile is untouched if anything below fails

        auto fail = [&](const char* why)
        {
            std::string msg = std::string("Failed: ") + sinName + " (" + why + ")";
            SetLastApplyMessage(msg);
            LogCritical(msg); // this is about writing the user's actual VfxDenoiser file -- surface it loudly, not just in the options panel
            s_applyStatus.store(EApplyStatus::Error);
            s_requestInFlight.store(false);
        };

        // 1. Back up the old file before touching anything, in case the
        //    merge has a bug -- never destroy a user's tuning silently.
        std::error_code ec;
        fs::path backupPath = fs::path(entry.installedPath).concat(".bak");
        fs::copy_file(entry.installedPath, backupPath, fs::copy_options::overwrite_existing, ec);
        if (ec) { fail("couldn't create .bak"); return; }

        // 2. Apply the already-confirmed plan (see merge.h for the rules).
        ApplyMergePlan(oldFile, entry.plan);

        // 3. Write to a temp file first, then rename over the final
        //    name -- so a crash mid-write can't corrupt anything.
        fs::path dir = fs::path(entry.installedPath).parent_path();
        std::string newFileName = "VfxD_" + sinName + "-v" + std::to_string(entry.latestVersion) + ".json";
        fs::path newPath = dir / newFileName;
        fs::path tmpPath = dir / (newFileName + ".tmp");

        try
        {
            std::ofstream out(tmpPath, std::ios::binary);
            if (!out) { fail("couldn't open temp file for writing"); return; }

            out << ToCrlf(oldFile.dump(1, '\t'));
            if (!out) { fail("write to temp file failed (disk full?)"); return; }

            out.close();
            if (!out) { fail("temp file didn't flush to disk cleanly (disk full?)"); return; }
        }
        catch (...) { fail("couldn't write temp file"); return; }

        fs::rename(tmpPath, newPath, ec);
        if (ec) { fail("couldn't rename into place"); return; }

        // 4. Remove the old-named file, unless the version-stamped name
        //    happens to be identical to what it already was.
        if (fs::path(entry.installedPath) != newPath)
            fs::remove(entry.installedPath, ec); // best-effort; leftover old file is harmless clutter, not corruption

        // 5. This sin's cached diff is now stale (it's been applied) --
        //    drop it so the options panel stops offering to re-apply it.
        {
            std::lock_guard<std::mutex> lock(s_diffMutex);
            s_diffCache.erase(sinName);
        }

        SetLastApplyMessage("Updated: " + sinName);
        s_applyStatus.store(EApplyStatus::Done);
        s_requestInFlight.store(false);

        // Re-verify against what's actually on disk now, rather than
        // assuming the write succeeded matches our in-memory expectation.
        StartUpdateCheck(denoiserAddonDir);
    })
    .detach();
}

void StartInstallSin(const std::string& denoiserAddonDir, const std::string& sinName)
{
    bool expected = false;
    if (!s_requestInFlight.compare_exchange_strong(expected, true))
        return; // a check, apply, or diff-load is already running

    SinUpdateInfo target;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const auto& s : s_sinInfo)
        {
            if (s.sinName == sinName) { target = s; found = true; break; }
        }
    }

    // Only proceed against the last completed check's own view of things --
    // if it doesn't think this sin is NotInstalled (stale info, or the user
    // clicked between two checks) or doesn't have a download URL for it,
    // there's nothing safe to do here.
    if (!found || target.state != ESinUpdateState::NotInstalled || target.latestDownloadUrl.empty())
    {
        s_requestInFlight.store(false);
        return;
    }

    s_applyStatus.store(EApplyStatus::Applying);

    std::thread([denoiserAddonDir, sinName, target]()
    {
        auto fail = [&](const char* why)
        {
            std::string msg = std::string("Failed: ") + sinName + " (" + why + ")";
            SetLastApplyMessage(msg);
            LogCritical(msg);
            s_applyStatus.store(EApplyStatus::Error);
            s_requestInFlight.store(false);
        };

        // 1. Download the release asset. Nothing local to reconcile
        //    against -- no merge, no .bak, since there's no existing file
        //    this could clobber.
        std::string body;
        int statusCode = 0;
        if (!HttpsGetToString(Widen(target.latestDownloadUrl), body, statusCode) || statusCode != 200)
        {
            fail("download failed");
            return;
        }

        json newFile;
        try { newFile = json::parse(body); }
        catch (...) { fail("couldn't parse downloaded file"); return; }

        // 2. Write to a temp file first, then rename over the final name --
        //    same write-safety path as StartApplyUpdate, so a crash
        //    mid-write can't leave a half-written file behind.
        fs::path dir = fs::path(denoiserAddonDir);
        std::error_code ec;
        std::string newFileName = "VfxD_" + sinName + "-v" + std::to_string(target.latestVersion) + ".json";
        fs::path newPath = dir / newFileName;
        fs::path tmpPath = dir / (newFileName + ".tmp");

        try
        {
            std::ofstream out(tmpPath, std::ios::binary);
            if (!out) { fail("couldn't open temp file for writing"); return; }

            out << ToCrlf(newFile.dump(1, '\t'));
            if (!out) { fail("write to temp file failed (disk full?)"); return; }

            out.close();
            if (!out) { fail("temp file didn't flush to disk cleanly (disk full?)"); return; }
        }
        catch (...) { fail("couldn't write temp file"); return; }

        fs::rename(tmpPath, newPath, ec);
        if (ec) { fail("couldn't rename into place"); return; }

        SetLastApplyMessage("Installed: " + sinName);
        s_applyStatus.store(EApplyStatus::Done);
        s_requestInFlight.store(false);

        // Re-verify against what's actually on disk now, rather than
        // assuming the write succeeded matches our in-memory expectation --
        // same reasoning as the end of StartApplyUpdate.
        StartUpdateCheck(denoiserAddonDir);
    })
    .detach();
}