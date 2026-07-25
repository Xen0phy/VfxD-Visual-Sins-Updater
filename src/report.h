#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// "Report an effect back" feature -- see HANDOFF_LiveLogEnrichment.md's
// "Report redesign -- decided plan" section for the full design this
// implements. Reworked from the earlier single free-text-box version:
// this is now a reporter identity line plus zero or more per-GUID blocks
// (each carrying its own Type and self-context snapshot) plus one required
// free-text note.
//
// Same overall networking shape as github_update.cpp -- synchronous WinHTTP
// call on a short-lived detached background thread, an atomic in-flight
// flag, a shutdown-hook cancel function -- but deliberately not sharing any
// code with it: github_update.cpp only ever GETs two fixed GitHub hosts,
// this only ever POSTs one relay URL, and the two files have no other
// reason to know about each other.
//
// Posts to the vfxd-sins-report-relay Cloudflare Worker, not straight to
// Discord -- the relay holds the real Discord webhook server-side (never
// in this addon), does its own per-guid cross-user dedup, and rate-limits
// abuse. The Worker never parses or understands the contents of a GUID's
// block -- it only ever handles an opaque string per guid plus that guid
// itself as a dedup key, so this file owns rendering every line of
// human-readable text that ends up in the Discord message.
//
// The relay URL itself is NOT a secret the way a raw Discord webhook would
// be (it's rate-limited and does nothing but relay+validate), but it's
// still not in this header or its .cpp as a plain string -- see
// webhook_config.h (gitignored, not committed; regenerate it via
// tools/generate_webhook_config.py, see that script and
// webhook_config.example.h for details). It's stored there XOR-obfuscated
// rather than as plain text, mainly to avoid it showing up trivially in a
// `strings` scan, and only reconstructed in memory right before each send
// -- see report.cpp's DecodeWebhookUrl for exactly what that does and
// doesn't protect against.
// ---------------------------------------------------------------------------

enum class EReportStatus
{
    Idle,
    Sending,
    Done,
    Error,
};

// One GUID block, fully rendered client-side before it's ever handed to
// this file -- report.cpp never knows what a Type or a self-context field
// actually mean, it only ever sees the finished text plus the raw guid
// (used for client-side duplicate-within-submission checking; the relay
// separately dedups against everyone else's known-guid set). Building
// `block`'s exact text (the "GUID: ... / Type: ... / Self context: ..."
// template) is addon.cpp's job -- see RenderReportSection -- since that's
// the only place that already has GameState_ProfessionName/RaceName/
// SpecializationName on hand to render human-readable values instead of
// raw enum ids.
struct ReportGuidBlock
{
    std::string guid;  // raw guid text, exactly as entered/auto-filled -- used for the within-submission duplicate check
    std::string block; // fully pre-rendered display text for this guid (GUID/Type/Self-context lines)
};

// Validates, and if valid, starts sending a report on a background thread.
//
// reporterLine: exactly one of "Reporter: <AccountName> / <CharacterName>"
// or "Reporter: (anonymous)" -- composed by the caller (addon.cpp already
// knows about the anonymous checkbox and GameState_Get*Name(), this file
// doesn't need to). Never validated here beyond "not touched" -- whichever
// literal line the caller built is sent as-is.
//
// entries: zero or more per-GUID blocks. Zero is valid on purpose -- same
// as before this rework, a report doesn't strictly need a GUID attached
// (a general "something's wrong" note is still a valid report). Rejected
// if any entry's guid is blank, or if the same guid (after trimming)
// appears more than once in this same submission -- a paste mistake, not
// a real second report. This is the only guid-dedup performed client-side;
// dedup against guids already known (by this user's installed sin files,
// or by anyone else who's reported them before) is left entirely to the
// relay's cross-user known-guid set.
//
// note: required. Rejected if blank/whitespace-only.
//
// Returns false immediately -- no network activity at all -- if validation
// fails or a report is already in flight; outError explains why in either
// case. Returns true once the background send has actually been started;
// from there, poll GetReportStatus()/GetLastReportMessage() for the
// outcome, same pattern as github_update.cpp's Start*/Get* split.
bool StartSendReport(const std::string& reporterLine,
                     const std::vector<ReportGuidBlock>& entries,
                     const std::string& note,
                     std::string& outError);

EReportStatus GetReportStatus();

// Most recent one-line human-readable outcome, e.g. "Report sent -- thank
// you!", "2 of 3 already known -- 1 sent, thanks!", "All submitted GUIDs
// were already known -- nothing new to send, thanks anyway!", or an error.
// Empty if nothing's been sent yet this session.
std::string GetLastReportMessage();

// Same cancellation shape as CancelInFlightUpdateRequest (github_update.h):
// closes this file's own WinHTTP handles from another thread so an addon
// unload/game close doesn't have to wait out a hung POST. Call alongside
// that one from Addon_Unload.
void CancelInFlightReportRequest();