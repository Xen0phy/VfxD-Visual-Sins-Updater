//################################################################################
// live_log.cpp   (see: live_log.h)
//--------------------------------------------------------------------------------

#include "effect_db.h"
#include "live_log.h"
#include "game_state.h"
#include "vfxd_sins_bridge.h"

#include <sstream>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// (anonymous namespace)
//--------------------------------------------------------------------------------
// Owns the subscribe/unsubscribe lifecycle, the one-entry-per-guid storage map
// (plus its for-science twin, see UpdateForScienceEntry), the drop-on-arrival
// "hide known" filter, and infostr parsing (see vfxd_sins_bridge.h for the wire
// format). Rendering stays in live_log_ui.cpp/addon.cpp, same as
// backup.cpp/report.cpp already do for their own sections.
//--------------------------------------------------------------------------------
namespace {

AddonAPI_t* s_api        = nullptr;
bool        s_listening  = false;
bool        s_hideKnown  = false;

//_ Gates IngestLogLine's ordinary-fold branch only; see LiveLog_SetForScienceOnly
bool        s_forScienceOnly = false;

//_ Types 0/1/9/11 default disabled, rest enabled; not persisted across reloads
bool s_typeEnabled[kLiveLogTypeCount] = {
    false, false, true, true, true, true, true, true, true, false, true, false
};

//_ Per-guid first-sight counter for render order; reset in LiveLog_Clear()
int s_nextSeq = 0;

std::unordered_map<std::string, LiveLogEntry> s_entries;        //. guid_b64 -> entry
std::unordered_map<std::string, std::string>  s_guidToName;     //. name map from addon.cpp
std::unordered_map<std::string, std::string>  s_guidToBehavior; //. behavior map from addon.cpp

//_ Display mirror of the effect db capture stream (see live_log.h)
std::unordered_map<std::string, LiveLogEntry> s_forScienceEntries; //. guid_b64 -> entry
int s_forScienceNextSeq = 0;   //. independent of s_nextSeq

//_ Module-scope "group currently open" state for AdvanceGroupState below
int          s_nextGroupId            = 0;
int          s_currentGroupId         = -1;
int          s_currentGroupDuration   = 0;
unsigned int s_currentGroupA4         = 0;
int          s_currentGroupStarterType = -1;   //. active group's starter type

//_ Starter guid for s_currentGroupId; feeds FeedEffectDb's group_members table
std::string  s_currentGroupStarterGuid;

//_ signature -> groupId map for content-addressed group identity; see Clear()
std::unordered_map<std::string, int> s_groupSignatureToId;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// MakeGroupSignature
//--------------------------------------------------------------------------------
// Builds the s_groupSignatureToId key for a starter line: two starters with the
// same guid/duration/a4 collapse onto the same groupId, no matter how far apart
// in time or how many unrelated groups sit between them (see AdvanceGroupState).
//--------------------------------------------------------------------------------
std::string MakeGroupSignature(const std::string& starterGuid, int duration, unsigned int a4)
{
    return starterGuid + "|" + std::to_string(duration) + "|" + std::to_string(a4);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// AdvanceGroupState
//--------------------------------------------------------------------------------
// Called unconditionally on every parsed line, before IngestLogLine's own
// type/hideKnown filters, so a dropped continuation line can't break group state
// while a dropped starter still blocks grouping. type:1/11 opens a group unless
// toggled off in s_typeEnabled, closing any group in progress; the id comes from
// a (starterGuid, duration, a4) signature (see MakeGroupSignature), reused on
// recurrence regardless of gap. A non-starter line joins only on an exact
// duration/a4 match; anything else closes the group (strict contiguity) and
// returns -1. Background-only mode (LiveLog_SetForScienceOnly) ignores
// s_typeEnabled here -- see typeGateOpen below. outGroupStarterGuid carries the
// starter guid to FeedEffectDb, cleared to "" on a -1 return.
//--------------------------------------------------------------------------------
int AdvanceGroupState(const std::string& starterGuid, int type, int duration, unsigned int a4,
                       std::string& outGroupStarterGuid)
{
    //_ Bypasses s_typeEnabled so background-only mode groups every type
    auto typeGateOpen = [](int t) { return s_forScienceOnly || s_typeEnabled[t]; };

    if (type == 1 || type == 11)
    {
        if (!typeGateOpen(type))
        {
            s_currentGroupId = -1;   //. starter filtered: closes any group
            s_currentGroupStarterGuid.clear();
            outGroupStarterGuid.clear();
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
        s_currentGroupStarterGuid = starterGuid;
        outGroupStarterGuid       = starterGuid;
        return s_currentGroupId;
    }

    if (s_currentGroupId >= 0
        && typeGateOpen(s_currentGroupStarterType)   //. starter's type must stay enabled
        && duration == s_currentGroupDuration
        && a4 == s_currentGroupA4)
    {
        outGroupStarterGuid = s_currentGroupStarterGuid;
        return s_currentGroupId;
    }

    //_ Closes any open group -- duration/a4 pairs recur across unrelated effects
    s_currentGroupId = -1;
    s_currentGroupStarterGuid.clear();
    outGroupStarterGuid.clear();
    return -1;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ParseInfoFields
//--------------------------------------------------------------------------------
// infostr's shape (see log_effect): an optional leading effectDef name (the one
// part that can contain spaces), then "type:" onward is a run of whitespace-
// delimited "key:value" tokens with no fixed order requirement, followed by
// optional trailing found_effect->name / " -> " + behavior text that isn't ours
// to parse anymore (see live_log.h: `behavior` is fully removed). Every field
// value is a single token with no embedded spaces, so each one is bounded by the
// next whitespace, not by searching for the next key's literal text.
//--------------------------------------------------------------------------------
void ParseInfoFields(const std::string& info, LiveLogEntry& e)
{
    //_ -1 default so a failed parse fails the bounds check, not aliases to type 0
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
// ParseLeadingBlock
//--------------------------------------------------------------------------------
// info's leading token, before "type:" -- e.g. "GDgna.cndZw" -- which
// ParseInfoFields above has always simply skipped past (its own
// info.find("type:") jump). Split on '.' into group/member. Left empty (both out
// params untouched) if this line has no dotted block at all -- not every infostr
// carries one, and guessing at a non-block-shaped leading token would be worse
// than leaving it blank.
//--------------------------------------------------------------------------------
void ParseLeadingBlock(const std::string& info, std::string& outGroup, std::string& outMember)
{
    size_t typePos = info.find("type:");
    if (typePos == std::string::npos)
        return;

    std::string leading = info.substr(0, typePos);
    size_t end = leading.find_last_not_of(" \t");
    if (end == std::string::npos)
        return;   //. whitespace-only prefix -- no block
    leading = leading.substr(0, end + 1);

    size_t dot = leading.find('.');
    if (dot == std::string::npos)
        return;   //. not block-shaped

    outGroup  = leading.substr(0, dot);
    outMember = leading.substr(dot + 1);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ParseTrailingName
//--------------------------------------------------------------------------------
// Everything after the last recognized key:value token, minus the " -> behavior"
// suffix this addon never parses further (see live_log.h on `behavior` being
// fully removed) -- e.g. "Cross 032 - Bullet Trail" out of "... target:self Cross
// 032 - Bullet Trail -> Hide". Empty for a nameless type 1/11 marker line. Walks
// every whitespace-delimited token, independent of field order (see
// ParseInfoFields), and returns whatever follows the last token shaped like a
// recognized key:value pair.
//--------------------------------------------------------------------------------
std::string ParseTrailingName(const std::string& info)
{
    static const std::string kKeys[] = { "type:", "duration:", "a4:", "caster:", "a6:", "target:" };

    std::istringstream tokens(info);
    std::string tok;
    size_t lastKeyEnd = std::string::npos;

    while (tokens >> tok)
    {
        bool isKey = false;
        for (const auto& k : kKeys)
        {
            if (tok.rfind(k, 0) == 0) { isKey = true; break; }
        }
        if (isKey)
        {
            auto pos = tokens.tellg();
            //_ tellg() returns -1 when exhausted; treat as "nothing follows"
            lastKeyEnd = (pos == std::istringstream::pos_type(-1)) ? info.size() : static_cast<size_t>(pos);
        }
    }

    if (lastKeyEnd == std::string::npos || lastKeyEnd > info.size())
        return "";

    std::string rest = info.substr(lastKeyEnd);
    size_t start = rest.find_first_not_of(" \t");
    if (start == std::string::npos)
        return "";
    rest = rest.substr(start);

    size_t arrow = rest.find(" -> ");
    if (arrow != std::string::npos)
        rest = rest.substr(0, arrow);

    size_t end = rest.find_last_not_of(" \t");
    return (end == std::string::npos) ? "" : rest.substr(0, end + 1);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ApplyGroupHistory
//--------------------------------------------------------------------------------
// Shared by IngestLogLine's ordinary entry and UpdateForScienceEntry -- factored
// out so the for-science branch's strip/tooltip (see live_log_ui.cpp's
// GroupStripColor and its caller) follows exactly the same "latest wins on
// groupId, append to recentGroupIds only on genuine change, cap at
// kLiveLogGroupHistoryCap" rule the ordinary branch already had, instead of a
// second, driftable copy of it.
//--------------------------------------------------------------------------------
void ApplyGroupHistory(LiveLogEntry& entry, int groupId)
{
    entry.groupId = groupId;   //. "latest wins", same as type/duration/a4

    //_ Appends only on a group change; ungrouped (-1) neither appends nor clears
    if (groupId >= 0 && (entry.recentGroupIds.empty() || entry.recentGroupIds.back() != groupId))
    {
        entry.recentGroupIds.push_back(groupId);
        if (entry.recentGroupIds.size() > kLiveLogGroupHistoryCap)
            entry.recentGroupIds.erase(entry.recentGroupIds.begin());
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FeedEffectDb
//--------------------------------------------------------------------------------
// The "for science" capture hook, called before this module's own type-
// toggle/hideKnown display filters -- those declutter the on-screen panel, not
// gate what's worth writing to the database, and s_typeEnabled starts types 1/11
// disabled even though those are the marker rows the database exists to
// correlate. Caster-only currently (see effect_db.h's kSelfMaskCaster), unlike
// the broader isSelfEvent below, which only feeds the display fold.
// groupStarterGuid passes straight through to EffectDbRawEvent, resolved by
// AdvanceGroupState. Bails before EffectDb_RecordEvent with no identity source
// attached (see GameState_GetProfession) -- recording then sets a sentinel bit
// that can't clear.
//--------------------------------------------------------------------------------
void FeedEffectDb(const std::string& guid_b64, const std::string& info, const LiveLogEntry& parsed,
                   const std::string& groupStarterGuid)
{
    if (!EffectDb_IsEnabled() || parsed.caster != "self")
        return;

    //_ No identity source yet; must bail here, not be filtered out later
    if (!GameState_IsRTAPILive() && !GameState_HasMumbleIdentity())
        return;

    EffectDbRawEvent ev;
    ev.guid_b64 = guid_b64;
    ev.name     = ParseTrailingName(info);
    ParseLeadingBlock(info, ev.blockGroup, ev.blockMember);
    ev.type             = parsed.type;
    ev.duration         = parsed.duration;
    ev.a4               = parsed.a4;
    ev.a6               = parsed.a6;
    ev.groupStarterGuid = groupStarterGuid;
    ev.selfMask = kSelfMaskCaster;

    //_ Read close together to shrink the window where RTAPI could flip mid-call
    ev.profession     = GameState_GetProfession();
    ev.race           = GameState_GetRace();
    ev.specialization = GameState_GetSpecialization();

    EffectDb_RecordEvent(ev);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// UpdateForScienceEntry
//--------------------------------------------------------------------------------
// Display-side twin of FeedEffectDb -- same gate (EffectDb_IsEnabled() && caster
// == "self"), so this map always matches what's landing in the db. Does not check
// s_typeEnabled or s_hideKnown -- this stream exists so "for science" isn't blind
// to whatever the ordinary display filters hide (see FeedEffectDb on why types
// 1/11 can't be filtered here). groupId comes from the same ApplyGroupHistory
// helper the ordinary entry uses; AdvanceGroupState's own s_typeEnabled gate
// still applies to grouping, so the two branches always agree on which lines are
// currently grouped.
//--------------------------------------------------------------------------------
void UpdateForScienceEntry(const std::string& guid_b64, const LiveLogEntry& parsed, int groupId)
{
    if (!EffectDb_IsEnabled() || parsed.caster != "self")
        return;

    bool known = s_guidToName.count(guid_b64) > 0;

    LiveLogEntry& entry = s_forScienceEntries[guid_b64];
    if (entry.seenCount == 0)
        entry.firstSeenSeq = s_forScienceNextSeq++;

    entry.guid_b64     = guid_b64;
    entry.knownInSin   = known;
    entry.displayName  = known ? s_guidToName.at(guid_b64) : guid_b64;
    entry.type         = parsed.type;
    entry.duration     = parsed.duration;
    entry.a4           = parsed.a4;
    entry.caster       = parsed.caster;
    entry.a6           = parsed.a6;
    entry.target       = parsed.target;
    ApplyGroupHistory(entry, groupId);
    entry.seenCount++;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// IngestLogLine
//--------------------------------------------------------------------------------
// The actual ingestion path, shared by the real Events_Subscribe callback below
// and directly usable by test code without going through Nexus at all. Takes
// plain strings, not the raw event struct, so it never has to trust the payload's
// lifetime past this call. Drops the event, before it ever becomes/updates an
// entry, if its type is toggled off or hideKnown applies; otherwise inserts-or-
// updates the one entry for this guid, "latest wins".
//--------------------------------------------------------------------------------
void IngestLogLine(const std::string& guid_b64, const std::string& info)
{
    //_ Scratch entry lets the filter check below reuse the freshly-parsed type
    LiveLogEntry parsed{};
    ParseInfoFields(info, parsed);

    //_ Runs even for a dropped line -- group state must stay continuous
    std::string groupStarterGuid;
    int groupId = AdvanceGroupState(guid_b64, parsed.type, parsed.duration, parsed.a4, groupStarterGuid);

    //_ Runs unconditionally, before either filter below -- see FeedEffectDb
    FeedEffectDb(guid_b64, info, parsed, groupStarterGuid);
    UpdateForScienceEntry(guid_b64, parsed, groupId);   //. same gate as FeedEffectDb

    //_ Background-capture mode; only skips the ordinary s_entries fold below
    if (s_forScienceOnly)
        return;

    if (parsed.type >= 0 && parsed.type < kLiveLogTypeCount && !s_typeEnabled[parsed.type])
        return;   //. type toggled off

    bool known = s_guidToName.count(guid_b64) > 0;
    if (s_hideKnown && known)
        return;   //. hideKnown drop

    LiveLogEntry& entry = s_entries[guid_b64]; //. insert-or-get, repeats collapse here
    if (entry.seenCount == 0)
        entry.firstSeenSeq = s_nextSeq++;   //. first sight only

    entry.guid_b64     = guid_b64;
    entry.knownInSin   = known;
    entry.displayName  = known ? s_guidToName.at(guid_b64) : guid_b64;
    //_ Independent lookup, guarded separately in case it drifts from s_guidToName
    entry.installedBehavior = (known && s_guidToBehavior.count(guid_b64)) ? s_guidToBehavior.at(guid_b64) : "";
    entry.type     = parsed.type;
    entry.duration = parsed.duration;
    entry.a4       = parsed.a4;
    entry.caster   = parsed.caster;
    entry.a6       = parsed.a6;
    entry.target   = parsed.target;
    ApplyGroupHistory(entry, groupId);   //. shared with UpdateForScienceEntry

    entry.seenCount++;

    //_ Self-context fields set only on an exact caster/target == "self" match
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
// Events_Subscribe callback for EV_VFXD_SINS_LOG. Copies the event's pointers out
// immediately, since they're only valid for the duration of this callback (see
// vfxd_sins_bridge.h), then hands off to IngestLogLine.
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

const std::unordered_map<std::string, LiveLogEntry>& LiveLog_GetForScienceEntries()
{
    return s_forScienceEntries;
}

void LiveLog_SetForScienceOnly(bool forScienceOnly)
{
    s_forScienceOnly = forScienceOnly;
}

bool LiveLog_GetForScienceOnly()
{
    return s_forScienceOnly;
}

void LiveLog_Clear()
{
    s_entries.clear();
    s_nextSeq = 0;
    s_forScienceEntries.clear();
    s_forScienceNextSeq = 0;

    //_ Resets group state so nothing stale carries over from before the clear
    s_nextGroupId            = 0;
    s_currentGroupId         = -1;
    s_currentGroupDuration   = 0;
    s_currentGroupA4         = 0;
    s_currentGroupStarterType = -1;
    s_currentGroupStarterGuid.clear();
    s_groupSignatureToId.clear();
}