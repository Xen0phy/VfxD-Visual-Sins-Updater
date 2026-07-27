// live_log.cpp
//
// See live_log.h for the module contract and vfxd_sins_bridge.h for the
// wire format. This file owns: the subscribe/unsubscribe lifecycle, the
// one-entry-per-guid storage map, the drop-on-arrival "hide known" filter,
// and infostr parsing. Rendering (the CollapsingHeader, the tree, the
// toggles) stays in addon.cpp alongside every other section, same as
// backup.cpp/report.cpp already do for their own sections.
#include "core/live_log.h"
#include "integration/vfxd_sins_bridge.h"
#include <sstream>

namespace {

AddonAPI_t* s_api        = nullptr;
bool        s_listening  = false;
bool        s_hideKnown  = false;

// Defaults from characterizing real captured data: types 0,
// 1, 9, 11 start disabled (never/rarely visible, a group toggle, or a
// near-duplicate of another group toggle -- not useful to see by default),
// everything else starts enabled. Plain array, not persisted anywhere --
// reset to these defaults every time the DLL loads, by construction of
// being a static initializer.
bool s_typeEnabled[kLiveLogTypeCount] = {
    false, false, true, true, true, true, true, true, true, false, true, false
};

// Monotonic counter for LiveLogEntry::firstSeenSeq -- assigned once per
// guid, on genuine first sight, so the render order can follow "received
// order" without re-deriving it from anything else. Reset alongside
// LiveLog_Clear() so a cleared log's next entry starts back at 0 rather
// than continuing to climb.
int s_nextSeq = 0;

std::unordered_map<std::string, LiveLogEntry> s_entries;        // guid_b64 -> entry
std::unordered_map<std::string, std::string>  s_guidToName;     // last name map handed to us by addon.cpp
std::unordered_map<std::string, std::string>  s_guidToBehavior; // last behavior map handed to us by addon.cpp

// infostr's shape (see log_effect): an optional leading effectDef name
// (the one part that CAN contain spaces), then "type:" onward is a run of
// whitespace-delimited "key:value" tokens with no fixed order requirement,
// followed by optional trailing found_effect->name / " -> " + behavior
// text that isn't ours to parse anymore (see live_log.h: `behavior` is
// fully removed). Since every field value is confirmed to be a single
// token with no embedded spaces, each one is bounded by "read to the next
// whitespace" rather than by searching for the next key's literal text --
// this removes the old target-is-last special case (no more " -> "
// search) and stops depending on keys appearing in a fixed order at all.
void ParseInfoFields(const std::string& info, LiveLogEntry& e)
{
    size_t pos = info.find("type:"); // skip past the optional leading effectDef name
    if (pos == std::string::npos)
        return; // malformed line -- nothing to parse

    std::istringstream tokens(info.substr(pos));
    std::string tok;
    while (tokens >> tok)
    {
        size_t eq = tok.find(':');
        if (eq == std::string::npos)
            break; // trailing "found_effect->name [-> behavior]" text -- ignore, not our concern anymore

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
            else break; // unrecognized token -- start of trailing name/behavior text, stop here
        }
        catch (...)
        {
            break; // malformed numeric token -- reject rather than silently truncate, same spirit as before
        }
    }
}

// The actual ingestion path, shared by the real Events_Subscribe callback
// below and usable directly by test code without going through Nexus at
// all. Deliberately takes plain strings, not the raw event struct, so it
// never has to trust the payload's lifetime past this call.
void IngestLogLine(const std::string& guid_b64, const std::string& info)
{
    // Parsed into a scratch entry first, not the real map slot, so the
    // filter check below can use the freshly-parsed type without a
    // separate single-field parse pass (today's ParseTypeValue is gone --
    // the same typed value now serves both the filter check and storage).
    LiveLogEntry parsed{};
    ParseInfoFields(info, parsed);

    if (parsed.type >= 0 && parsed.type < kLiveLogTypeCount && !s_typeEnabled[parsed.type])
        return; // this type is toggled off -- dropped before ever becoming/updating an entry

    bool known = s_guidToName.count(guid_b64) > 0;
    if (s_hideKnown && known)
        return; // dropped here -- never becomes/updates an entry, nothing written in the background for it

    LiveLogEntry& entry = s_entries[guid_b64]; // insert-or-get: repeats collapse onto the same entry
    if (entry.seenCount == 0)
        entry.firstSeenSeq = s_nextSeq++; // only on genuine first sight of this guid -- never touched again

    entry.guid_b64     = guid_b64;
    entry.knownInSin   = known;
    entry.displayName  = known ? s_guidToName.at(guid_b64) : guid_b64;
    // Independent lookup, never derived from the event itself -- see
    // live_log.h's field comment. s_guidToBehavior is built from the same
    // installed-sin walk as s_guidToName, but guarded separately in case
    // the two maps are ever out of sync (e.g. mid-refresh).
    entry.installedBehavior = (known && s_guidToBehavior.count(guid_b64)) ? s_guidToBehavior.at(guid_b64) : "";
    entry.type     = parsed.type;
    entry.duration = parsed.duration;
    entry.a4       = parsed.a4;
    entry.caster   = parsed.caster;
    entry.a6       = parsed.a6;
    entry.target   = parsed.target;
    entry.seenCount++;

    // Self-only enrichment: written only when this event's caster or
    // target is "self" (exact reproduction of VfxDenoiser's own
    // pointer-identity self-check).
    // A non-self event for a guid that already has these populated leaves
    // them completely untouched -- not cleared, not overwritten.
    bool isSelfEvent = (parsed.caster == "self" || parsed.target == "self");
    if (isSelfEvent)
    {
        entry.mapID          = GameState_GetMapID();
        entry.profession     = GameState_GetProfession();
        entry.specialization = GameState_GetSpecialization();
        entry.race           = GameState_GetRace(); // always Mumble -- see game_state.h's "Known gap" note
        entry.hasSelfContext = true;
    }
}

void OnVfxdSinsLog(void* aEventArgs)
{
    if (!aEventArgs)
        return;

    const auto* evt = static_cast<const VfxSinsLogEvent*>(aEventArgs);
    if (evt->struct_version != kVfxSinsLogEventVersion)
        return; // unknown shape -- ignore rather than misread it

    // Copy out immediately: these pointers are only valid for the duration
    // of this callback (see vfxd_sins_bridge.h).
    std::string guid_b64 = evt->guid_b64 ? evt->guid_b64 : "";
    std::string info     = evt->info     ? evt->info     : "";
    IngestLogLine(guid_b64, info);
}

} // namespace

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
        return; // no real state change -- don't spam the event bus from a checkbox re-rendering every frame

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
        return true; // fail open -- an unrecognized type is never silently dropped
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
}