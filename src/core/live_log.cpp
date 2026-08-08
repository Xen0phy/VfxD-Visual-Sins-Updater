//################################################################################
// live_log.cpp
//--------------------------------------------------------------------------------
// See live_log.h for the module contract and vfxd_sins_bridge.h for the
// wire format. This file owns: the subscribe/unsubscribe lifecycle, the
// one-entry-per-guid storage map, the drop-on-arrival "hide known"
// filter, and infostr parsing. Rendering (the CollapsingHeader, the tree,
// the toggles) stays in addon.cpp alongside every other section, same as
// backup.cpp/report.cpp already do for their own sections.
//--------------------------------------------------------------------------------

#include "live_log.h"
#include "vfxd_sins_bridge.h"

#include <sstream>

namespace {

AddonAPI_t* s_api        = nullptr;
bool        s_listening  = false;
bool        s_hideKnown  = false;

//_ Types 0, 1, 9, 11 start disabled (rarely useful by default); the rest
// start enabled -- from characterizing real captured data. Not persisted
// anywhere; resets to these defaults every time the DLL loads.
bool s_typeEnabled[kLiveLogTypeCount] = {
    false, false, true, true, true, true, true, true, true, false, true, false
};

//_ Assigned once per guid on genuine first sight (see firstSeenSeq), so
// render order can follow "received order" without re-deriving it. Reset
// alongside LiveLog_Clear() so a cleared log's next entry starts at 0.
int s_nextSeq = 0;

std::unordered_map<std::string, LiveLogEntry> s_entries;        //. guid_b64 -> entry
std::unordered_map<std::string, std::string>  s_guidToName;     //. name map from addon.cpp
std::unordered_map<std::string, std::string>  s_guidToBehavior; //. behavior map from addon.cpp

//_ Running "is a group currently open" state for AdvanceGroupState below.
// Lives at module scope, not per-entry, since grouping is a property of
// arrival order -- a single guid can drift through several groups.
int          s_nextGroupId            = 0;
int          s_currentGroupId         = -1;
int          s_currentGroupDuration   = 0;
unsigned int s_currentGroupA4         = 0;
int          s_currentGroupStarterType = -1;   //. which of {1, 11} opened the active group

//_ signature -> groupId, for content-addressed group identity (see
// AdvanceGroupState below). Session-lifetime; cleared in LiveLog_Clear.
std::unordered_map<std::string, int> s_groupSignatureToId;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// MakeGroupSignature
//--------------------------------------------------------------------------------
// Builds the s_groupSignatureToId key for a starter line: two starters
// with the same guid/duration/a4 collapse onto the same groupId, no
// matter how far apart in time or how many unrelated groups sit between
// them (see AdvanceGroupState).
//--------------------------------------------------------------------------------
std::string MakeGroupSignature(const std::string& starterGuid, int duration, unsigned int a4)
{
    return starterGuid + "|" + std::to_string(duration) + "|" + std::to_string(a4);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// AdvanceGroupState
//--------------------------------------------------------------------------------
// Called unconditionally on every parsed line, before IngestLogLine's own
// type/hideKnown filters -- a dropped *continuation* line must not break
// group state, but a dropped *starter* type must still block grouping
// entirely (see below).
//
// type:1 / type:11 opens a group, unless that starter type is itself
// toggled off in s_typeEnabled -- then nothing opens, and any group in
// progress closes immediately. The id is looked up by (starterGuid,
// duration, a4) signature (see MakeGroupSignature), not freshly minted,
// so a recurrence of the same starter effect/numbers reuses the same
// groupId no matter how long ago it last showed up.
//
// A non-starter line joins the open group only if its duration and a4
// match the starter's exactly; anything else -- including a toggled-off
// type -- closes the group outright (strict contiguity: no gap of
// non-matching lines survives mid-group) and returns -1 (ungrouped).
//--------------------------------------------------------------------------------
int AdvanceGroupState(const std::string& starterGuid, int type, int duration, unsigned int a4)
{
    if (type == 1 || type == 11)
    {
        if (!s_typeEnabled[type])
        {
            s_currentGroupId = -1;   //. starter filtered off: nothing opens, and anything open closes
            return -1;
        }

        std::string sig = MakeGroupSignature(starterGuid, duration, a4);
        auto it = s_groupSignatureToId.find(sig);
        s_currentGroupId = (it != s_groupSignatureToId.end())
            ? it->second
            : (s_groupSignatureToId[sig] = s_nextGroupId++);

        s_currentGroupDuration    = duration;
        s_currentGroupA4          = a4;
        s_currentGroupStarterType = type;
        return s_currentGroupId;
    }

    if (s_currentGroupId >= 0
        && s_typeEnabled[s_currentGroupStarterType]   //. group's own starter type must still be on
        && duration == s_currentGroupDuration
        && a4 == s_currentGroupA4)
        return s_currentGroupId;

    //_ Closes whatever group was open, not just this line -- (duration,
    // a4) pairs get reused by unrelated effects, so without this,
    // unrelated lines could silently keep joining on shared numbers.
    s_currentGroupId = -1;
    return -1;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ParseInfoFields
//--------------------------------------------------------------------------------
// infostr's shape (see log_effect): an optional leading effectDef name
// (the one part that can contain spaces), then "type:" onward is a run of
// whitespace-delimited "key:value" tokens with no fixed order
// requirement, followed by optional trailing found_effect->name / " -> "
// + behavior text that isn't ours to parse anymore (see live_log.h:
// `behavior` is fully removed). Every field value is a single token with
// no embedded spaces, so each one is bounded by "read to the next
// whitespace" rather than by searching for the next key's literal text.
//--------------------------------------------------------------------------------
void ParseInfoFields(const std::string& info, LiveLogEntry& e)
{
    //_ Overrides the default 0 -- a missing/failed "type:" parse should
    // fail IngestLogLine's bounds check and show unfiltered, not silently
    // alias to type 0 (disabled) and vanish the entry without a trace.
    e.type = -1;

    size_t pos = info.find("type:");   //. skip the leading effectDef name
    if (pos == std::string::npos)
        return;   //. malformed line

    std::istringstream tokens(info.substr(pos));
    std::string tok;
    while (tokens >> tok)
    {
        size_t eq = tok.find(':');
        if (eq == std::string::npos)
            break;   //. trailing name/behavior text

        std::string key = tok.substr(0, eq + 1);
        std::string val = tok.substr(eq + 1);

        try
        {
            if (key == "type:")          e.type     = std::stoi(val);
            else if (key == "duration:") e.duration = std::stoi(val);
            else if (key == "a4:")       e.a4       = static_cast<unsigned int>(std::stoul(val));
            else if (key == "caster:")   e.caster   = val;
            else if (key == "a6:")       e.a6       = val;
            else if (key == "target:")   e.target   = val;
            else break;   //. unrecognized token, stop here
        }
        catch (...)
        {
            break;   //. malformed token, don't truncate
        }
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IngestLogLine
//--------------------------------------------------------------------------------
// The actual ingestion path, shared by the real Events_Subscribe callback
// below and directly usable by test code without going through Nexus at
// all. Takes plain strings, not the raw event struct, so it never has to
// trust the payload's lifetime past this call. Drops the event, before it
// ever becomes/updates an entry, if its type is toggled off or hideKnown
// applies; otherwise inserts-or-updates the one entry for this guid,
// "latest wins".
//--------------------------------------------------------------------------------
void IngestLogLine(const std::string& guid_b64, const std::string& info)
{
    //_ Parsed into a scratch entry first so the filter check below can
    // use the freshly-parsed type without a separate parse pass.
    LiveLogEntry parsed{};
    ParseInfoFields(info, parsed);

    //_ Always runs, even for a line about to be dropped below -- group
    // state must stay continuous regardless of per-type display filters
    // (see AdvanceGroupState).
    int groupId = AdvanceGroupState(guid_b64, parsed.type, parsed.duration, parsed.a4);

    if (parsed.type >= 0 && parsed.type < kLiveLogTypeCount && !s_typeEnabled[parsed.type])
        return;   //. type toggled off

    bool known = s_guidToName.count(guid_b64) > 0;
    if (s_hideKnown && known)
        return;   //. hideKnown drop

    LiveLogEntry& entry = s_entries[guid_b64]; //. insert-or-get: repeats collapse onto the same entry
    if (entry.seenCount == 0)
        entry.firstSeenSeq = s_nextSeq++;   //. first sight only

    entry.guid_b64     = guid_b64;
    entry.knownInSin   = known;
    entry.displayName  = known ? s_guidToName.at(guid_b64) : guid_b64;
    //_ Independent lookup, never derived from the event (see live_log.h)
    // -- guarded separately from s_guidToName in case the two go out of sync.
    entry.installedBehavior = (known && s_guidToBehavior.count(guid_b64)) ? s_guidToBehavior.at(guid_b64) : "";
    entry.type     = parsed.type;
    entry.duration = parsed.duration;
    entry.a4       = parsed.a4;
    entry.caster   = parsed.caster;
    entry.a6       = parsed.a6;
    entry.target   = parsed.target;
    entry.groupId  = groupId;   //. "latest wins", same as type/duration/a4 above

    //_ Append only when the group actually differs from the last one
    // recorded -- same-group repeats don't grow this, and an ungrouped
    // event neither appends nor clears history (groupId == -1).
    if (groupId >= 0 && (entry.recentGroupIds.empty() || entry.recentGroupIds.back() != groupId))
    {
        entry.recentGroupIds.push_back(groupId);
        if (entry.recentGroupIds.size() > kLiveLogGroupHistoryCap)
            entry.recentGroupIds.erase(entry.recentGroupIds.begin());
    }

    entry.seenCount++;

    //_ Written only when this event's caster or target is "self" (exact
    // match of VfxDenoiser's own pointer-identity check) -- see LiveLogEntry.
    bool isSelfEvent = (parsed.caster == "self" || parsed.target == "self");
    if (isSelfEvent)
    {
        entry.mapID          = GameState_GetMapID();
        entry.profession     = GameState_GetProfession();
        entry.specialization = GameState_GetSpecialization();
        entry.race           = GameState_GetRace();
        entry.hasSelfContext = true;
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// OnVfxdSinsLog
//--------------------------------------------------------------------------------
// Events_Subscribe callback for EV_VFXD_SINS_LOG. Copies the event's
// pointers out immediately, since they're only valid for the duration of
// this callback (see vfxd_sins_bridge.h), then hands off to IngestLogLine.
//--------------------------------------------------------------------------------
void OnVfxdSinsLog(void* aEventArgs)
{
    if (!aEventArgs)
        return;

    const auto* evt = static_cast<const VfxSinsLogEvent*>(aEventArgs);
    if (evt->struct_version != kVfxSinsLogEventVersion)
        return;   //. unknown shape, ignore

    std::string guid_b64 = evt->guid_b64 ? evt->guid_b64 : "";
    std::string info     = evt->info     ? evt->info     : "";
    IngestLogLine(guid_b64, info);
}

} //. namespace

void LiveLog_Init(AddonAPI_t* aApi)
{
    s_api = aApi;
    s_api->Events_Subscribe(EV_VFXD_SINS_LOG, OnVfxdSinsLog);
}

void LiveLog_Shutdown(AddonAPI_t* aApi)
{
    if (!aApi)
        return;

    if (s_listening)
        aApi->Events_RaiseNotification(EV_VFXD_SINS_LISTEN_STOP);

    aApi->Events_Unsubscribe(EV_VFXD_SINS_LOG, OnVfxdSinsLog);
    s_listening = false;
    s_api = nullptr;
}

void LiveLog_SetListening(AddonAPI_t* aApi, bool listening)
{
    if (listening == s_listening)
        return;   //. no real change

    s_listening = listening;
    if (aApi)
        aApi->Events_RaiseNotification(s_listening ? EV_VFXD_SINS_LISTEN_START : EV_VFXD_SINS_LISTEN_STOP);
}

bool LiveLog_IsListening()
{
    return s_listening;
}

void LiveLog_SetHideKnown(bool hide)
{
    s_hideKnown = hide;
}

bool LiveLog_GetHideKnown()
{
    return s_hideKnown;
}

bool LiveLog_GetTypeEnabled(int type)
{
    if (type < 0 || type >= kLiveLogTypeCount)
        return true;   //. fail open
    return s_typeEnabled[type];
}

void LiveLog_SetTypeEnabled(int type, bool enabled)
{
    if (type < 0 || type >= kLiveLogTypeCount)
        return;
    s_typeEnabled[type] = enabled;
}

void LiveLog_SetKnownGuidNames(const std::unordered_map<std::string, std::string>& guidToName)
{
    s_guidToName = guidToName;
}

void LiveLog_SetKnownGuidBehaviors(const std::unordered_map<std::string, std::string>& guidToBehavior)
{
    s_guidToBehavior = guidToBehavior;
}

const std::unordered_map<std::string, LiveLogEntry>& LiveLog_GetEntries()
{
    return s_entries;
}

void LiveLog_Clear()
{
    s_entries.clear();
    s_nextSeq = 0;

    //_ So a cleared log's next group starts at 0 with no stale "currently
    // open group" or stale signatures carried over from before the clear
    // (see s_groupSignatureToId).
    s_nextGroupId            = 0;
    s_currentGroupId         = -1;
    s_currentGroupDuration   = 0;
    s_currentGroupA4         = 0;
    s_currentGroupStarterType = -1;
    s_groupSignatureToId.clear();
}