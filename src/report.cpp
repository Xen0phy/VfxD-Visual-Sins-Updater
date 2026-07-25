// report.cpp
//
// See report.h for the feature-level description. Implementation notes:
// - HTTP via WinHTTP, synchronous, always on a short-lived detached
//   background thread -- same shape as github_update.cpp's
//   HttpsGetToString, but POST + a JSON body instead of GET, against the
//   report-relay Worker's host instead of GitHub's (which itself then
//   holds/POSTs the real Discord webhook -- see report.h). Deliberately
//   not shared code with github_update.cpp -- see report.h's header
//   comment for why.
// - All validation (empty note, blank/duplicate guids within one
//   submission) happens synchronously on the calling thread, before
//   anything is queued -- a rejected report never touches the network at
//   all. Deduping against guids already known (either locally or by
//   other users) is left entirely to the relay -- see StartSendReport's
//   doc comment in report.h for why.
// - This file never renders a GUID block's text itself -- entries[].block
//   arrives already-composed (see report.h), so this file's only job with
//   it is passing it through into the JSON payload untouched.
#include "report.h"
#include "webhook_config.h"
#include "nlohmann_json.hpp"
#include <windows.h>
#include <winhttp.h>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <cctype>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Shared state -- same split as github_update.cpp: an atomic in-flight
// flag, a mutex-guarded last-result message, and a status enum polled from
// the render thread.
// ---------------------------------------------------------------------------
static std::atomic<EReportStatus> s_reportStatus{EReportStatus::Idle};
static std::atomic<bool>          s_reportInFlight{false};
static std::mutex                 s_messageMutex;
static std::string                s_lastReportMessage; // guarded by s_messageMutex

static std::mutex s_activeHandlesMutex;
static HINTERNET  s_activeSession = nullptr;
static HINTERNET  s_activeConnect = nullptr;
static HINTERNET  s_activeRequest = nullptr;

void CancelInFlightReportRequest()
{
    std::lock_guard<std::mutex> lock(s_activeHandlesMutex);
    if (s_activeRequest) { WinHttpCloseHandle(s_activeRequest); s_activeRequest = nullptr; }
    if (s_activeConnect) { WinHttpCloseHandle(s_activeConnect); s_activeConnect = nullptr; }
    if (s_activeSession) { WinHttpCloseHandle(s_activeSession); s_activeSession = nullptr; }
}

namespace {

std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end()); // webhook host/path here are plain ASCII
}

std::string Trim(const std::string& s)
{
    size_t start = 0, end = s.size();
    while (start < end && std::isspace((unsigned char)s[start])) ++start;
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(start, end - start);
}

// Deliberately not a secret itself -- the same fixed key obfuscates
// whatever URL tools/generate_webhook_config.py encoded into
// webhook_config.h's kWebhookUrlXor array (see that script's own header
// comment for how to regenerate it, and webhook_config.example.h for what
// this obfuscation does and doesn't protect against). Keep this key in
// sync with the KEY list in that script if it's ever changed.
constexpr unsigned char kWebhookXorKey[] = { 0x5A, 0x3C, 0x91, 0x7E, 0x2D, 0xC8, 0x11 };

// Reconstructs the real relay URL from webhook_config.h's obfuscated
// bytes (name kept as-is for continuity with webhook_config.h/
// generate_webhook_config.py -- it decodes a URL either way). Only ever
// called right before the POST that needs it -- the plain URL exists in
// memory only as long as it takes WinHTTP to consume it, same as any
// string handed to WinHttpConnect ordinarily would.
std::string DecodeWebhookUrl()
{
    std::string out;
    out.reserve(kWebhookUrlXorLen);
    for (size_t i = 0; i < kWebhookUrlXorLen; ++i)
        out.push_back(static_cast<char>(kWebhookUrlXor[i] ^ kWebhookXorKey[i % sizeof(kWebhookXorKey)]));
    return out;
}

// Synchronous HTTPS POST of a JSON body against a full URL. Same shape as
// github_update.cpp's HttpsGetToString (crack URL, WinHttpOpen/Connect/
// OpenRequest/SendRequest/ReceiveResponse, handles tracked in
// s_active*/CancelInFlightReportRequest so an addon unload can interrupt a
// hung call) but POST with a request body instead of GET, and this file's
// own handle-tracking statics rather than shared ones -- github_update.cpp
// and this file hit unrelated hosts and have no other reason to coordinate.
bool HttpsPostJson(const std::wstring& url, const std::string& jsonBody, int& outStatusCode, std::string& outResponseBody)
{
    outStatusCode = 0;
    outResponseBody.clear();

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName     = hostBuf;
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

    // A single small JSON message -- no need for the release-download's
    // more generous timeouts in github_update.cpp.
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, hostBuf, uc.nPort, 0);
    if (!hConnect)
    {
        std::lock_guard<std::mutex> lock(s_activeHandlesMutex);
        if (s_activeSession) { WinHttpCloseHandle(s_activeSession); s_activeSession = nullptr; }
        return false;
    }
    { std::lock_guard<std::mutex> lock(s_activeHandlesMutex); s_activeConnect = hConnect; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", pathBuf,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest)
    {
        std::lock_guard<std::mutex> lock(s_activeHandlesMutex);
        if (s_activeConnect) { WinHttpCloseHandle(s_activeConnect); s_activeConnect = nullptr; }
        if (s_activeSession) { WinHttpCloseHandle(s_activeSession); s_activeSession = nullptr; }
        return false;
    }
    { std::lock_guard<std::mutex> lock(s_activeHandlesMutex); s_activeRequest = hRequest; }

    const wchar_t* headers = L"Content-Type: application/json\r\n";

    bool ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
                   (LPVOID)jsonBody.data(), (DWORD)jsonBody.size(), (DWORD)jsonBody.size(), 0)
           && WinHttpReceiveResponse(hRequest, NULL);

    if (ok)
    {
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        outStatusCode = (int)statusCode;

        // Unlike posting straight to Discord (204 empty on success), the
        // relay always returns a small JSON body that StartSendReport's
        // background thread needs to parse -- either
        // {"droppedGuids": [...]} on success (possibly empty), or
        // {"error": ...} on failure, plus a specific error code (rate_
        // limited, discord_failed, etc.) instead of a generic failure
        // message.
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (WinHttpReadData(hRequest, chunk.data(), available, &read))
            {
                chunk.resize(read);
                outResponseBody += chunk;
            }
            else
            {
                break;
            }
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

} // namespace

bool StartSendReport(const std::string& reporterLine,
                     const std::vector<ReportGuidBlock>& entries,
                     const std::string& note,
                     std::string& outError)
{
    if (s_reportInFlight.load())
    {
        outError = "A report is already being sent -- wait for it to finish.";
        return false;
    }

    std::string trimmedNote = Trim(note);
    if (trimmedNote.empty())
    {
        outError = "Additional information can't be empty.";
        return false;
    }

    // Every entry needs a non-blank guid (an entry with a blank guid is a
    // half-finished row, not a real submission), and the same guid can't
    // appear twice in one submission (a paste mistake, not a real second
    // report) -- blocks the whole submission rather than silently
    // dropping just the offending row, so the user sees exactly what to
    // fix before retrying. Dedup against guids already known -- by this
    // user's own installed sin files, or by anyone else who's reported
    // them before -- is left entirely to the relay's cross-user
    // known-guid set (see report.h's doc comment on StartSendReport).
    std::unordered_set<std::string> seenThisSubmission;
    for (const auto& entry : entries)
    {
        std::string trimmedGuid = Trim(entry.guid);
        if (trimmedGuid.empty())
        {
            outError = "One of the GUID rows is empty -- fill it in or remove it.";
            return false;
        }
        if (!seenThisSubmission.insert(trimmedGuid).second)
        {
            outError = "GUID \"" + trimmedGuid + "\" is listed more than once.";
            return false;
        }
    }

    // Built here, on the calling (render) thread -- cheap, and keeps the
    // background thread doing nothing but the network call itself, same
    // split github_update.cpp's Start* functions use.
    //
    // Payload shape matches vfxd-sins-report-relay/src/index.js's expected
    // body exactly: reporterLine and each entry's block arrive fully
    // rendered from the caller (see report.h) -- the Worker never parses
    // or understands them, it only ever treats each entry's guid as a
    // dedup key and entry.block/reporterLine/note as opaque text to
    // assemble into the final Discord message.
    json payload;
    payload["reporterLine"] = reporterLine;
    json jsonEntries = json::array();
    for (const auto& entry : entries)
    {
        json e;
        e["guid"]  = Trim(entry.guid);
        e["block"] = entry.block;
        jsonEntries.push_back(std::move(e));
    }
    payload["entries"] = std::move(jsonEntries);
    payload["note"] = trimmedNote;
    std::string body = payload.dump();

    const size_t submittedCount = entries.size();

    s_reportInFlight.store(true);
    s_reportStatus.store(EReportStatus::Sending);

    std::thread([body, submittedCount]()
    {
        int statusCode = 0;
        std::string responseBody;
        bool ok = HttpsPostJson(Widen(DecodeWebhookUrl()), body, statusCode, responseBody);

        // Best-effort parse -- a malformed/empty body (e.g. a connection
        // that dies mid-response) just falls through to the generic
        // messages below rather than throwing.
        json parsed;
        bool parsedOk = false;
        if (!responseBody.empty())
        {
            try { parsed = json::parse(responseBody); parsedOk = true; }
            catch (...) { parsedOk = false; }
        }

        std::lock_guard<std::mutex> lock(s_messageMutex);
        if (ok && statusCode >= 200 && statusCode < 300)
        {
            // Per-entry dedup, not all-or-nothing: droppedGuids lists
            // exactly which submitted guids were already in the relay's
            // cross-user known-guid set (and so weren't forwarded to
            // Discord) -- everything else in the submission was sent,
            // including the note either way. A blank-guid-only report
            // (submittedCount == 0) has nothing to dedup, so it's always
            // just "sent."
            size_t droppedCount = 0;
            if (parsedOk && parsed.contains("droppedGuids") && parsed["droppedGuids"].is_array())
                droppedCount = parsed["droppedGuids"].size();

            if (submittedCount == 0 || droppedCount == 0)
            {
                s_lastReportMessage = "Report sent -- thank you!";
            }
            else if (droppedCount >= submittedCount)
            {
                s_lastReportMessage = "All submitted GUIDs were already known -- nothing new to send, thanks anyway!";
            }
            else
            {
                size_t sentCount = submittedCount - droppedCount;
                s_lastReportMessage = std::to_string(droppedCount) + " of " + std::to_string(submittedCount) +
                                       " GUID(s) already known -- " + std::to_string(sentCount) + " sent, thanks!";
            }
            s_reportStatus.store(EReportStatus::Done);
        }
        else if (ok)
        {
            // Relay's error codes: invalid_guid/note_required/
            // duplicate_in_submission (400, shouldn't happen -- report.cpp
            // already validates these client-side, but the relay is the
            // source of truth), rate_limited (429), discord_failed (502).
            std::string errCode = (parsedOk && parsed.contains("error") && parsed["error"].is_string())
                                       ? parsed["error"].get<std::string>()
                                       : "";
            if (errCode == "rate_limited")
                s_lastReportMessage = "Too many reports sent recently -- wait a bit and try again.";
            else if (errCode == "discord_failed")
                s_lastReportMessage = "Report relay couldn't reach Discord -- try again later.";
            else if (!errCode.empty())
                s_lastReportMessage = "Report rejected: " + errCode;
            else
                s_lastReportMessage = "Report rejected (HTTP " + std::to_string(statusCode) + ").";
            s_reportStatus.store(EReportStatus::Error);
        }
        else
        {
            s_lastReportMessage = "Couldn't reach the report relay -- check your connection and try again.";
            s_reportStatus.store(EReportStatus::Error);
        }
        s_reportInFlight.store(false);
    }).detach();

    return true;
}

EReportStatus GetReportStatus()
{
    return s_reportStatus.load();
}

std::string GetLastReportMessage()
{
    std::lock_guard<std::mutex> lock(s_messageMutex);
    return s_lastReportMessage;
}