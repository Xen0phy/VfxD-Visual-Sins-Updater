//################################################################################
// webhook_report.h
//--------------------------------------------------------------------------------
// EReportStatus                  polling status for an in-flight/just-sent report
// EReportOutcome                 which of sent/partially-sent/none-sent a Done report was
// ReportGuidBlock                one pre-rendered per-guid block
// StartSendReport()              validates and starts sending a report
// GetReportStatus()               current EReportStatus
// GetLastReportOutcome()          outcome kind for GetLastReportMessage()'s text
// GetLastReportMessage()          most recent human-readable outcome
// CancelInFlightReportRequest()   closes in-flight WinHTTP handles
//--------------------------------------------------------------------------------
// "Report an effect back" feature. Reworked from a single free-text-box
// version: this is now a reporter identity line plus zero or more per-guid
// blocks (each carrying its own Type and self-context snapshot) plus one
// required free-text note.
//
// Same overall networking shape as github_update.cpp -- synchronous
// WinHTTP call on a short-lived detached background thread, an atomic
// in-flight flag, a shutdown-hook cancel function -- but deliberately not
// sharing any code with it: github_update.cpp only ever GETs two fixed
// GitHub hosts, this only ever POSTs one relay URL, and the two files have
// no other reason to know about each other.
//
// Posts to the vfxd-sins-report-relay Cloudflare Worker, not straight to
// Discord -- the relay holds the real Discord webhook server-side (never
// in this addon), does its own per-guid cross-user dedup, and rate-limits
// abuse. The Worker never parses or understands the contents of a guid's
// block -- it only ever handles an opaque string per guid plus that guid
// itself as a dedup key, so this file owns rendering every line of
// human-readable text that ends up in the Discord message.
//
// The relay URL is stored XOR-obfuscated rather than as a plain string --
// see webhook_config.example.h for what that does and doesn't protect
// against, and webhook_report.cpp's DecodeWebhookUrl for where it's
// reconstructed.
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
// Only meaningful once GetReportStatus() == Done -- distinguishes the
// relay's three possible per-entry outcomes for a successful submission
// (None outside that: not yet sent, or the last attempt errored) so
// report_ui.cpp can color/word the outcome without re-deriving it from
// raw guid counts itself. Mirrors the relay's "status" response field --
// see vfxd-sins-report-relay/src/index.js's top-of-file response doc.
//--------------------------------------------------------------------------------
enum class EReportOutcome
{
    None,          //. no report done yet this session, or the last one errored
    AllSent,       //. every submitted guid was new (or there were no guids at all)
    PartiallySent, //. some guids were already known, the rest still went through
    NoneSent,      //. every submitted guid was already known -- nothing forwarded
};

//_ A soft cap on row count, not the real length guarantee -- see
// EstimateDiscordContentLength in webhook_report.cpp for the actual
// character-counting check StartSendReport runs before every send.
constexpr size_t kMaxReportGuids = 5;

//********************************************************************************
// ReportGuidBlock
//--------------------------------------------------------------------------------
// guid    raw guid text, exactly as entered/auto-filled -- used for the
//         within-submission duplicate check
// block   fully pre-rendered display text for this guid: a "GUID:
//         `<guid>`" line followed by a fenced code block with its Type
//         and self-context lines
//--------------------------------------------------------------------------------
// One guid block, fully rendered client-side before it's ever handed to
// this file -- webhook_report.cpp never knows what a Type or a
// self-context field actually means, it only ever sees the finished text
// plus the raw guid. Building `block`'s exact text is addon.cpp's job (see
// RenderReportSection), since that's the only place that already has
// GameState_ProfessionName/RaceName/SpecializationName on hand to render
// human-readable values instead of raw enum ids.
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
// reporterLine is sent as-is, whatever literal "Reporter: ..." line the
// caller composed (addon.cpp already knows the anonymous checkbox and
// GameState_Get*Name(), this file doesn't need to). entries may be empty
// on purpose -- a guid-less "something's wrong" note is still a valid
// report -- but is rejected if any entry's guid is blank or repeats
// (after trimming) within this submission, or if entries.size() exceeds
// kMaxReportGuids; that's the only guid-dedup done client-side,
// cross-user dedup is the relay's job. note is required
// and rejected if blank/whitespace-only.
// Returns false with no network activity if validation fails or a report
// is already in flight (outError explains why); true once the background
// send has started -- poll GetReportStatus()/GetLastReportMessage() from
// there, same pattern as github_update.cpp's Start*/Get* split.
//--------------------------------------------------------------------------------
bool StartSendReport(const std::string& reporterLine,
                     const std::vector<ReportGuidBlock>& entries,
                     const std::string& note,
                     std::string& outError);

EReportStatus GetReportStatus();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetLastReportOutcome
//--------------------------------------------------------------------------------
// EReportOutcome for the same result GetLastReportMessage() describes in
// prose -- None until a report has actually completed Done, and again
// after any Error.
//--------------------------------------------------------------------------------
EReportOutcome GetLastReportOutcome();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GetLastReportMessage
//--------------------------------------------------------------------------------
// Most recent one-line human-readable outcome, e.g. "Report sent -- thank
// you!", "2 of 3 GUID(s) already known -- 1 sent, thanks!", "All submitted
// GUIDs were already known -- nothing new to send, thanks anyway!", or an
// error. Empty if nothing's been sent yet this session.
//--------------------------------------------------------------------------------
std::string GetLastReportMessage();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CancelInFlightReportRequest
//--------------------------------------------------------------------------------
// Same cancellation shape as CancelInFlightUpdateRequest (github_update.h):
// closes this file's own WinHTTP handles from another thread so an addon
// unload/game close doesn't have to wait out a hung POST. Call alongside
// that one from Addon_Unload.
//--------------------------------------------------------------------------------
void CancelInFlightReportRequest();

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// BeginReportShutdown / GetReportActiveThreadCount
//--------------------------------------------------------------------------------
// Same shape as github_update.h's BeginUpdateShutdown/GetUpdateActiveThreadCount, kept as
// separate functions (rather than shared ones) for the same reason this
// file doesn't otherwise share code with github_update.cpp -- distinctly
// named so both can be called independently from AddonUnload without a
// link collision. Call alongside CancelInFlightReportRequest from
// AddonUnload.
//--------------------------------------------------------------------------------
void BeginReportShutdown();
int  GetReportActiveThreadCount();