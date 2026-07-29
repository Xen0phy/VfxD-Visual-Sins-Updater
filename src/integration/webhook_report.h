//################################################################################
// webhook_report.h
//--------------------------------------------------------------------------------
// EReportStatus                  polling status for an in-flight/just-sent report
// ReportGuidBlock                one pre-rendered per-guid block
// StartSendReport()              validates and starts sending a report
// GetReportStatus()               current EReportStatus
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

//_ Discord's webhook message-content limit is 2000 characters. The relay
// (not this file -- see below) is what actually concatenates reporterLine
// + every entry's block + note into the message it posts, so this is a
// client-side guardrail rather than a byte-exact guarantee: five rendered
// GUID blocks comfortably stays under 2000 without this file needing to
// count characters itself. Enforced twice -- as the real safety net in
// StartSendReport below, and again in report_ui.cpp so the form doesn't
// let a user get this far in the first place.
constexpr size_t kMaxReportGuids = 5;

//********************************************************************************
// ReportGuidBlock
//--------------------------------------------------------------------------------
// guid    raw guid text, exactly as entered/auto-filled -- used for the
//         within-submission duplicate check
// block   fully pre-rendered display text for this guid (the "GUID: ... /
//         Type: ... / Self context: ..." lines)
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
// GetLastReportMessage
//--------------------------------------------------------------------------------
// Most recent one-line human-readable outcome, e.g. "Report sent -- thank
// you!", "2 of 3 already known -- 1 sent, thanks!", or an error. Empty if
// nothing's been sent yet this session.
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