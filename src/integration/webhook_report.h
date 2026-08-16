//################################################################################
// webhook_report.h
//--------------------------------------------------------------------------------
// EReportStatus                  polling status for an in-flight/just-sent report
// EReportOutcome                 which of sent/partial/none a Done report was
// ReportGuidBlock                one pre-rendered per-guid block
// StartSendReport()              validates and starts sending a report
// GetReportStatus()               poll for the current send status
// GetLastReportOutcome()          outcome kind for GetLastReportMessage()'s text
// GetLastReportMessage()          most recent human-readable outcome
// CancelInFlightReportRequest()   closes in-flight WinHTTP handles
// BeginReportShutdown / GetReportActiveThreadCount   AddonUnload's shutdown pair
//--------------------------------------------------------------------------------
// "Report an effect back" feature, reworked from a single free-text box: now a
// reporter line plus zero or more per-guid blocks (each with its own Type and
// self-context snapshot) plus one required free-text note. Same networking shape
// as github_update.cpp -- a synchronous WinHTTP call on a short-lived detached
// background thread -- but not sharing code with it: github_update.cpp only GETs
// two fixed GitHub hosts, this only POSTs one relay URL. Posts to the
// vfxd-sins-report-relay Cloudflare Worker, not straight to Discord -- the relay
// holds the real webhook server-side and dedups per guid across users, so this
// file renders all the message's human-readable text. The relay URL is stored
// XOR-obfuscated, not as a plain string -- see webhook_config.example.h and
// DecodeWebhookUrl (webhook_report.cpp).
//--------------------------------------------------------------------------------

#pragma once

#include <string>
#include <vector>

enum class EReportStatus
{
    Idle,
    Sending,
    Done,
    Error,
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EReportOutcome
//--------------------------------------------------------------------------------
// Only meaningful once GetReportStatus() == Done -- distinguishes the relay's
// three possible per-entry outcomes for a successful submission (None outside
// that: not yet sent, or the last attempt errored) so report_ui.cpp can
// color/word the outcome without re-deriving it from raw guid counts itself.
// Mirrors the relay's "status" response field -- see
// vfxd-sins-report-relay/src/index.js's top-of-file response doc.
//--------------------------------------------------------------------------------
enum class EReportOutcome
{
    None,          //. not sent yet, or errored
    AllSent,       //. every submitted guid was new
    PartiallySent, //. some known, some new
    NoneSent,      //. all guids already known
};

//_ Soft cap on row count, not a length guarantee -- see EstimateDiscordContentLength (webhook_report.cpp) for the real check
constexpr size_t kMaxReportGuids = 5;

//********************************************************************************
// ReportGuidBlock
//--------------------------------------------------------------------------------
// guid    raw guid text, exactly as entered/auto-filled -- used for the
//         within-submission duplicate check
// block   fully pre-rendered display text for this guid: a "GUID: `<guid>`" line
//         followed by a fenced Type/self-context code block
//--------------------------------------------------------------------------------
// One guid block, rendered client-side before being handed to this file --
// webhook_report.cpp never knows what a Type or self-context field means, only
// the finished text and raw guid. addon.cpp's RenderReportSection builds block's
// exact text, since it already has
// GameState_ProfessionName/RaceName/SpecializationName on hand for human-readable
// values instead of raw enum ids.
//--------------------------------------------------------------------------------
struct ReportGuidBlock
{
    std::string guid;
    std::string block;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// StartSendReport
//--------------------------------------------------------------------------------
// Validates, and if valid, starts sending a report on a background thread.
// reporterLine is sent as-is, whatever literal "Reporter: ..." line the caller
// composed. entries may be empty -- a guid-less "something's wrong" note is still
// valid -- but rejected if any entry's guid is blank or repeats (after trimming)
// within this submission, or if entries.size() exceeds kMaxReportGuids. note is
// required and rejected if blank/whitespace-only. Returns false with no network
// activity if validation fails or a report is already in flight (outError
// explains why); true once the background send has started -- poll
// GetReportStatus()/GetLastReportMessage() from there, same pattern as
// github_update.cpp's Start*/Get* split.
//--------------------------------------------------------------------------------
bool StartSendReport(const std::string& reporterLine,
                     const std::vector<ReportGuidBlock>& entries,
                     const std::string& note,
                     std::string& outError);

EReportStatus GetReportStatus();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetLastReportOutcome
//--------------------------------------------------------------------------------
// EReportOutcome for the same result GetLastReportMessage() describes in prose --
// None until a report has actually completed Done, and again after any Error.
//--------------------------------------------------------------------------------
EReportOutcome GetLastReportOutcome();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetLastReportMessage
//--------------------------------------------------------------------------------
// Most recent one-line human-readable outcome, e.g. "Report sent -- thank you!",
// "2 of 3 GUID(s) already known -- 1 sent, thanks!", "All submitted GUIDs were
// already known -- nothing new to send, thanks anyway!", or an error. Empty if
// nothing's been sent yet this session.
//--------------------------------------------------------------------------------
std::string GetLastReportMessage();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CancelInFlightReportRequest
//--------------------------------------------------------------------------------
// Same cancellation shape as CancelInFlightUpdateRequest (github_update.h):
// closes this file's own WinHTTP handles from another thread so an addon
// unload/game close doesn't have to wait out a hung POST. Call alongside that one
// from AddonUnload.
//--------------------------------------------------------------------------------
void CancelInFlightReportRequest();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginReportShutdown / GetReportActiveThreadCount
//--------------------------------------------------------------------------------
// Same shape as github_update.h's BeginUpdateShutdown/GetUpdateActiveThreadCount,
// kept as separate functions instead of shared ones for the same reason this file
// doesn't otherwise share code with github_update.cpp -- distinctly named so both
// can be called independently from AddonUnload without a link collision. Call
// alongside CancelInFlightReportRequest from AddonUnload.
//--------------------------------------------------------------------------------
void BeginReportShutdown();
int  GetReportActiveThreadCount();