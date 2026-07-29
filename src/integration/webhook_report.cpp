//################################################################################
// webhook_report.cpp
//--------------------------------------------------------------------------------
// StartSendReport()              validates and starts sending a report
// GetReportStatus()               current EReportStatus
// GetLastReportOutcome()          EReportOutcome for the last Done/Error result
// GetLastReportMessage()          most recent human-readable outcome
// CancelInFlightReportRequest()   closes in-flight WinHTTP handles
//--------------------------------------------------------------------------------
// See webhook_report.h for the feature-level description. HTTP is via
// WinHTTP, synchronous, always on a short-lived detached background
// thread -- same shape as github_update.cpp's HttpsGetToString, but POST +
// a JSON body instead of GET, against the report-relay Worker's host
// instead of GitHub's -- deliberately not sharing code with
// github_update.cpp, see webhook_report.h for why. All validation (empty
// note, blank/duplicate guids, rendered-length) runs synchronously on the
// calling thread before anything is queued; a rejected report never
// touches the network. This file never renders a guid block's text
// itself -- entries[].block arrives already-composed, so its only job
// with it is passing it through into the JSON payload untouched, and (for
// the length check) measuring it rather than reading it.
//
// Shared state: an atomic in-flight flag and status enum polled from the
// render thread, a mutex-guarded last-result message, and the active
// WinHTTP handles tracked so CancelInFlightReportRequest can interrupt a
// hung call from another thread -- same split as github_update.cpp.
//--------------------------------------------------------------------------------

#pragma comment(lib, "winhttp.lib")

#include "nlohmann_json.hpp"
#include "webhook_report.h"
#include "webhook_config.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <cctype>

using json = nlohmann::json;

static std::atomic<EReportStatus>  s_reportStatus{EReportStatus::Idle};
static std::atomic<EReportOutcome> s_reportOutcome{EReportOutcome::None};
static std::atomic<bool>           s_reportInFlight{false};
static std::mutex                 s_messageMutex;
static std::string                s_lastReportMessage; //. guarded by s_messageMutex

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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Widen
//--------------------------------------------------------------------------------
// Assumes `s` is plain ASCII -- true of every webhook host/path this file
// ever builds one from.
//--------------------------------------------------------------------------------
std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

std::string Trim(const std::string& s)
{
    size_t start = 0, end = s.size();
    while (start < end && std::isspace((unsigned char)s[start])) ++start;
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(start, end - start);
}

//_ Discord's real webhook message-content limit -- see
// EstimateDiscordContentLength, the actual check this backs.
constexpr size_t kDiscordContentLimit = 2000;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EstimateDiscordContentLength
//--------------------------------------------------------------------------------
// Mirrors vfxd-sins-report-relay/src/index.js's exact assembly:
// `${reporterLine}\n\n${bodyParts.join("\n")}`, where bodyParts is every
// surviving entry's block plus the note under an "Additional" heading,
// blockquoted one "> " per line.
//
// Always assumes zero omitted guids -- the real worst case, since an
// omitted guid drops its full block for just its bare 24-char text in
// the omission list, only ever shrinking the message. Note is measured
// after trimming (what's actually sent); blocks are measured as-is.
//--------------------------------------------------------------------------------
size_t EstimateDiscordContentLength(const std::string& reporterLine,
                                    const std::vector<ReportGuidBlock>& entries,
                                    const std::string& trimmedNote)
{
    size_t total = reporterLine.size() + 2; //. reporterLine + blank line ("\n\n")

    for (const auto& entry : entries)
        total += entry.block.size() + 1; //. block + its join("\n") separator

    //_ "Additional\n" header (11 chars) + the note, blockquoted -- join
    // preserves the note's own newline count, so only the "> " prefixes
    // (2 chars per line) add length on top of trimmedNote.size() itself
    size_t noteLines = 1 + std::count(trimmedNote.begin(), trimmedNote.end(), '\n');
    total += 11 + trimmedNote.size() + 2 * noteLines;

    return total;
}

//_ Not a secret itself -- obfuscates whatever URL
// tools/generate_webhook_config.py wrote into webhook_config.h. Keep in
// sync with that script's KEY list if this ever changes.
constexpr unsigned char kWebhookXorKey[] = { 0x5A, 0x3C, 0x91, 0x7E, 0x2D, 0xC8, 0x11 };

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// DecodeWebhookUrl
//--------------------------------------------------------------------------------
// Reconstructs the real relay URL from webhook_config.h's obfuscated
// bytes. Only ever called right before the POST that needs it -- the
// plain URL exists in memory only as long as it takes WinHTTP to consume
// it, same as any string ordinarily handed to WinHttpConnect.
//--------------------------------------------------------------------------------
std::string DecodeWebhookUrl()
{
    std::string out;
    out.reserve(kWebhookUrlXorLen);
    for (size_t i = 0; i < kWebhookUrlXorLen; ++i)
        out.push_back(static_cast<char>(kWebhookUrlXor[i] ^ kWebhookXorKey[i % sizeof(kWebhookXorKey)]));
    return out;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// HttpsPostJson
//--------------------------------------------------------------------------------
// Synchronous HTTPS POST of a JSON body against a full URL. Same shape as
// github_update.cpp's HttpsGetToString (crack URL, WinHttpOpen/Connect/
// OpenRequest/SendRequest/ReceiveResponse, handles tracked in s_active*/
// CancelInFlightReportRequest so an addon unload can interrupt a hung
// call) but POST with a body instead of GET, and this file's own
// handle-tracking statics rather than shared ones. Unlike posting
// straight to Discord (204 empty on success), the relay always returns a
// small JSON body -- {"status": "sent"|"partial"|"duplicate", "sent":
// [...], "omitted": [...]} on success, {"error": ...} plus an error code
// on failure -- which StartSendReport's background thread parses.
//--------------------------------------------------------------------------------
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

    //_ A single small JSON message -- no need for the release-download's
    // more generous timeouts in github_update.cpp
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

} //. namespace

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// StartSendReport
//--------------------------------------------------------------------------------
// Claims s_reportInFlight via compare_exchange (not load-then-store) so
// two near-simultaneous calls can't both pass the in-flight check and
// both spawn a send -- same CAS-then-release-on-bail shape github_update.cpp's
// Start* functions use. Validation runs entirely before the claim is used
// for anything; every failure path releases it again before returning.
//--------------------------------------------------------------------------------
bool StartSendReport(const std::string& reporterLine,
                     const std::vector<ReportGuidBlock>& entries,
                     const std::string& note,
                     std::string& outError)
{
    bool expected = false;
    if (!s_reportInFlight.compare_exchange_strong(expected, true))
    {
        outError = "A report is already being sent -- wait for it to finish.";
        return false;
    }

    std::string trimmedNote = Trim(note);
    if (trimmedNote.empty())
    {
        outError = "Additional information can't be empty.";
        s_reportInFlight.store(false); //. release the claim
        return false;
    }

    //_ Real enforcement of kMaxReportGuids -- a row-count bound only, not
    // a length guarantee. See the EstimateDiscordContentLength check
    // further down below for that.
    if (entries.size() > kMaxReportGuids)
    {
        outError = "Reports are capped at " + std::to_string(kMaxReportGuids) +
                   " GUIDs at a time -- send this batch first, then start another.";
        s_reportInFlight.store(false); //. release the claim
        return false;
    }

    //_ Blocks the whole submission (not just the offending row) so the
    // user sees exactly what to fix; cross-user dedup is the relay's job
    std::unordered_set<std::string> seenThisSubmission;
    for (const auto& entry : entries)
    {
        std::string trimmedGuid = Trim(entry.guid);
        if (trimmedGuid.empty())
        {
            outError = "One of the GUID rows is empty -- fill it in or remove it.";
            s_reportInFlight.store(false); //. release the claim
            return false;
        }
        if (!seenThisSubmission.insert(trimmedGuid).second)
        {
            outError = "GUID \"" + trimmedGuid + "\" is listed more than once.";
            s_reportInFlight.store(false); //. release the claim
            return false;
        }
    }

    //_ Real enforcement of Discord's 2000-char content limit, computed
    // from the actual reporterLine/blocks/note -- see
    // EstimateDiscordContentLength's own comment for the worst-case logic.
    if (EstimateDiscordContentLength(reporterLine, entries, trimmedNote) > kDiscordContentLimit)
    {
        outError = "Report is too long -- shorten the note or remove a GUID.";
        s_reportInFlight.store(false); //. release the claim
        return false;
    }

    //_ Built here, on the calling thread -- cheap, keeps the background
    // thread doing nothing but the network call, same split github_update.cpp
    // uses. Shape matches vfxd-sins-report-relay/src/index.js: the Worker
    // only ever treats guid as a dedup key and block/reporterLine/note as
    // opaque text to assemble into the Discord message.
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

    //_ s_reportInFlight is already true, claimed atomically above
    s_reportStatus.store(EReportStatus::Sending);

    std::thread([body, submittedCount]()
    {
        int statusCode = 0;
        std::string responseBody;
        bool ok = HttpsPostJson(Widen(DecodeWebhookUrl()), body, statusCode, responseBody);

        //_ Best-effort parse -- a malformed/empty body (e.g. a connection
        // dying mid-response) falls through to the generic messages below
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
            //_ Per-entry, not all-or-nothing: "sent"/"omitted" list which
            // submitted guids were actually forwarded vs. already known.
            // "status" drives the wording/outcome directly rather than
            // being re-derived from the counts, so it stays in sync with
            // whatever the relay decided (see index.js's response doc).
            std::string status = (parsedOk && parsed.contains("status") && parsed["status"].is_string())
                                      ? parsed["status"].get<std::string>()
                                      : "";
            size_t sentCount = (parsedOk && parsed.contains("sent") && parsed["sent"].is_array())
                                    ? parsed["sent"].size() : 0;
            size_t omittedCount = (parsedOk && parsed.contains("omitted") && parsed["omitted"].is_array())
                                    ? parsed["omitted"].size() : 0;

            if (status == "duplicate")
            {
                s_lastReportMessage = "All submitted GUIDs were already known -- nothing new to send, thanks anyway!";
                s_reportOutcome.store(EReportOutcome::NoneSent);
            }
            else if (status == "partial")
            {
                s_lastReportMessage = std::to_string(omittedCount) + " of " + std::to_string(submittedCount) +
                                       " GUID(s) already known -- " + std::to_string(sentCount) + " sent, thanks!";
                s_reportOutcome.store(EReportOutcome::PartiallySent);
            }
            else
            {
                //_ "sent", or an unrecognized/missing status -- treat as
                // the ordinary success case rather than silently doing
                // nothing, same fallback spirit as the errCode handling below
                s_lastReportMessage = "Report sent -- thank you!";
                s_reportOutcome.store(EReportOutcome::AllSent);
            }
            s_reportStatus.store(EReportStatus::Done);
        }
        else if (ok)
        {
            //_ 400s (invalid_guid/note_required/too_many_entries/
            // note_too_long/content_too_long) shouldn't happen -- already
            // validated client-side. rate_limited (429), discord_failed (502).
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
            s_reportOutcome.store(EReportOutcome::None);
            s_reportStatus.store(EReportStatus::Error);
        }
        else
        {
            s_lastReportMessage = "Couldn't reach the report relay -- check your connection and try again.";
            s_reportOutcome.store(EReportOutcome::None);
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

EReportOutcome GetLastReportOutcome()
{
    return s_reportOutcome.load();
}

std::string GetLastReportMessage()
{
    std::lock_guard<std::mutex> lock(s_messageMutex);
    return s_lastReportMessage;
}