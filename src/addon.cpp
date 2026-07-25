// addon.cpp
//
// The addon's actual behavior, split out from entry.cpp's bare Nexus
// wiring: locating VfxDenoiser, the options-panel UI (RT_OptionsRender),
// kicking off the initial silent update check on load, and tearing all of
// that down again on unload. All the update-check/merge logic itself
// lives in sin_files.*, github_update.* and merge.*; this file is UI glue
// plus the addon's own state (which folder it's pointed at, what's
// currently cached for display) over that.
//
// The addon has no floating window of its own -- everything lives inside
// Nexus's own options panel (RT_OptionsRender), registered once and drawn
// only while that panel is open.
#include "addon.h"
#include "imgui.h"
#include "github_update.h"
#include "sin_files.h"
#include "report.h"
#include "backup.h"
#include "live_log.h"
#include "game_state.h"
#include "specialization_names.h"
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <utility>

namespace fs = std::filesystem;

static std::string s_denoiserAddonDir;

// Set once, in Addon_Load, to the same AddonAPI_t pointer entry.cpp got from
// Nexus. Only used for aApi->Log calls from this file (SaveInstalledSinFile's
// write-failure path) -- never reassigned afterward, so reading it later is
// safe without a lock, same as s_denoiserAddonDir below.
static AddonAPI_t* s_api = nullptr;

// Set once, in Addon_Load, to true only if VfxDenoiser's addon folder
// actually exists -- avoids repeatedly rescanning a folder we already know
// isn't there.
static std::atomic<bool> s_denoiserFound{false};

// ---------------------------------------------------------------------------
// Always-visible read-only effect tree (separate from the update diff view
// below it). Reads whatever's actually on disk right now via
// ScanInstalledSinFiles + a plain ifstream >> json, independent of the
// updater's own cached oldFile copies in github_update.cpp -- this is a
// browsing concern, not part of the update-check/merge pipeline.
//
// Loaded lazily (first time the header is opened, or "Refresh" is pressed)
// and cached rather than re-read from disk every frame the panel is open.
// This is also the renderer the future right-click-to-edit feature is meant
// to extend -- see HANDOFF's open decision #1.
// ---------------------------------------------------------------------------
static bool                                            s_installedTreeLoaded = false;
static std::vector<InstalledSinFile>                   s_installedSins;
static std::unordered_map<std::string, nlohmann::ordered_json> s_installedJson; // sinName -> parsed file, only present if it parsed OK

// Bumped every time s_installedJson is (re)loaded from disk -- see
// LoadInstalledEffectsTree. Exists purely to invalidate s_overlayCache
// below: anything that forces a reload already sets s_installedTreeLoaded
// = false first (every ApplyPending* function, "Refresh", an apply/install
// completing), so tying cache invalidation to this one counter covers all
// of those for free rather than needing a bump at every call site that
// mutates the tree.
static int s_installedTreeGeneration = 0;

// Per-sin cache for the duplicate/diff overlay trees RenderInstalledEffects
// paints onto the installed tree (see BuildDuplicateOverlayTree/
// BuildDiffOverlayTree below). Building either means deep-copying the
// entire installed file plus a recursive tag pass -- real work for a large
// file (thousands of effects isn't unusual, see HANDOFF's
// seed_known_guids.json) -- and this used to happen again on every single
// ImGui frame the tree was open, whether or not anything had actually
// changed since the last frame. That's the direct cause of a reported bug:
// with an overlay active and the tree tall enough to need a scrollbar, the
// per-frame rebuild made frame time long enough that mouse-wheel scrolling
// felt like it had stopped working outright (wheel deltas the host
// accumulates between frames get eaten by a stalled one) -- reproducible
// only with an overlay showing and only once there was enough content to
// make the rebuild expensive, which matches exactly how it was reported.
// Invalidated on s_installedTreeGeneration changing (the file itself was
// reloaded/edited) or the sin's own EDiffStatus changing (a diff only ever
// produces one MergePlan per Ready transition -- StartLoadDiff replacing
// it always goes through NotLoaded/Loading again first, which this catches
// too).
struct OverlayCacheEntry
{
    int         generation = -1;
    EDiffStatus diffStatus = EDiffStatus::NotLoaded;
    nlohmann::ordered_json file;
};
static std::unordered_map<std::string, OverlayCacheEntry> s_overlayCache;

// Set right when the user clicks Install or Apply changes (both live in the
// top action row's per-sin button now, see RenderSinActionRow) --
// StartInstallSin/StartApplyUpdate already
// serialize with each other via github_update.cpp's own single in-flight
// guard, so at most one of these is ever meaningfully "the" pending one;
// this just lets the right column say "Installing.../Applying..." instead
// of every column reading the same generic busy state.
static std::string s_pendingActionSin;

namespace {

// nlohmann::json::dump() always emits bare '\n' line endings, but every
// VfxDenoiser file shipped/edited in the wild uses CRLF. Converting here
// (rather than leaving dump()'s output as-is) keeps a saved file's line
// endings consistent with what it had on disk before the edit, instead of
// silently flipping the whole file to LF the first time someone edits a
// single effect.
std::string ToCrlf(const std::string& lfText)
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

// Joins a category path like {"Combat", "Downstate"} into "Combat / Downstate".
std::string JoinPath(const std::vector<std::string>& path)
{
    std::string out;
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (i) out += " / ";
        out += path[i];
    }
    return out;
}

// Colors used to flag pending-update content overlaid onto the installed-
// effects tree (see BuildDiffOverlayTree): a brand-new effect not yet
// applied, an existing effect whose GUIDs would be refreshed, and any
// category that contains one of those somewhere underneath it. One color
// per kind of change, used consistently whether it's painting the leaf
// effect itself or an ancestor category header that contains one --
// green for "new", orange for "reworked" (a rework wins the category tint
// over a new effect if a category has both underneath, since a rework is
// the thing worth a second look). Chosen to read clearly against imgui's
// default dark theme without being confused for the existing error-red
// used elsewhere in this file.
static const ImVec4 kNewColor       (0.40f, 0.85f, 0.40f, 1.0f); // green
static const ImVec4 kReworkColor    (0.95f, 0.60f, 0.20f, 1.0f); // orange
static const ImVec4 kDuplicateColor (0.90f, 0.25f, 0.25f, 1.0f); // red

// True if `sinName` currently has any guid appearing on more than one
// installed effect (see FindDuplicateGuids in merge.h/.cpp). Populated once
// per LoadInstalledEffectsTree call, not recomputed every frame -- this is
// a property of the on-disk file, not of anything update-related, so it
// only needs to change when the tree is (re)loaded.
static std::unordered_map<std::string, std::vector<std::string>> s_duplicateGuidsBySin;

// Recursively marks the ONE effect owning any guid in `oldGuids` -- not
// every effect sharing a name -- with a "__vfxd_rework" display-only
// marker plus `newGuids` (as "__vfxd_new_guids", so the tree can show old
// GUIDs alongside what they'd become), and bubbles a "__vfxd_hasrework"
// marker up onto every category that contains that match -- this is what
// gives that category (and every ancestor above it) the orange tint,
// taking priority over "__vfxd_hasnew" (see BuildDiffOverlayTree) since a
// rework is the thing worth a second look. Returns whether anything under
// `category` changed, so the caller can tag ancestors too.
//
// Matched by guid rather than name because names aren't guaranteed unique
// (GW2 reuses display names across genuinely distinct effects) -- matching
// by name here used to tag every same-named effect with the same
// `newGuids`, even though only one of them was the real target and the
// others were untouched. Guids are globally unique, so checking for any
// overlap with `oldGuids` (the matched effect's exact guid list at resolve
// time) identifies the one specific node unambiguously.
bool TagReworkEffect(nlohmann::ordered_json& category, const std::vector<std::string>& oldGuids,
                     const std::vector<std::string>& newGuids)
{
    bool changed = false;

    if (category.contains("effects") && category["effects"].is_array())
    {
        for (auto& eff : category["effects"])
        {
            if (!eff.contains("guids") || !eff["guids"].is_array())
                continue;

            bool isMatch = false;
            for (const auto& g : eff["guids"])
                if (g.is_string())
                    for (const auto& og : oldGuids)
                        if (g.get<std::string>() == og) { isMatch = true; break; }

            if (isMatch)
            {
                eff["__vfxd_rework"]    = true;
                eff["__vfxd_new_guids"] = newGuids;
                changed = true;
            }
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
    {
        for (auto& sub : category["categories"])
            if (TagReworkEffect(sub, oldGuids, newGuids))
                changed = true;
    }

    if (changed)
        category["__vfxd_hasrework"] = true;

    return changed;
}

// Deep-copies `installed` and overlays `plan` onto it purely for display,
// so the installed-effects tree stays the single source of truth for what
// a pending update would do instead of a second, separate list:
//   - reworks are tagged onto the matching existing effect in place
//     ("__vfxd_rework", plus its "__vfxd_new_guids" so the old GUIDs
//     already on the effect and what they'd become can both be shown)
//   - inserts are appended under their target category path, creating any
//     category that doesn't exist yet in the installed file (tagged
//     "__vfxd_virtual" -- it isn't real yet, see below), tagged
//     "__vfxd_new"
//   - every category from an overlaid node up to the root is tagged
//     "__vfxd_hasnew" so RenderCategoryTree can tint ancestor headers green
//     -- unless a rework is also present somewhere in that ancestor chain
//     ("__vfxd_hasrework", set by TagReworkEffect above), which tints
//     orange instead and takes priority
// "__vfxd_virtual" additionally tells RenderCategoryTree to suppress the
// right-click "Rename" menu on that category, and "__vfxd_new" suppresses
// "Edit" on that effect -- neither exists in the real on-disk file yet
// (that's what applying the update would do), so editing/renaming them
// now would just fail to re-find them when Applied. Nothing here is ever
// written back to disk -- these marker fields exist only in this
// in-memory copy. RenderCategoryTree's existing "unexpected field"
// fallback for effects explicitly skips them so a marker can never leak
// into the visible field list (see the skip-list there).
nlohmann::ordered_json BuildDiffOverlayTree(const nlohmann::ordered_json& installed, const MergePlan& plan)
{
    nlohmann::ordered_json overlay = installed;
    if (!overlay.contains("categories") || !overlay["categories"].is_array())
        overlay["categories"] = nlohmann::ordered_json::array();

    for (const auto& rw : plan.reworks)
        for (auto& cat : overlay["categories"])
            TagReworkEffect(cat, rw.oldGuids, rw.newGuids);

    for (const auto& ins : plan.inserts)
    {
        nlohmann::ordered_json* cursor = &overlay;
        for (const auto& segment : ins.categoryPath)
        {
            if (!cursor->contains("categories") || !(*cursor)["categories"].is_array())
                (*cursor)["categories"] = nlohmann::ordered_json::array();

            nlohmann::ordered_json* next = nullptr;
            for (auto& sub : (*cursor)["categories"])
            {
                if (sub.value("name", std::string()) == segment)
                {
                    next = &sub;
                    break;
                }
            }
            if (!next)
            {
                nlohmann::ordered_json newCat;
                newCat["name"]          = segment;
                newCat["categories"]    = nlohmann::ordered_json::array();
                newCat["effects"]       = nlohmann::ordered_json::array();
                newCat["__vfxd_virtual"] = true; // doesn't exist on disk yet
                (*cursor)["categories"].push_back(std::move(newCat));
                next = &(*cursor)["categories"].back();
            }

            (*next)["__vfxd_hasnew"] = true;
            cursor = next;
        }

        if (!cursor->contains("effects") || !(*cursor)["effects"].is_array())
            (*cursor)["effects"] = nlohmann::ordered_json::array();

        nlohmann::ordered_json newEffect        = ins.effect;
        newEffect["__vfxd_new"]         = true;
        (*cursor)["effects"].push_back(std::move(newEffect));
    }

    return overlay;
}

// Recursively marks every effect owning one of `dupeGuids` with a display-
// only "__vfxd_dupe_guid" marker, and bubbles a "__vfxd_hasdupe" marker up
// onto every ancestor category that contains one -- same shape as
// TagReworkEffect above, but flagging a correctness problem already present
// in the installed file itself (see FindDuplicateGuids), not a pending
// update. Returns whether anything under `category` was tagged, so the
// caller can tag ancestors too.
bool TagDuplicateGuidEffects(nlohmann::ordered_json& category, const std::unordered_set<std::string>& dupeGuids)
{
    bool changed = false;

    if (category.contains("effects") && category["effects"].is_array())
    {
        for (auto& eff : category["effects"])
        {
            if (!eff.contains("guids") || !eff["guids"].is_array())
                continue;

            bool isDupe = false;
            for (const auto& g : eff["guids"])
                if (g.is_string() && dupeGuids.count(g.get<std::string>()))
                {
                    isDupe = true;
                    break;
                }

            if (isDupe)
            {
                eff["__vfxd_dupe_guid"] = true;
                changed = true;
            }
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
        for (auto& sub : category["categories"])
            if (TagDuplicateGuidEffects(sub, dupeGuids))
                changed = true;

    if (changed)
        category["__vfxd_hasdupe"] = true;

    return changed;
}

// Deep-copies `installed` and tags it with duplicate-guid markers for
// display, same reasoning as BuildDiffOverlayTree below: these markers must
// never reach s_installedJson itself, since that's the copy
// ApplyPendingEdit/SaveInstalledSinFile eventually serialize back to disk
// verbatim. Called independently of any update/diff overlay -- this is
// about the file as it sits on disk right now, not about a pending change.
nlohmann::ordered_json BuildDuplicateOverlayTree(const nlohmann::ordered_json& installed, const std::vector<std::string>& dupeGuids)
{
    nlohmann::ordered_json overlay = installed;
    std::unordered_set<std::string> dset(dupeGuids.begin(), dupeGuids.end());

    if (overlay.contains("categories") && overlay["categories"].is_array())
        for (auto& cat : overlay["categories"])
            TagDuplicateGuidEffects(cat, dset);

    return overlay;
}

// `color` is optional -- pass nullptr for the default text color (used for
// a plain guids list), or a color to tint every bullet line (used to show
// a reworked effect's post-update GUIDs in kReworkColor, distinct from its
// current GUIDs just above in the default color).
void RenderGuidList(const char* label, const std::vector<std::string>& guids, const ImVec4* color = nullptr)
{
    if (guids.empty())
    {
        ImGui::TextDisabled("%s: (none)", label);
        return;
    }

    ImGui::TextDisabled("%s:", label);
    ImGui::Indent();
    if (color)
        ImGui::PushStyleColor(ImGuiCol_Text, *color);
    for (const auto& g : guids)
        ImGui::BulletText("%s", g.c_str());
    if (color)
        ImGui::PopStyleColor();
    ImGui::Unindent();
}

// Prints one key/value pair that isn't part of the confirmed effect/
// category schema below (name/description/guids/behaviors). This is only
// a forward-compat fallback now -- e.g. if a future VfxDenoiser version
// adds a new field -- rendered generically by JSON type so an unknown
// field still shows up as *something* rather than silently vanishing.
void RenderJsonValue(const std::string& key, const nlohmann::ordered_json& value)
{
    switch (value.type())
    {
        case nlohmann::ordered_json::value_t::string:
            ImGui::BulletText("%s: %s", key.c_str(), value.get<std::string>().c_str());
            break;
        case nlohmann::ordered_json::value_t::boolean:
            ImGui::BulletText("%s: %s", key.c_str(), value.get<bool>() ? "true" : "false");
            break;
        case nlohmann::ordered_json::value_t::number_integer:
        case nlohmann::ordered_json::value_t::number_unsigned:
            ImGui::BulletText("%s: %lld", key.c_str(), static_cast<long long>(value.get<int64_t>()));
            break;
        case nlohmann::ordered_json::value_t::number_float:
            ImGui::BulletText("%s: %g", key.c_str(), value.get<double>());
            break;
        case nlohmann::ordered_json::value_t::null:
            ImGui::BulletText("%s: (null)", key.c_str());
            break;
        case nlohmann::ordered_json::value_t::array:
        case nlohmann::ordered_json::value_t::object:
        default:
            // Unknown shape -- dump it compactly rather than guess a
            // schema-specific rendering for a nested object/array.
            ImGui::Bullet();
            ImGui::TextWrapped("%s: %s", key.c_str(), value.dump().c_str());
            break;
    }
}

// Renders one entry of an effect's "behaviors" array. Confirmed real
// shape (from a sample Collection.json): {"type": "Hide"|"Show"|
// "SetDuration", "caster": "Self"|"Others"|"All", plus "duration"
// (milliseconds, per VfxDenoiser's own README) only when type is
// SetDuration}.
void RenderBehavior(const nlohmann::ordered_json& behavior)
{
    std::string type   = behavior.value("type", std::string("?"));
    std::string caster = behavior.value("caster", std::string("?"));

    if (type == "SetDuration" && behavior.contains("duration") && behavior["duration"].is_number())
    {
        ImGui::BulletText("Set duration: %gms for %s", behavior["duration"].get<double>(), caster.c_str());
    }
    else
    {
        ImGui::BulletText("%s for %s", type.c_str(), caster.c_str());
    }

    // Anything beyond type/caster/duration is unexpected -- surface it
    // rather than silently dropping it.
    for (const auto& [key, value] : behavior.items())
    {
        if (key == "type" || key == "caster" || key == "duration")
            continue;
        ImGui::Indent();
        RenderJsonValue(key, value);
        ImGui::Unindent();
    }
}

// (Re)scans the VfxDenoiser folder and reloads every installed sin file's
// JSON into s_installedJson. A file that fails to open/parse is simply
// left absent from the map rather than aborting the whole refresh --
// RenderInstalledEffects shows a per-sin error line for that case so one
// corrupt file doesn't hide the other two.
void LoadInstalledEffectsTree()
{
    s_installedSins = ScanInstalledSinFiles(s_denoiserAddonDir);
    s_installedJson.clear();
    s_duplicateGuidsBySin.clear();

    for (const auto& sin : s_installedSins)
    {
        std::ifstream in(sin.fullPath, std::ios::binary);
        if (!in)
            continue;

        nlohmann::ordered_json parsed;
        try
        {
            in >> parsed;
        }
        catch (const nlohmann::ordered_json::exception&)
        {
            continue; // malformed file on disk -- leave it out of the map, not fatal
        }

        // Checked once here, against the real on-disk file, independent of
        // whether an update is even available -- this is a property of
        // this file in isolation (see merge.h's FindDuplicateGuids doc
        // comment), not something StartLoadDiff/ResolveMergePlan need to
        // discover on their own. github_update.cpp's StartLoadDiff runs
        // this same check again on its own read of the file before
        // touching the network, so a duplicate found here and one found
        // there are consistent -- neither trusts the other's cache.
        s_duplicateGuidsBySin[sin.sinName] = FindDuplicateGuids(parsed);

        s_installedJson[sin.sinName] = std::move(parsed);
    }

    s_installedTreeLoaded     = true;
    ++s_installedTreeGeneration;
}

// Recursively walks every effect anywhere under `category`, keeping each
// effect's name alongside its guids -- what RenderLiveLogSection needs to
// resolve an incoming guid to a display name. A guid appearing on more
// than one effect (see s_duplicateGuidsBySin) just keeps whichever name
// is visited last; that ambiguity already exists on-disk and isn't this
// map's concern to fix.
void CollectGuidNamesRecursive(const nlohmann::ordered_json& category,
                                std::unordered_map<std::string, std::string>& out)
{
    if (category.contains("effects") && category["effects"].is_array())
    {
        for (const auto& eff : category["effects"])
        {
            if (!eff.contains("guids") || !eff["guids"].is_array() ||
                !eff.contains("name") || !eff["name"].is_string())
                continue;

            std::string name = eff["name"].get<std::string>();
            for (const auto& g : eff["guids"])
                if (g.is_string())
                    out[g.get<std::string>()] = name;
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            CollectGuidNamesRecursive(sub, out);
}

// guid -> effect name across every currently-loaded installed sin file.
// Passed into LiveLog_SetKnownGuidNames rather than having live_log.cpp
// read s_installedJson directly -- same "passed in, not read from
// statics" shape used elsewhere in this addon. Caller is responsible for
// the installed tree having been loaded at least once first, same as
// LoadInstalledEffectsTree's other callers.
std::unordered_map<std::string, std::string> CollectGuidNameMap()
{
    std::unordered_map<std::string, std::string> out;
    for (const auto& [sinName, file] : s_installedJson)
    {
        if (file.contains("categories") && file["categories"].is_array())
            for (const auto& cat : file["categories"])
                CollectGuidNamesRecursive(cat, out);
    }
    return out;
}

// Flattens one effect's "behaviors" array (schema per RenderBehavior above)
// into a single display string. An effect can legitimately carry more than
// one behavior at once (e.g. Hide for Others + Show for Self), so entries
// are joined with "; " rather than assuming exactly one.
std::string FormatBehaviors(const nlohmann::ordered_json& behaviors)
{
    std::string out;
    for (const auto& behavior : behaviors)
    {
        std::string type   = behavior.value("type", std::string("?"));
        std::string caster = behavior.value("caster", std::string("?"));

        std::string one;
        if (type == "SetDuration" && behavior.contains("duration") && behavior["duration"].is_number())
            one = "Set duration: " + std::to_string(behavior["duration"].get<double>()) + "ms for " + caster;
        else
            one = type + " for " + caster;

        if (!out.empty())
            out += "; ";
        out += one;
    }
    return out;
}

// Same recursive walk as CollectGuidNamesRecursive, but keeping each
// effect's own formatted "behaviors" summary instead of its name -- what
// RenderLiveLogSection needs to independently resolve an incoming guid to
// *this user's own* currently-configured Hide/Show/SetDuration, rather
// than ever trusting whatever behavior text a real event happens to carry
// (that reflects VfxDenoiser's own installed copy, which could be
// organized completely differently -- see HANDOFF's "Gap found" writeup).
// Same last-write-wins note as CollectGuidNamesRecursive for a guid
// duplicated across effects. An effect with no "behaviors" array still
// gets an (empty-string) entry, so a known guid is distinguishable from
// one that's merely unconfigured.
void CollectGuidBehaviorsRecursive(const nlohmann::ordered_json& category,
                                    std::unordered_map<std::string, std::string>& out)
{
    if (category.contains("effects") && category["effects"].is_array())
    {
        for (const auto& eff : category["effects"])
        {
            if (!eff.contains("guids") || !eff["guids"].is_array())
                continue;

            std::string summary = (eff.contains("behaviors") && eff["behaviors"].is_array())
                                       ? FormatBehaviors(eff["behaviors"])
                                       : std::string();

            for (const auto& g : eff["guids"])
                if (g.is_string())
                    out[g.get<std::string>()] = summary;
        }
    }

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            CollectGuidBehaviorsRecursive(sub, out);
}

// guid -> this user's own configured-behavior summary, across every
// currently-loaded installed sin file. Passed into
// LiveLog_SetKnownGuidBehaviors the same "handed in, not read from
// statics" way CollectGuidNameMap is passed into LiveLog_SetKnownGuidNames.
std::unordered_map<std::string, std::string> CollectGuidBehaviorMap()
{
    std::unordered_map<std::string, std::string> out;
    for (const auto& [sinName, file] : s_installedJson)
    {
        if (file.contains("categories") && file["categories"].is_array())
            for (const auto& cat : file["categories"])
                CollectGuidBehaviorsRecursive(cat, out);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Right-click-to-edit (HANDOFF open decision #1). The tree stays read-only
// display by default; right-clicking an effect and choosing "Edit" is what
// makes that one node's fields editable. Scoped deliberately narrow to this
// addon's own concerns -- name, description, guids, and category placement.
// Behaviors (Hide/Show/SetDuration) are NEVER made editable here; those stay
// owned by VfxDenoiser's own UI, always rendered read-only via RenderBehavior
// regardless of edit state.
//
// Only one edit can be in flight at a time, addon-wide -- mirrors the
// existing single-in-flight-request philosophy in github_update.cpp, and
// avoids the ambiguity of two half-finished edits landing in whatever order
// imgui happens to render them.
// ---------------------------------------------------------------------------

struct EditState
{
    bool                      active = false;
    std::string               sinName;
    std::vector<int>          originalPath; // category identity: root -> immediate parent, each element that level's index within its parent's "categories" array (see FindCategoryByPath)
    std::string               originalName; // effect's name at the moment editing started, used for display/messages and as a sanity check on save
    int                        originalIndex = -1; // this effect's position within originalPath's "effects" array at BeginEdit time -- the actual identity key, since two sibling effects can share a name (see HANDOFF)

    char nameBuf[256]         = {};
    char descBuf[1024]        = {};
    char guidsBuf[2048]       = {}; // one guid per line
};
static EditState s_edit;

// Set by the Save button; consumed once, after the whole tree has finished
// rendering for this frame, so the effect array is never mutated mid-walk.
struct EditSaveJob
{
    std::string               sinName;
    std::vector<int>          originalPath; // see EditState::originalPath
    std::string               originalName;
    int                        originalIndex = -1; // position within originalPath's "effects" array -- see EditState::originalIndex
    std::string                newName;
    std::string                newDescription;
    std::vector<std::string>   newGuids;
};
static bool        s_hasPendingSave = false;
static EditSaveJob s_pendingSave;
static std::string s_editResultMessage; // shown until the next edit action (effect edit or category rename), success or failure

// Category rename -- a deliberately much smaller sibling of the effect
// editor above. Only the category's own "name" field is editable; moving
// a category (i.e. changing its parent) isn't offered here since that's a
// bigger, riskier operation (it would silently take every effect and
// subcategory underneath it along for the ride) and nothing's asked for
// that yet. Shares the same "only one edit in flight addon-wide" rule as
// effect editing -- see EditState's comment -- so an effect edit and a
// category rename can never be open at the same time either.
struct CategoryEditState
{
    bool                      active = false;
    std::string               sinName;
    std::vector<int>          path; // this category's own identity, root -> ... -> this category, inclusive (see EditState::originalPath)
    char                       nameBuf[256] = {};
};
static CategoryEditState s_categoryEdit;

struct CategoryRenameJob
{
    std::string               sinName;
    std::vector<int>          path; // root -> ... -> this category, inclusive (same convention as CategoryEditState::path)
    std::string               newName;
};
static bool              s_hasPendingCategoryRename = false;
static CategoryRenameJob s_pendingCategoryRename;

// ---------------------------------------------------------------------------
// Drag-and-drop between categories, and reordering within one (HANDOFF open
// decision #7). Follows the same "record a job, apply it once after the
// whole tree has finished rendering this frame" shape as the edit/rename
// jobs above -- the effects/categories arrays are never mutated mid-walk.
//
// Two drop targets share this one job type: dropping onto a category's own
// row appends to the end of its "effects" (destinationIndex left at -1),
// and dropping onto a specific effect's row inserts immediately above that
// effect (destinationIndex set to that effect's index). Between the two,
// every position in a category's list is reachable -- the effect-row
// target reaches everything except the very last slot, and the category-
// row target reaches exactly that slot -- without either drop zone needing
// to distinguish "above" from "below" within a single row.
//
// Scoped to moving one effect at a time into a category *within the same
// installed sin file* (the destination may be the effect's own current
// category, for reordering, or a different one) -- moving a whole category
// (which would also carry every effect/subcategory underneath it) isn't
// offered, same reasoning CategoryEditState's comment already gives for why
// category rename doesn't offer reparenting either. Cross-file moves (e.g.
// Gluttony -> Pride) aren't offered either: nothing in the confirmed schema
// says an effect from one sin file has any meaning in another's tree, so
// keeping this to same-file moves avoids inventing semantics nobody asked
// for.
//
// Unlike RenderEffectEditor's Save, a move never touches the effect's own
// content (name/description/guids/behaviors/any unknown field) -- only
// which category owns it, and where within that category's list, changes.
// See ApplyPendingMove.
//
// Categories themselves can also be dragged (CategoryDragPayload /
// CategoryMoveJob below), but deliberately reorder-only: a category only
// ever lands back among its own *current* siblings, at a new position --
// dragging it into a *different* parent (reparenting, which would carry
// every effect/subcategory underneath it along for the ride) isn't
// offered, same call CategoryEditState's comment already makes for why
// rename doesn't offer reparenting either. That's why CategoryMoveJob has
// no destinationPath the way EffectMoveJob does: where a category ends up
// is never in question, only where within its unchanged parent's list.
// ---------------------------------------------------------------------------

// ImGui's SetDragDropPayload copies raw bytes, which doesn't map cleanly
// onto a std::string + std::vector<int> category path (arbitrary length,
// no fixed size to copy). Simpler to keep the actual identifying info
// here -- set once in BeginDragDropSource, which imgui calls every frame
// the item is held, so it can't go stale mid-drag -- and pass only a
// fixed-size marker as the payload itself; AcceptDragDropPayload uses the
// marker purely to confirm a drop of the right *type* landed, then reads
// the real data from here.
struct EffectDragPayload
{
    std::string               sinName;
    std::vector<int>          originalPath; // see EditState::originalPath
    std::string               effectName;
    int                        originalIndex = -1; // position within originalPath's "effects" array -- see EditState::originalIndex
};
static EffectDragPayload s_dragPayload;
static const int         kEffectDragMarker = 1; // payload bytes are a placeholder; see EffectDragPayload's comment

struct EffectMoveJob
{
    std::string               sinName;
    std::vector<int>          originalPath;
    std::string               effectName;
    int                        originalIndex = -1; // position within originalPath's "effects" array -- see EditState::originalIndex
    std::vector<int>          destinationPath; // root -> ... -> target category, inclusive; empty means "this sin file's top level" -- this is a real existing category's own index path (captured from the tree at drop time, see RenderCategoryTree's drag-drop target), not typed text, so it's index-based same as originalPath
    int                        destinationIndex = -1; // -1 means "append" (dropped on destinationPath's own row); otherwise the index, within destinationPath's "effects" array *as captured at drop time, before any erase has run*, that the dragged effect should end up immediately above (dropped on that effect's row) -- see ApplyPendingMove for how this is reconciled against the source erase when originalPath == destinationPath
};
static bool          s_hasPendingMove = false;
static EffectMoveJob s_pendingMove;

// Payload for dragging a category itself, as opposed to an effect
// (EffectDragPayload above) -- same "ImGui payload is just a fixed-size
// type marker, the real data lives in a static struct kept current every
// frame BeginDragDropSource runs" reasoning as EffectDragPayload's own
// comment.
struct CategoryDragPayload
{
    std::string       sinName;
    std::vector<int>  path; // this category's own identity, root -> ... -> this category, inclusive (same convention as CategoryEditState::path) -- path.back() is this category's own index within its parent's "categories" array, path.begin()..end()-1 is that parent's own path
};
static CategoryDragPayload s_categoryDragPayload;
static const int           kCategoryDragMarker = 1; // payload bytes are a placeholder; see CategoryDragPayload's comment

// Reorder-only (see this section's header comment): a category's
// destination is always its own *current* parent's "categories" array, so
// unlike EffectMoveJob there's no separate destinationPath -- only where
// within that unchanged list it ends up.
struct CategoryMoveJob
{
    std::string       sinName;
    std::vector<int>  originalPath; // this category's own identity at drag time -- see CategoryDragPayload::path
    int               destinationIndex = -1; // -1 means "append" (dropped on the shared parent's own row -- that parent's own TreeNode for a nested category, or the sin file's root row for a top-level one); otherwise the sibling index, within that same parent's "categories" array *as captured at drop time, before the source erase*, that this category should end up immediately above (dropped on that sibling's own row) -- see ApplyPendingCategoryMove for the same erase-shift adjustment EffectMoveJob's destinationIndex needed
};
static bool            s_hasPendingCategoryMove = false;
static CategoryMoveJob s_pendingCategoryMove;

// ---------------------------------------------------------------------------
// Delete (effects always; categories only when empty) and "add category".
// Same "record state while rendering, apply once after the whole tree has
// finished this frame" shape as edit/rename/move above.
// ---------------------------------------------------------------------------

// Confirmation state for a pending delete. Rendered inline right next to
// the item's own row (see the "-" SmallButton in RenderCategoryTree),
// which is always visible whether or not that row's TreeNode is open --
// so unlike EditState/CategoryEditState, this deliberately does NOT get
// cancelled just because the owning node is collapsed; nothing about it
// was ever hidden by collapsing in the first place.
struct DeleteConfirmState
{
    bool                      active     = false;
    bool                      isCategory = false;
    std::string               sinName;
    std::vector<int>          path;        // effect: containing category's identity; category: this category's OWN identity, inclusive (see EditState::originalPath)
    int                       index       = -1; // effect index within path's "effects" array -- see EditState::originalIndex; unused for a category delete
    std::string                displayName; // just for the confirmation text/messages
};
static DeleteConfirmState s_deleteConfirm;

struct DeleteJob
{
    bool                      isCategory = false;
    std::string               sinName;
    std::vector<int>          path;
    int                       index = -1;
    std::string                name; // sanity check at apply time -- see ApplyPendingDelete
};
static bool      s_hasPendingDelete = false;
static DeleteJob s_pendingDelete;

// "Add category" prompt state. Rendered inside the parent category's
// TreeNode (same idea as RenderCategoryEditor for rename), so -- unlike
// DeleteConfirmState -- this DOES get cancelled on collapse, same
// reasoning as EditState/CategoryEditState.
struct CreateCategoryState
{
    bool                      active = false;
    std::string               sinName;
    std::vector<int>          parentPath; // where the new category goes; empty means this sin file's top level (see EditState::originalPath for the indexing convention)
    char                       nameBuf[256] = {};
};
static CreateCategoryState s_createCategory;

struct CreateCategoryJob
{
    std::string               sinName;
    std::vector<int>          parentPath;
    std::string                newName;
};
static bool               s_hasPendingCreateCategory = false;
static CreateCategoryJob  s_pendingCreateCategory;

// True if an effect edit, category rename, delete confirmation, or
// create-category prompt is open anywhere (addon-wide, not just this
// sin's tree). Gates every action that would start a new one of these, so
// at most one is ever in flight at a time -- see EditState's comment for
// why that matters.
bool AnyEditInFlight()
{
    return s_edit.active || s_categoryEdit.active || s_deleteConfirm.active || s_createCategory.active;
}

// Splits `text` into trimmed, non-empty lines. Used to turn the guids
// text box back into a guid list.
std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line))
    {
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end   = line.find_last_not_of(" \t\r\n");
        if (start == std::string::npos)
            continue; // blank line
        out.push_back(line.substr(start, end - start + 1));
    }
    return out;
}

// True if `prefix` is `path`'s first N elements (N = prefix.size()),
// including the case where they're equal. Used to tell whether an
// in-progress edit lives at or underneath a category that's about to be
// collapsed -- see the "cancel on collapse" checks in RenderCategoryTree.
bool PathHasPrefix(const std::vector<int>& path, const std::vector<int>& prefix)
{
    if (prefix.size() > path.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (path[i] != prefix[i])
            return false;
    return true;
}

// Read-only category lookup by index path, starting from `root` (a sin
// file's top-level json, which has its own "categories" array exactly
// like any other category node). Each element of `path` is that level's
// position within its parent's "categories" array at the moment the path
// was captured during a tree walk (see `pathSoFar` in RenderCategoryTree)
// -- category identity is index-based, mirroring how effect edit/drag
// identity already works (see EditState::originalIndex's comment), so
// same-named sibling categories can never collide here the way they used
// to when this matched by name. Returns nullptr if any segment is out of
// range for the array at that point -- callers treat that as "the tree
// changed since editing started, don't guess," same as the old
// missing-name case.
nlohmann::ordered_json* FindCategoryByPath(nlohmann::ordered_json& root, const std::vector<int>& path)
{
    nlohmann::ordered_json* cursor = &root;
    for (int idx : path)
    {
        if (!cursor->contains("categories") || !(*cursor)["categories"].is_array())
            return nullptr;

        auto& cats = (*cursor)["categories"];
        if (idx < 0 || static_cast<size_t>(idx) >= cats.size())
            return nullptr;

        cursor = &cats[idx];
    }
    return cursor;
}

// Same walk as FindCategoryByPath, but collects each step's "name" field
// instead of returning a pointer, joined the same way JoinPath formats a
// typed destination ("Combat / Downstate") -- used only for display
// (result/error messages), never for identity. Meant to be called right
// after a FindCategoryByPath lookup on the same path has already
// succeeded; if the tree were to change in between, this just resolves as
// much as it still can rather than asserting.
std::string JoinCategoryPathNames(const nlohmann::ordered_json& root, const std::vector<int>& path)
{
    std::vector<std::string> names;
    const nlohmann::ordered_json* cursor = &root;
    for (int idx : path)
    {
        if (!cursor->contains("categories") || !(*cursor)["categories"].is_array())
            break;
        const auto& cats = (*cursor)["categories"];
        if (idx < 0 || static_cast<size_t>(idx) >= cats.size())
            break;
        cursor = &cats[idx];
        names.push_back(cursor->value("name", std::string("(unnamed category)")));
    }
    return JoinPath(names);
}

// Writes s_installedJson[sinName] back to the file it was loaded from,
// using the same backup-then-tmp-then-rename safety pattern as
// github_update.cpp's StartApplyUpdate: never touch the real file directly,
// so a crash or failed write can't corrupt or lose the user's data. Unlike
// an applied GitHub update, an edit never changes the filename (no version
// bump), so this always writes back to the exact path it read from.
bool SaveInstalledSinFile(const std::string& sinName, std::string& outError)
{
    auto jsonIt = s_installedJson.find(sinName);
    if (jsonIt == s_installedJson.end())
    {
        outError = "no in-memory copy of this file to save";
        return false;
    }

    std::string fullPath;
    for (const auto& sin : s_installedSins)
    {
        if (sin.sinName == sinName)
        {
            fullPath = sin.fullPath;
            break;
        }
    }
    if (fullPath.empty())
    {
        outError = "couldn't find this file's path on disk";
        return false;
    }

    std::error_code ec;
    fs::path backupPath = fs::path(fullPath).concat(".bak");
    fs::copy_file(fullPath, backupPath, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        outError = "couldn't create .bak";
        return false;
    }

    fs::path tmpPath = fs::path(fullPath).concat(".tmp");
    try
    {
        std::ofstream out(tmpPath, std::ios::binary);
        if (!out)
        {
            outError = "couldn't open temp file for writing";
            if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
            return false;
        }

        // dump() always emits bare '\n'; VfxDenoiser's own files are CRLF,
        // so convert here rather than silently flipping every line ending
        // to LF the moment a file gets edited and saved.
        out << ToCrlf(jsonIt->second.dump(1, '\t'));
        if (!out)
        {
            outError = "write to temp file failed (disk full?)";
            if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
            return false;
        }

        out.close();
        if (!out)
        {
            outError = "temp file didn't flush to disk cleanly (disk full?)";
            if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
            return false;
        }
    }
    catch (...)
    {
        outError = "couldn't write temp file";
        if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
        return false;
    }

    fs::rename(tmpPath, fullPath, ec);
    if (ec)
    {
        outError = "couldn't rename into place";
        return false;
    }

    return true;
}

// Populates the edit buffers from `effect`'s current values and marks
// editing active. `path` is the category's identity (root -> immediate
// parent, each element an index into its parent's "categories" array) --
// used to re-find it on Save. Category placement itself is edited only
// via drag-and-drop on the tree (see EffectDragPayload) -- there used to
// be a retypeable "Category (slash-separated)" text field here too, but
// it's removed: typing a brand-new path into it and saving created the
// category correctly but also duplicated the effect into it, and it was
// a fully redundant second way to do what drag-and-drop already does
// correctly, so it wasn't worth debugging -- just cut.
void BeginEdit(const std::string& sinName, const std::vector<int>& path,
               int index, const nlohmann::ordered_json& effect)
{
    s_edit.active        = true;
    s_edit.sinName       = sinName;
    s_edit.originalPath  = path;
    s_edit.originalName  = effect.value("name", std::string());
    s_edit.originalIndex = index;

    std::snprintf(s_edit.nameBuf, sizeof(s_edit.nameBuf), "%s", s_edit.originalName.c_str());

    std::string desc = effect.value("description", std::string());
    std::snprintf(s_edit.descBuf, sizeof(s_edit.descBuf), "%s", desc.c_str());

    std::string guidsJoined;
    if (effect.contains("guids") && effect["guids"].is_array())
    {
        for (const auto& g : effect["guids"])
        {
            if (!g.is_string())
                continue;
            if (!guidsJoined.empty())
                guidsJoined += "\n";
            guidsJoined += g.get<std::string>();
        }
    }
    std::snprintf(s_edit.guidsBuf, sizeof(s_edit.guidsBuf), "%s", guidsJoined.c_str());

    s_editResultMessage.clear();
}

void CancelEdit()
{
    s_edit.active = false;
    s_editResultMessage.clear();
}

// Draws the editable widgets for whichever effect is currently being
// edited. Only called from inside that one effect's TreeNode. Saving here
// never touches the json directly -- it just records `s_pendingSave` and
// sets `s_hasPendingSave`, so the actual mutation happens once, safely,
// after the whole tree has finished walking for this frame (see
// ApplyPendingEdit).
void RenderEffectEditor()
{
    // Fixed width for all edit fields -- without this, ImGui::InputText
    // stretches to fill the whole options-panel width, which reads as
    // way too wide for short values like a name or a guid.
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("Editing this addon's own fields. Hide/Show/duration stay owned by VfxDenoiser.");

    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name", s_edit.nameBuf, sizeof(s_edit.nameBuf));
    ImGui::InputTextMultiline("Description", s_edit.descBuf, sizeof(s_edit.descBuf), ImVec2(kFieldWidth, 60));
    ImGui::TextDisabled("GUIDs (one per line):");
    ImGui::InputTextMultiline("##guids", s_edit.guidsBuf, sizeof(s_edit.guidsBuf), ImVec2(kFieldWidth, 80));

    if (ImGui::Button("Save"))
    {
        std::string trimmedName = s_edit.nameBuf;
        size_t start = trimmedName.find_first_not_of(" \t\r\n");
        size_t end   = trimmedName.find_last_not_of(" \t\r\n");
        trimmedName  = (start == std::string::npos) ? std::string() : trimmedName.substr(start, end - start + 1);

        if (trimmedName.empty())
        {
            s_editResultMessage = "Edit not saved: name can't be empty.";
        }
        else
        {
            EditSaveJob job;
            job.sinName         = s_edit.sinName;
            job.originalPath    = s_edit.originalPath;
            job.originalName    = s_edit.originalName;
            job.originalIndex   = s_edit.originalIndex;
            job.newName         = trimmedName;
            job.newDescription  = s_edit.descBuf;
            job.newGuids        = SplitLines(s_edit.guidsBuf);

            s_pendingSave    = std::move(job);
            s_hasPendingSave = true;
            s_edit.active    = false; // the node this refers to may move/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        CancelEdit();
}

// Applies a previously-recorded edit (see RenderEffectEditor) to the
// in-memory json and writes it to disk. Deliberately re-finds the source
// category and effect by path/name right before mutating, rather than
// carrying a pointer from render time -- same reasoning as
// ApplyMergePlan's re-derived index in merge.cpp: nothing else is supposed
// to mutate s_installedJson between "Save was clicked" and "this runs" (both
// happen within the same frame), but re-deriving costs nothing and removes
// any doubt.
void ApplyPendingEdit()
{
    if (!s_hasPendingSave)
        return;
    s_hasPendingSave = false;

    const EditSaveJob& job = s_pendingSave;

    auto fileIt = s_installedJson.find(job.sinName);
    if (fileIt == s_installedJson.end())
    {
        s_editResultMessage = "Edit failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = fileIt->second;

    nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.originalPath);
    if (!srcCategory || !srcCategory->contains("effects") || !(*srcCategory)["effects"].is_array())
    {
        s_editResultMessage = "Edit failed: the original category is no longer there.";
        return;
    }

    auto& effectsArr = (*srcCategory)["effects"];
    // Indexed lookup, not a name search: two sibling effects can share a
    // name (see HANDOFF), and a name search would silently grab whichever
    // one happens to come first, editing the wrong effect. originalIndex
    // is this effect's position at BeginEdit time, which can't have
    // shifted since -- nothing else is allowed to mutate this array while
    // an edit is in flight (see this function's own header comment). The
    // name check is just a defensive sanity check, not the identity.
    if (job.originalIndex < 0 || static_cast<size_t>(job.originalIndex) >= effectsArr.size())
    {
        s_editResultMessage = "Edit failed: effect \"" + job.originalName + "\" is no longer there.";
        return;
    }
    auto it = effectsArr.begin() + job.originalIndex;
    if (!it->contains("name") || (*it)["name"] != job.originalName)
    {
        s_editResultMessage = "Edit failed: effect \"" + job.originalName + "\" is no longer there.";
        return;
    }

    // Apply the edited fields. Note "guids" is always written, even if
    // empty -- an edit can legitimately clear all guids (though see
    // merge.h: a guid-less effect then can't be tracked across a future
    // update and will simply be skipped, same as any other guid-less
    // effect). Category placement is never touched here -- that's
    // drag-and-drop's job exclusively now (see BeginEdit's comment) -- so
    // this always mutates the existing array element in place, same as
    // category rename does for "name." No erase/re-insert, so no chance
    // of reordering the effect within its category.
    nlohmann::ordered_json guidsArr = nlohmann::ordered_json::array();
    for (const auto& g : job.newGuids)
        guidsArr.push_back(g);

    (*it)["name"] = job.newName;
    if (job.newDescription.empty())
        it->erase("description");
    else
        (*it)["description"] = job.newDescription;
    (*it)["guids"] = std::move(guidsArr);

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Edit applied in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        s_installedTreeLoaded = false; // force a clean reload; don't trust the in-memory copy after a failed write
        return;
    }

    s_editResultMessage  = "Saved changes to \"" + job.newName + "\".";
    s_installedTreeLoaded = false; // force a clean reload from disk next expand, same as after an applied update
}

// Populates the category-rename buffer and marks it active. `path` is
// this category's own path (root -> ... -> this category, inclusive).
void BeginCategoryEdit(const std::string& sinName, const std::vector<int>& path, const std::string& currentName)
{
    s_categoryEdit.active  = true;
    s_categoryEdit.sinName = sinName;
    s_categoryEdit.path    = path;
    std::snprintf(s_categoryEdit.nameBuf, sizeof(s_categoryEdit.nameBuf), "%s", currentName.c_str());
    s_editResultMessage.clear();
}

void CancelCategoryEdit()
{
    s_categoryEdit.active = false;
    s_editResultMessage.clear();
}

// Draws the rename widget for whichever category is currently being
// renamed. Only called from inside that one category's TreeNode. As with
// the effect editor, Save only records `s_pendingCategoryRename` --
// the actual json mutation happens in ApplyPendingCategoryRename, after
// the whole tree has finished rendering for this frame.
void RenderCategoryEditor()
{
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("Renaming this category.");
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name##category", s_categoryEdit.nameBuf, sizeof(s_categoryEdit.nameBuf));

    if (ImGui::Button("Save##category"))
    {
        std::string trimmed = s_categoryEdit.nameBuf;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        size_t end   = trimmed.find_last_not_of(" \t\r\n");
        trimmed = (start == std::string::npos) ? std::string() : trimmed.substr(start, end - start + 1);

        if (trimmed.empty())
        {
            s_editResultMessage = "Rename not saved: category name can't be empty.";
        }
        else
        {
            CategoryRenameJob job;
            job.sinName = s_categoryEdit.sinName;
            job.path    = s_categoryEdit.path;
            job.newName = trimmed;

            s_pendingCategoryRename    = std::move(job);
            s_hasPendingCategoryRename = true;
            s_categoryEdit.active      = false; // this node's path may change/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##category"))
        CancelCategoryEdit();
}

// Applies a previously-recorded category rename to the in-memory json and
// writes it to disk. Re-finds the category by its recorded path right
// before mutating -- same reasoning as ApplyPendingEdit above.
void ApplyPendingCategoryRename()
{
    if (!s_hasPendingCategoryRename)
        return;
    s_hasPendingCategoryRename = false;

    const CategoryRenameJob& job = s_pendingCategoryRename;

    auto fileIt = s_installedJson.find(job.sinName);
    if (fileIt == s_installedJson.end())
    {
        s_editResultMessage = "Rename failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = fileIt->second;

    nlohmann::ordered_json* category = FindCategoryByPath(root, job.path);
    if (!category)
    {
        s_editResultMessage = "Rename failed: this category is no longer there.";
        return;
    }

    (*category)["name"] = job.newName;

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Rename applied in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        s_installedTreeLoaded = false;
        return;
    }

    s_editResultMessage   = "Renamed category to \"" + job.newName + "\".";
    s_installedTreeLoaded = false; // force a clean reload from disk next expand
}

// Populates the delete-confirmation state and marks it active. `path` is
// the effect's containing category path (effect delete) or the category's
// own path, inclusive (category delete); `index` is the effect's position
// within that path's "effects" array, unused for a category delete.
void BeginDeleteConfirm(const std::string& sinName, const std::vector<int>& path, int index,
                         bool isCategory, const std::string& displayName)
{
    s_deleteConfirm.active      = true;
    s_deleteConfirm.isCategory  = isCategory;
    s_deleteConfirm.sinName     = sinName;
    s_deleteConfirm.path        = path;
    s_deleteConfirm.index       = index;
    s_deleteConfirm.displayName = displayName;
    s_editResultMessage.clear();
}

void CancelDeleteConfirm()
{
    s_deleteConfirm = DeleteConfirmState();
}

// Draws the inline "Delete X? [Delete] [Cancel]" prompt for whichever
// effect/category is currently pending delete. As with every other editor
// here, this only records `s_pendingDelete` -- the actual removal happens
// in ApplyPendingDelete, after the whole tree has finished rendering for
// this frame.
void RenderDeleteConfirm()
{
    ImGui::TextColored(kDuplicateColor, "Delete \"%s\"? A .bak is kept, but there's no in-app undo for this.",
                        s_deleteConfirm.displayName.c_str());
    if (ImGui::SmallButton("Delete##confirm"))
    {
        DeleteJob job;
        job.isCategory = s_deleteConfirm.isCategory;
        job.sinName    = s_deleteConfirm.sinName;
        job.path       = s_deleteConfirm.path;
        job.index      = s_deleteConfirm.index;
        job.name       = s_deleteConfirm.displayName;

        s_pendingDelete        = std::move(job);
        s_hasPendingDelete     = true;
        s_deleteConfirm.active = false; // this node's path may change/disappear on the next reload
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel##delete"))
        CancelDeleteConfirm();
}

// Applies a previously-recorded delete to the in-memory json and writes it
// to disk. Re-finds the target right before mutating -- same reasoning as
// every other Apply* function above -- and, for a category, re-checks
// it's still empty right here too, in case anything changed between the
// confirm prompt and now (nothing can in practice, since only one
// edit/delete/create/rename is ever in flight addon-wide, but this is the
// authoritative check either way, not the confirm prompt).
void ApplyPendingDelete()
{
    if (!s_hasPendingDelete)
        return;
    s_hasPendingDelete = false;

    const DeleteJob& job = s_pendingDelete;

    auto fileIt = s_installedJson.find(job.sinName);
    if (fileIt == s_installedJson.end())
    {
        s_editResultMessage = "Delete failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = fileIt->second;

    if (job.isCategory)
    {
        if (job.path.empty())
        {
            s_editResultMessage = "Delete failed: nothing to delete.";
            return;
        }
        std::vector<int> parentPath(job.path.begin(), job.path.end() - 1);
        int myIndex = job.path.back();
        nlohmann::ordered_json* parent = parentPath.empty() ? &root : FindCategoryByPath(root, parentPath);
        if (!parent || !parent->contains("categories") || !(*parent)["categories"].is_array())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }

        auto& siblings = (*parent)["categories"];
        // Indexed lookup, not a name search -- same reasoning as the
        // effect-delete branch below: two sibling categories can share a
        // name, and a name search would silently grab whichever one comes
        // first. myIndex is this category's position at BeginDeleteConfirm
        // time, which can't have shifted since (only one edit/delete/
        // create/rename is ever in flight addon-wide). The name check is
        // just a defensive sanity check, not the identity.
        if (myIndex < 0 || static_cast<size_t>(myIndex) >= siblings.size())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        auto it = siblings.begin() + myIndex;
        if (!it->contains("name") || (*it)["name"] != job.name)
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        bool hasEffects = it->contains("effects") && !(*it)["effects"].empty();
        bool hasSubcats = it->contains("categories") && !(*it)["categories"].empty();
        if (hasEffects || hasSubcats)
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" isn't empty anymore.";
            return;
        }
        siblings.erase(it);
    }
    else
    {
        nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.path);
        if (!srcCategory || !srcCategory->contains("effects") || !(*srcCategory)["effects"].is_array())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        auto& effectsArr = (*srcCategory)["effects"];
        // Indexed lookup, not a name search -- same reasoning as
        // ApplyPendingEdit/ApplyPendingMove above.
        if (job.index < 0 || static_cast<size_t>(job.index) >= effectsArr.size())
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        auto it = effectsArr.begin() + job.index;
        if (!it->contains("name") || (*it)["name"] != job.name)
        {
            s_editResultMessage = "Delete failed: \"" + job.name + "\" is no longer there.";
            return;
        }
        effectsArr.erase(it);
    }

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Delete applied in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        s_installedTreeLoaded = false;
        return;
    }

    s_editResultMessage   = "Deleted \"" + job.name + "\".";
    s_installedTreeLoaded = false; // force a clean reload from disk next expand
}

// Populates the create-category state and marks it active. `parentPath` is
// where the new category will go -- empty means this sin file's top level.
void BeginCreateCategory(const std::string& sinName, const std::vector<int>& parentPath)
{
    s_createCategory.active      = true;
    s_createCategory.sinName     = sinName;
    s_createCategory.parentPath  = parentPath;
    s_createCategory.nameBuf[0]  = '\0';
    s_editResultMessage.clear();
}

void CancelCreateCategory()
{
    s_createCategory = CreateCategoryState();
}

// Draws the inline "new subcategory" prompt for whichever category is
// currently the target parent. Only records `s_pendingCreateCategory` --
// the actual creation happens in ApplyPendingCreateCategory, after the
// whole tree has finished rendering for this frame.
void RenderCreateCategoryEditor()
{
    const float kFieldWidth = 250.0f;

    ImGui::TextDisabled("New subcategory.");
    ImGui::SetNextItemWidth(kFieldWidth);
    ImGui::InputText("Name##newcategory", s_createCategory.nameBuf, sizeof(s_createCategory.nameBuf));

    if (ImGui::Button("Create##newcategory"))
    {
        std::string trimmed = s_createCategory.nameBuf;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        size_t end   = trimmed.find_last_not_of(" \t\r\n");
        trimmed = (start == std::string::npos) ? std::string() : trimmed.substr(start, end - start + 1);

        if (trimmed.empty())
        {
            s_editResultMessage = "Not created: category name can't be empty.";
        }
        else
        {
            CreateCategoryJob job;
            job.sinName    = s_createCategory.sinName;
            job.parentPath = s_createCategory.parentPath;
            job.newName    = trimmed;

            s_pendingCreateCategory    = std::move(job);
            s_hasPendingCreateCategory = true;
            s_createCategory.active    = false; // this node's path may change/disappear on the next reload
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##newcategory"))
        CancelCreateCategory();
}

// Applies a previously-recorded category creation to the in-memory json
// and writes it to disk. Re-finds the parent by path right before
// mutating -- same reasoning as every other Apply* function above.
// Deliberately errors out rather than silently reusing an existing
// same-named sibling: the user asked to create a specific new category,
// so finding one already there is worth surfacing, not hiding.
void ApplyPendingCreateCategory()
{
    if (!s_hasPendingCreateCategory)
        return;
    s_hasPendingCreateCategory = false;

    const CreateCategoryJob& job = s_pendingCreateCategory;

    auto fileIt = s_installedJson.find(job.sinName);
    if (fileIt == s_installedJson.end())
    {
        s_editResultMessage = "Create failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = fileIt->second;

    nlohmann::ordered_json* parent = job.parentPath.empty() ? &root : FindCategoryByPath(root, job.parentPath);
    if (!parent)
    {
        s_editResultMessage = "Create failed: the parent category is no longer there.";
        return;
    }

    if (!parent->contains("categories") || !(*parent)["categories"].is_array())
        (*parent)["categories"] = nlohmann::ordered_json::array();

    for (const auto& sub : (*parent)["categories"])
    {
        if (sub.contains("name") && sub["name"] == job.newName)
        {
            s_editResultMessage = "Create failed: \"" + job.newName + "\" already exists here.";
            return;
        }
    }

    nlohmann::ordered_json fresh;
    fresh["name"] = job.newName;
    (*parent)["categories"].push_back(std::move(fresh));

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Create applied in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        s_installedTreeLoaded = false;
        return;
    }

    s_editResultMessage   = "Created category \"" + job.newName + "\".";
    s_installedTreeLoaded = false; // force a clean reload from disk next expand
}

// Applies a previously-recorded drag-and-drop move (see EffectMoveJob) to
// the in-memory json and writes it to disk. Covers both moving an effect
// into a different category and reordering it within its current one --
// see EffectMoveJob's destinationIndex comment for how those share this one
// job type. Re-finds the source category and effect by path/name right
// before mutating -- same reasoning as ApplyPendingEdit/ApplyPendingCategoryRename
// above. Unlike ApplyPendingEdit, this moves the matched effect object
// verbatim (whatever fields it has, known or not) rather than rebuilding it
// field-by-field from an edit form -- a move never touches the effect's own
// content, only its parent and its position within that parent.
void ApplyPendingMove()
{
    if (!s_hasPendingMove)
        return;
    s_hasPendingMove = false;

    const EffectMoveJob& job = s_pendingMove;

    auto fileIt = s_installedJson.find(job.sinName);
    if (fileIt == s_installedJson.end())
    {
        s_editResultMessage = "Move failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = fileIt->second;

    nlohmann::ordered_json* srcCategory = FindCategoryByPath(root, job.originalPath);
    if (!srcCategory || !srcCategory->contains("effects") || !(*srcCategory)["effects"].is_array())
    {
        s_editResultMessage = "Move failed: the original category is no longer there.";
        return;
    }

    nlohmann::ordered_json effectCopy;
    bool found = false;
    auto& effectsArr = (*srcCategory)["effects"];
    // Indexed lookup -- see ApplyPendingEdit's comment for why a name
    // search isn't safe when sibling effects can share a name.
    if (job.originalIndex >= 0 && static_cast<size_t>(job.originalIndex) < effectsArr.size())
    {
        auto it = effectsArr.begin() + job.originalIndex;
        if (it->contains("name") && (*it)["name"] == job.effectName)
        {
            effectCopy = *it;
            effectsArr.erase(it);
            found = true;
        }
    }
    if (!found)
    {
        s_editResultMessage = "Move failed: effect \"" + job.effectName + "\" is no longer there.";
        return;
    }

    // destinationPath is the drop target category's own index-based
    // identity (captured from the tree at drop time, see
    // RenderCategoryTree's drag-drop target handler) -- it's a real,
    // already-existing category, never typed text, so this resolves it
    // the same way srcCategory was resolved above rather than creating
    // anything new.
    nlohmann::ordered_json* destCategory = FindCategoryByPath(root, job.destinationPath);
    if (!destCategory)
    {
        s_editResultMessage = "Move failed: the destination category is no longer there.";
        return;
    }
    if (!destCategory->contains("effects") || !(*destCategory)["effects"].is_array())
        (*destCategory)["effects"] = nlohmann::ordered_json::array();
    auto& destEffectsArr = (*destCategory)["effects"];

    // destinationIndex == -1 means "append" (dropped on destinationPath's
    // own row, see RenderCategoryTree's category-row drop target) -- the
    // same behavior this always had before destinationIndex existed.
    // Otherwise it's "insert immediately before this index" (dropped on a
    // specific effect's row, see the effect-row drop target), where that
    // index was captured against destEffectsArr *before* the erase above
    // ran. If the source and destination are the same category, that
    // erase already shifted every index after originalIndex down by one,
    // so a destination index that came from later in that same array
    // needs the same adjustment here or the effect lands one slot too far
    // right. A destination index from a different category, or from
    // earlier in the same array, was never touched by that erase and
    // needs no adjustment.
    if (job.destinationIndex < 0)
    {
        destEffectsArr.push_back(std::move(effectCopy));
    }
    else
    {
        int insertIndex = job.destinationIndex;
        if (job.originalPath == job.destinationPath && job.originalIndex < insertIndex)
            insertIndex -= 1;

        // Defensive clamp -- shouldn't trigger given the no-op guards at
        // drop time and everything happening within one frame, but a
        // stale index silently landing outside the array is worse than a
        // slightly-off placement.
        if (insertIndex < 0)
            insertIndex = 0;
        if (static_cast<size_t>(insertIndex) > destEffectsArr.size())
            insertIndex = static_cast<int>(destEffectsArr.size());

        destEffectsArr.insert(destEffectsArr.begin() + insertIndex, std::move(effectCopy));
    }

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Moved \"" + job.effectName + "\" in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        s_installedTreeLoaded = false; // force a clean reload; don't trust the in-memory copy after a failed write
        return;
    }

    std::string destLabel = job.destinationPath.empty() ? std::string("(top level)") : JoinCategoryPathNames(root, job.destinationPath);
    if (job.originalPath == job.destinationPath)
        s_editResultMessage = "Reordered \"" + job.effectName + "\" within \"" + destLabel + "\".";
    else
        s_editResultMessage = "Moved \"" + job.effectName + "\" to \"" + destLabel + "\".";
    s_installedTreeLoaded  = false; // force a clean reload from disk next expand, same as after an applied update
}

// Applies a previously-recorded category reorder (see CategoryMoveJob) to
// the in-memory json and writes it to disk. Reorder-only, by design (see
// this file's drag-and-drop header comment) -- the category never leaves
// its own current parent's "categories" array, so unlike ApplyPendingMove
// there's only ever one array in play here, not a separate source and
// destination. Re-finds that array, and the category's position in it, by
// path/index right before mutating -- same reasoning as
// ApplyPendingEdit/ApplyPendingMove above.
void ApplyPendingCategoryMove()
{
    if (!s_hasPendingCategoryMove)
        return;
    s_hasPendingCategoryMove = false;

    const CategoryMoveJob& job = s_pendingCategoryMove;

    auto fileIt = s_installedJson.find(job.sinName);
    if (fileIt == s_installedJson.end())
    {
        s_editResultMessage = "Reorder failed: " + job.sinName + " is no longer loaded.";
        return;
    }
    nlohmann::ordered_json& root = fileIt->second;

    if (job.originalPath.empty())
    {
        s_editResultMessage = "Reorder failed: nothing to reorder.";
        return;
    }
    std::vector<int> parentPath(job.originalPath.begin(), job.originalPath.end() - 1);
    int              originalIndex = job.originalPath.back();
    nlohmann::ordered_json* parent = parentPath.empty() ? &root : FindCategoryByPath(root, parentPath);
    if (!parent || !parent->contains("categories") || !(*parent)["categories"].is_array())
    {
        s_editResultMessage = "Reorder failed: the category's parent is no longer there.";
        return;
    }

    auto& siblings = (*parent)["categories"];
    // Indexed lookup, not a name search -- same reasoning as
    // ApplyPendingEdit/ApplyPendingDelete's category branch: two sibling
    // categories can share a name, and a name search would silently grab
    // whichever one comes first.
    if (originalIndex < 0 || static_cast<size_t>(originalIndex) >= siblings.size())
    {
        s_editResultMessage = "Reorder failed: the category is no longer there.";
        return;
    }
    auto        it   = siblings.begin() + originalIndex;
    std::string name = it->value("name", std::string());

    nlohmann::ordered_json categoryCopy = std::move(*it);
    siblings.erase(it);

    // destinationIndex == -1 means "append" (dropped on the shared
    // parent's own row); otherwise "insert immediately before this
    // index," captured against `siblings` *before* the erase just above
    // ran, so (since source and destination are always this same array
    // here) an index that came from later than originalIndex needs the
    // same -1 shift ApplyPendingMove's same-category case needed, for the
    // same reason: everything after originalIndex just moved down by one.
    if (job.destinationIndex < 0)
    {
        siblings.push_back(std::move(categoryCopy));
    }
    else
    {
        int insertIndex = job.destinationIndex;
        if (originalIndex < insertIndex)
            insertIndex -= 1;

        // Defensive clamp -- see ApplyPendingMove's identical comment for
        // why this is worth having even though the no-op guards at drop
        // time shouldn't let a bad index reach here.
        if (insertIndex < 0)
            insertIndex = 0;
        if (static_cast<size_t>(insertIndex) > siblings.size())
            insertIndex = static_cast<int>(siblings.size());

        siblings.insert(siblings.begin() + insertIndex, std::move(categoryCopy));
    }

    std::string writeError;
    if (!SaveInstalledSinFile(job.sinName, writeError))
    {
        s_editResultMessage = "Reordered \"" + name + "\" in memory but failed to write to disk (" + writeError +
                               "). Reloading from disk so nothing shown is out of sync with what's actually saved.";
        s_installedTreeLoaded = false; // force a clean reload; don't trust the in-memory copy after a failed write
        return;
    }

    s_editResultMessage   = "Reordered \"" + name + "\".";
    s_installedTreeLoaded = false; // force a clean reload from disk next expand, same as after an applied update
}

// ---------------------------------------------------------------------------
// Installed-effects tree search box (RenderInstalledEffects). A single text
// box filters every installed sin file's tree at once by name, category
// name, description, or GUID substring, case-insensitively. s_treeSearchBuf
// is the raw ImGui input buffer; s_treeSearchQueryLower is recomputed from
// it once per frame at the top of RenderInstalledEffects and is what the
// matching helpers below actually compare against, so nothing else in this
// file has to lowercase repeatedly.
static char        s_treeSearchBuf[256] = {};
static std::string s_treeSearchQueryLower;

// Search only actually starts once at least this many characters are
// typed -- a 1-2 character query matches almost everything in a typical
// tree anyway, so there's little value in it and it's needless work (both
// the matching itself and the expansion it triggers) on every keystroke
// along the way to a more useful query.
static constexpr size_t kMinTreeSearchLength = 3;

// True only on the single frame where s_treeSearchQueryLower just changed
// from what it was last frame (recomputed once, at the top of
// RenderInstalledEffects). RenderCategoryTree gates its forced-open calls
// on this rather than on "a search is active" -- forcing potentially
// hundreds of nodes open is only meant to happen once, right when the
// query changes, not on every single frame the search box merely still has
// text in it. Re-forcing it every frame was expensive enough on a large
// tree to stall the whole overlay (dropped keystrokes, unresponsive
// scrolling) while typing.
static bool s_treeSearchQueryChanged = false;

// Case-insensitive substring test. An empty `needleLower` always matches
// (an empty search box means "no filter"), so callers don't need their own
// early-out for that case.
static bool ContainsCI(const std::string& haystack, const std::string& needleLower)
{
    if (needleLower.empty())
        return true;

    std::string haystackLower = haystack;
    std::transform(haystackLower.begin(), haystackLower.end(), haystackLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystackLower.find(needleLower) != std::string::npos;
}

// True if `effect`'s own visible name -- what's shown right on its
// (possibly collapsed) row -- contains `queryLower`. A name match never
// needs the effect's own node opened: the match is already on-screen.
static bool EffectNameMatches(const nlohmann::ordered_json& effect, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;
    return ContainsCI(effect.value("name", std::string()), queryLower);
}

// True if `effect` matches `queryLower` only through content that's hidden
// until its own node is opened -- its description or any one of its GUIDs.
// Unlike a name match, this DOES need the node forced open, or the reason
// it matched never becomes visible.
static bool EffectHiddenContentMatches(const nlohmann::ordered_json& effect, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;

    if (ContainsCI(effect.value("description", std::string()), queryLower))
        return true;

    if (effect.contains("guids") && effect["guids"].is_array())
        for (const auto& g : effect["guids"])
            if (g.is_string() && ContainsCI(g.get<std::string>(), queryLower))
                return true;

    return false;
}

// True if `effect` matches `queryLower` at all -- by name or by hidden
// content. Used for the filtering decision (show this effect or skip it),
// which doesn't care which part of it matched, only whether it did.
static bool EffectMatchesSearch(const nlohmann::ordered_json& effect, const std::string& queryLower)
{
    if (queryLower.empty())
        return true;
    return EffectNameMatches(effect, queryLower) || EffectHiddenContentMatches(effect, queryLower);
}

// True if `category`'s own visible name -- shown right on its (possibly
// collapsed) row -- contains `queryLower`. Same reasoning as
// EffectNameMatches: a name match doesn't by itself need this category's
// own node opened.
static bool CategoryNameMatches(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;
    return ContainsCI(category.value("name", std::string()), queryLower);
}

// True if `category`'s own description -- only shown once this category's
// node is open -- contains `queryLower`. Unlike a name match, this DOES
// need the node forced open to be seen at all.
static bool CategoryDescriptionMatches(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;
    return ContainsCI(category.value("description", std::string()), queryLower);
}

// True if `category` (its own name/description), any effect directly inside
// it, or any nested subcategory (recursively) matches `queryLower`. This is
// the "does this subtree have anything worth showing at all" check
// RenderCategoryTree uses to decide whether to draw a category during a
// search rather than skip it outright.
static bool CategorySubtreeMatchesSearch(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return true;

    if (CategoryNameMatches(category, queryLower) || CategoryDescriptionMatches(category, queryLower))
        return true;

    if (category.contains("effects") && category["effects"].is_array())
        for (const auto& eff : category["effects"])
            if (EffectMatchesSearch(eff, queryLower))
                return true;

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            if (CategorySubtreeMatchesSearch(sub, queryLower))
                return true;

    return false;
}

// True if something *below* `category` (a direct effect, or a nested
// subcategory either by its own name/description or transitively via this
// same check) matches `queryLower`. Deliberately excludes `category`'s own
// name/description -- this is only about whether opening THIS category is
// necessary to reveal a match further down, not about whether this category
// is itself the match. That distinction is exactly what keeps "Warrior"
// itself collapsed when a search for "Warrior" only matched its own name,
// while still forcing "Classes" (Warrior's parent) open so Warrior's row
// isn't hidden.
static bool CategoryHasDescendantMatch(const nlohmann::ordered_json& category, const std::string& queryLower)
{
    if (queryLower.empty())
        return false;

    if (category.contains("effects") && category["effects"].is_array())
        for (const auto& eff : category["effects"])
            if (EffectMatchesSearch(eff, queryLower))
                return true;

    if (category.contains("categories") && category["categories"].is_array())
        for (const auto& sub : category["categories"])
            if (CategoryNameMatches(sub, queryLower) || CategoryDescriptionMatches(sub, queryLower) ||
                CategoryHasDescendantMatch(sub, queryLower))
                return true;

    return false;
}


// Recursively walks one category node (read-only by default): a TreeNode
// per category, effects listed as nested TreeNodes underneath. `category`
// and `effects`/`categories` are exactly the same JSON shape merge.cpp
// walks, so this stays a faithful mirror of what ResolveMergePlan sees.
//
// `pathSoFar` is threaded through and mutated in place (pushed before
// descending, popped after) so any effect rendered inside knows its own
// category's identity (root -> immediate parent, each element that
// level's index within its parent's "categories" array) -- this is what
// lets right-click-to-edit identify, and later re-find, a specific
// category or effect, the same way effect identity is index-based (see
// EditState::originalIndex's comment). `myIndex` is this category's own
// position within its parent's "categories" array, supplied by the
// caller (which is already iterating that array to get here) since this
// function has no other way to know it. `sinName` identifies which
// installed file this tree belongs to, since edit state is scoped
// per-sin as well as per-path.
//
// Caller must PushID a stable per-sibling key (index is fine here, since
// this tree is rebuilt wholesale on every reload rather than mutated in
// place) before calling, so sibling categories that happen to share a
// name don't collide in imgui's ID stack.
//
// `forceShow` is true once an ancestor category has already matched the
// tree search box directly (by its own name/description) -- from that
// point down, the whole subtree is shown unfiltered, the same way a folder
// search that matches a folder name shows everything inside it rather than
// filtering further. Callers outside this function never need to pass it;
// it's only ever set by RenderCategoryTree itself on the recursive call for
// its own subcategories.
void RenderCategoryTree(const std::string& sinName, const nlohmann::ordered_json& category,
                         std::vector<int>& pathSoFar, int myIndex, bool forceShow = false)
{
    std::string name = category.value("name", std::string("(unnamed category)"));
    pathSoFar.push_back(myIndex);

    bool searchActive = !s_treeSearchQueryLower.empty();

    // Skip this category (and everything under it) entirely when a search
    // is active and nothing in this subtree matches -- an unrelated branch
    // just isn't drawn, rather than shown collapsed and empty-looking.
    // Cancel any edit state scoped under here first, same reasoning as the
    // "collapsed" branch further down: nothing inside is being drawn this
    // frame, so nothing should be left running invisibly.
    if (searchActive && !forceShow && !CategorySubtreeMatchesSearch(category, s_treeSearchQueryLower))
    {
        if (s_categoryEdit.active && s_categoryEdit.sinName == sinName && PathHasPrefix(s_categoryEdit.path, pathSoFar))
            CancelCategoryEdit();
        if (s_edit.active && s_edit.sinName == sinName && PathHasPrefix(s_edit.originalPath, pathSoFar))
            CancelEdit();
        if (s_createCategory.active && s_createCategory.sinName == sinName && PathHasPrefix(s_createCategory.parentPath, pathSoFar))
            CancelCreateCategory();

        pathSoFar.pop_back();
        return;
    }

    // Whether THIS category matched directly (as opposed to only containing
    // a match further down) -- decides whether its own effects/subcategories
    // get filtered individually below, or shown in full because the category
    // itself is what the search was looking for.
    bool categoryMatchesDirectly = !searchActive || forceShow ||
        ContainsCI(name, s_treeSearchQueryLower) ||
        ContainsCI(category.value("description", std::string()), s_treeSearchQueryLower);

    // Only force this category open if leaving it collapsed would hide
    // something: either its own description (only shown once open), or a
    // match somewhere in its subtree (whose row only becomes visible once
    // THIS node is open). A category matching only by its own name does
    // NOT need forcing open -- that match is already visible right on its
    // (possibly collapsed) row. This is deliberately independent of
    // categoryMatchesDirectly/forceShow above: those control what's shown
    // once a node IS open, not whether it needs to be forced open at all.
    bool categoryNeedsForceOpen = searchActive &&
        (CategoryDescriptionMatches(category, s_treeSearchQueryLower) ||
         CategoryHasDescendantMatch(category, s_treeSearchQueryLower));

    // Only apply on the frame the search query just changed -- see
    // s_treeSearchQueryChanged's own comment for why doing this every
    // frame regardless was expensive enough to stall the overlay while
    // typing. Sets the state explicitly either way (not just when true):
    // a category that was forced open for a shorter/different query (e.g.
    // "war" matching "Warhorn" here) but no longer needs it once the query
    // narrows further (e.g. "warrior", which "Warhorn" doesn't match) must
    // be forced back shut on that same frame, or it just stays open
    // forever since nothing else would ever tell it to close. Once set
    // here, ImGui's own persisted open/closed state carries it forward on
    // later unchanged frames, same as always.
    if (s_treeSearchQueryChanged && searchActive)
        ImGui::SetNextItemOpen(categoryNeedsForceOpen, ImGuiCond_Always);

    bool isRenamingThis = s_categoryEdit.active && s_categoryEdit.sinName == sinName &&
                          s_categoryEdit.path == pathSoFar;
    bool isDeletingThisCategory = s_deleteConfirm.active && s_deleteConfirm.isCategory &&
                                  s_deleteConfirm.sinName == sinName && s_deleteConfirm.path == pathSoFar;
    bool isCreatingHere = s_createCategory.active && s_createCategory.sinName == sinName &&
                          s_createCategory.parentPath == pathSoFar;

    bool categoryHasDupe   = category.value("__vfxd_hasdupe", false);
    bool categoryHasRework = category.value("__vfxd_hasrework", false);
    bool categoryHasNew    = category.value("__vfxd_hasnew", false);
    bool categoryVirtual   = category.value("__vfxd_virtual", false);

    // A duplicate-guid problem wins the tint (red) over everything else --
    // it's a correctness issue in the installed file itself, not a
    // pending-update preview, and needs attention before an update should
    // even be trusted to know which effect is which underneath this
    // category. Failing that, a rework anywhere underneath wins over a new
    // effect (orange over green) -- a rework is the thing worth double-
    // checking, so a category with both should still stand out.
    const ImVec4* categoryTint = nullptr;
    if (categoryHasDupe)
        categoryTint = &kDuplicateColor;
    else if (categoryHasRework)
        categoryTint = &kReworkColor;
    else if (categoryHasNew)
        categoryTint = &kNewColor;

    if (categoryTint)
        ImGui::PushStyleColor(ImGuiCol_Text, *categoryTint);
    bool categoryOpen = ImGui::TreeNode(name.c_str());
    if (categoryTint)
        ImGui::PopStyleColor();

    // Drop target for an effect dragged from elsewhere in this same sin
    // file (see EffectDragPayload/BeginDragDropSource below) -- attaches to
    // the TreeNode row itself, so a category accepts a drop whether it's
    // open or collapsed. Not offered on a pending-update overlay category
    // ("__vfxd_virtual") for the same reason Rename isn't: it isn't in the
    // real on-disk file yet, so there's nothing to re-find and move into
    // once the drop is applied.
    if (!categoryVirtual && ImGui::BeginDragDropTarget())
    {
        if (ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
        {
            // Payload bytes are just a type marker (see EffectDragPayload's
            // comment) -- the real source info lives in s_dragPayload,
            // which BeginDragDropSource keeps current every frame the drag
            // is held. Since only same-sin moves are offered, guard
            // against a payload somehow tagged with a different sin
            // (shouldn't happen -- BeginDragDropSource below only starts a
            // drag using this category tree's own sinName -- but cheap to
            // check rather than assume).
            //
            // Unlike before this category also doubled as a reorder-to-
            // end target, a drop here from the *same* category is now
            // allowed -- it's how an effect reaches the last position in
            // its own list (see this file's drag-and-drop header comment)
            // -- except when the dragged effect is already last, which
            // would erase-then-reinsert it in the exact same spot: a
            // no-op that would still trigger a pointless rewrite+.bak.
            bool sameCategory = s_dragPayload.originalPath == pathSoFar;
            bool alreadyLast  = sameCategory && category.contains("effects") && category["effects"].is_array() &&
                                s_dragPayload.originalIndex == static_cast<int>(category["effects"].size()) - 1;
            if (s_dragPayload.sinName == sinName && !alreadyLast)
            {
                EffectMoveJob job;
                job.sinName          = s_dragPayload.sinName;
                job.originalPath     = s_dragPayload.originalPath;
                job.effectName       = s_dragPayload.effectName;
                job.originalIndex    = s_dragPayload.originalIndex;
                job.destinationPath  = pathSoFar;
                job.destinationIndex = -1; // append -- see EffectMoveJob's comment

                s_pendingMove    = std::move(job);
                s_hasPendingMove = true;
            }
        }

        if (ImGui::AcceptDragDropPayload("VFXD_CATEGORY"))
        {
            // A dragged category can land in one of two places relative to
            // THIS row, and nowhere else -- reorder-only, see this file's
            // drag-and-drop header comment. If this category IS the
            // dragged one's current parent, dropping here means "append to
            // the end of my own children" (mirrors the effect target just
            // above). If this category instead shares that same parent
            // (i.e. it's a current sibling of the dragged one), dropping
            // here means "insert immediately above me." Anything else --
            // a category dropped onto an unrelated one under a different
            // parent entirely -- would be reparenting, which isn't offered
            // yet, so it's silently ignored rather than guessed at.
            if (s_categoryDragPayload.sinName == sinName && !s_categoryDragPayload.path.empty())
            {
                std::vector<int> draggedParentPath(s_categoryDragPayload.path.begin(), s_categoryDragPayload.path.end() - 1);
                int              draggedIndex = s_categoryDragPayload.path.back();

                std::vector<int> myParentPath = pathSoFar;
                myParentPath.pop_back();

                if (pathSoFar == draggedParentPath)
                {
                    // Append case -- but only a real move if the dragged
                    // category isn't already the last child here, same
                    // no-op reasoning as the effect target's alreadyLast.
                    bool alreadyLast = category.contains("categories") && category["categories"].is_array() &&
                                       draggedIndex == static_cast<int>(category["categories"].size()) - 1;
                    if (!alreadyLast)
                    {
                        CategoryMoveJob job;
                        job.sinName          = s_categoryDragPayload.sinName;
                        job.originalPath     = s_categoryDragPayload.path;
                        job.destinationIndex = -1;

                        s_pendingCategoryMove    = std::move(job);
                        s_hasPendingCategoryMove = true;
                    }
                }
                else if (myParentPath == draggedParentPath)
                {
                    // Sibling case -- insert immediately above this
                    // category, skipping the same two no-op positions the
                    // effect-row target skips: dropped on itself
                    // (draggedIndex == myIndex), or on the sibling right
                    // after it, which -- once the erase-shift is
                    // accounted for in ApplyPendingCategoryMove -- would
                    // land the dragged category right back where it
                    // started.
                    bool noOp = (draggedIndex == myIndex) || (draggedIndex == myIndex - 1);
                    if (!noOp)
                    {
                        CategoryMoveJob job;
                        job.sinName          = s_categoryDragPayload.sinName;
                        job.originalPath     = s_categoryDragPayload.path;
                        job.destinationIndex = myIndex;

                        s_pendingCategoryMove    = std::move(job);
                        s_hasPendingCategoryMove = true;
                    }
                }
                // else: a different parent entirely -- would be
                // reparenting, not offered yet, so left alone.
            }
        }

        ImGui::EndDragDropTarget();
    }

    // Drag source -- any real category (not one only existing in a
    // pending-update overlay, see "__vfxd_virtual" above) can be picked up
    // and dropped to reorder it among its own current siblings (see this
    // file's drag-and-drop header comment for why reparenting into a
    // different parent isn't offered). Gated on the same "no other edit in
    // flight" rule as the effect drag source below, so a drag can't start
    // while an edit/rename elsewhere is mid-flight.
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginDragDropSource())
    {
        s_categoryDragPayload.sinName = sinName;
        s_categoryDragPayload.path    = pathSoFar;
        ImGui::SetDragDropPayload("VFXD_CATEGORY", &kCategoryDragMarker, sizeof(kCategoryDragMarker));
        ImGui::Text("Move \"%s\"", name.c_str());
        ImGui::EndDragDropSource();
    }

    // Only offer to start a rename (or an effect edit, see below) when no
    // other edit of either kind is already in flight anywhere, and not on
    // a category that only exists in a pending-update overlay -- see
    // BuildDiffOverlayTree's "__vfxd_virtual" comment.
    if (!categoryVirtual && !AnyEditInFlight() && ImGui::BeginPopupContextItem("category_ctx"))
    {
        if (ImGui::MenuItem("Rename"))
            BeginCategoryEdit(sinName, pathSoFar, name);
        ImGui::EndPopup();
    }

    // "-" delete / "+" add-subcategory, rendered after the context menu
    // above so they don't steal "last item" from the TreeNode (which
    // BeginDragDropTarget/BeginPopupContextItem both attach to). Not
    // offered on a pending-update overlay category, same reasoning as
    // Rename/the drop target above. Delete stays grayed out for as long
    // as this category has any subcategory or effect inside it -- never
    // silently deletes content. "+" only appears once the category is
    // unfolded, since it's about adding something *inside* what you're
    // currently looking at.
    if (!categoryVirtual)
    {
        bool categoryEmpty = (!category.contains("effects") || category["effects"].empty()) &&
                              (!category.contains("categories") || category["categories"].empty());
        bool deleteDisabled = !categoryEmpty || (AnyEditInFlight() && !isDeletingThisCategory);

        ImGui::SameLine();
        if (deleteDisabled)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        bool deleteClicked = ImGui::SmallButton("-##delcat");
        if (deleteDisabled)
            ImGui::PopStyleVar();
        if (deleteClicked && !deleteDisabled)
            BeginDeleteConfirm(sinName, pathSoFar, -1, /*isCategory=*/true, name);

        if (categoryOpen)
        {
            bool createDisabled = AnyEditInFlight() && !isCreatingHere;
            ImGui::SameLine();
            if (createDisabled)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            bool createClicked = ImGui::SmallButton("+##addcat");
            if (createDisabled)
                ImGui::PopStyleVar();
            if (createClicked && !createDisabled)
                BeginCreateCategory(sinName, pathSoFar);
        }
    }

    // The confirm prompt sits right below this row, not nested inside the
    // TreeNode's collapsible content -- the "-" button above is visible
    // whether this node is open or collapsed, so the prompt it opens stays
    // visible either way too, unlike isRenamingThis/isCreatingHere below.
    if (isDeletingThisCategory)
        RenderDeleteConfirm();

    if (categoryOpen)
    {
        if (isRenamingThis)
            RenderCategoryEditor();
        if (isCreatingHere)
            RenderCreateCategoryEditor();

        if (category.contains("description") && category["description"].is_string())
        {
            std::string desc = category["description"].get<std::string>();
            if (!desc.empty())
                ImGui::TextWrapped("%s", desc.c_str());
        }

        if (category.contains("effects") && category["effects"].is_array())
        {
            int i = 0;
            for (const auto& effect : category["effects"])
            {
                const int effIndex = i++;

                // Hidden by the tree search box: this category's own name/
                // description didn't match, and neither does this effect
                // (name/description/guids). Cancel any edit in flight on it
                // first -- same "nothing invisible stays running" reasoning
                // as the subtree-skip above -- since it won't be drawn this
                // frame at all, not even collapsed.
                if (searchActive && !categoryMatchesDirectly && !EffectMatchesSearch(effect, s_treeSearchQueryLower))
                {
                    bool isEditingThisHidden = s_edit.active && s_edit.sinName == sinName &&
                                               s_edit.originalPath == pathSoFar && s_edit.originalIndex == effIndex;
                    if (isEditingThisHidden)
                        CancelEdit();
                    continue;
                }

                ImGui::PushID(effIndex);

                std::string effName = effect.value("name", std::string("(unnamed effect)"));
                // Identity is (sinName, category path, index) -- NOT name.
                // Sibling effects can legitimately share a name (VfxDenoiser
                // itself doesn't require uniqueness), and matching by name
                // here would make every same-named sibling in this category
                // think it was the one being edited: all of them would pop
                // open showing the same editor content, and closing any one
                // of them (since only the actually-open one has real state)
                // would look like it cancelled the edit that was just
                // started on a different sibling.
                bool isEditingThis = s_edit.active && s_edit.sinName == sinName &&
                                     s_edit.originalPath == pathSoFar && s_edit.originalIndex == effIndex;
                bool isDeletingThisEffect = s_deleteConfirm.active && !s_deleteConfirm.isCategory &&
                                            s_deleteConfirm.sinName == sinName &&
                                            s_deleteConfirm.path == pathSoFar && s_deleteConfirm.index == effIndex;

                bool effIsDupe   = effect.value("__vfxd_dupe_guid", false);
                bool effIsNew    = effect.value("__vfxd_new", false);
                bool effIsRework = effect.value("__vfxd_rework", false);

                if (effIsDupe)
                    ImGui::PushStyleColor(ImGuiCol_Text, kDuplicateColor);
                else if (effIsNew)
                    ImGui::PushStyleColor(ImGuiCol_Text, kNewColor);
                else if (effIsRework)
                    ImGui::PushStyleColor(ImGuiCol_Text, kReworkColor);

                // An effect's own node is only forced open if it matched
                // through content that's actually hidden until then -- its
                // description or a GUID. A name match alone doesn't need
                // it: that match is already visible right on this row.
                bool effectNeedsForceOpen = searchActive && EffectHiddenContentMatches(effect, s_treeSearchQueryLower);
                // Set explicitly either way, not just when true -- see the
                // category force-open comment above for why a node that no
                // longer needs opening has to be forced shut too.
                if (s_treeSearchQueryChanged && searchActive)
                    ImGui::SetNextItemOpen(effectNeedsForceOpen, ImGuiCond_Always);
                bool nodeOpen = ImGui::TreeNode("effect", "%s%s", effName.c_str(), isEditingThis ? " (editing)" : "");

                if (effIsDupe || effIsNew || effIsRework)
                    ImGui::PopStyleColor();

                // Drop target for an effect dragged onto this effect's own
                // row -- places the dragged effect immediately above this
                // one (see EffectMoveJob's destinationIndex comment).
                // Complements the category-row target above: together they
                // reach every position in the list, so this one never
                // needs to distinguish "above" from "below" within the
                // row. Not offered on an effect that only exists in a
                // pending-update overlay ("__vfxd_new") -- same reasoning
                // as skipping it as a drag source below: it isn't in the
                // real file yet, so there's no real position to insert
                // before.
                if (!effIsNew && ImGui::BeginDragDropTarget())
                {
                    if (ImGui::AcceptDragDropPayload("VFXD_EFFECT"))
                    {
                        // Same same-sin guard as the category-row target,
                        // plus two no-op cases specific to landing on a
                        // specific effect rather than appending: dropping
                        // an effect onto itself, and dropping it onto the
                        // effect immediately after it in the same
                        // category. The first is obvious. The second is
                        // subtler -- once the dragged effect is erased
                        // from originalIndex, the effect that used to sit
                        // at originalIndex + 1 shifts down to originalIndex,
                        // so "insert before it" would put the dragged
                        // effect right back where it started (see
                        // ApplyPendingMove's index adjustment). Both are
                        // skipped here rather than left for ApplyPendingMove
                        // to silently no-op, so neither triggers a
                        // pointless rewrite+.bak.
                        bool sameCategory = s_dragPayload.originalPath == pathSoFar;
                        bool noOp = sameCategory && (s_dragPayload.originalIndex == effIndex ||
                                                      s_dragPayload.originalIndex == effIndex - 1);
                        if (s_dragPayload.sinName == sinName && !noOp)
                        {
                            EffectMoveJob job;
                            job.sinName          = s_dragPayload.sinName;
                            job.originalPath     = s_dragPayload.originalPath;
                            job.effectName       = s_dragPayload.effectName;
                            job.originalIndex    = s_dragPayload.originalIndex;
                            job.destinationPath  = pathSoFar;
                            job.destinationIndex = effIndex;

                            s_pendingMove    = std::move(job);
                            s_hasPendingMove = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                // Drag source -- any real effect (not one only existing in
                // a pending-update overlay, see "__vfxd_new" below) can be
                // picked up and dropped onto a different category's
                // TreeNode row to move it there. Gated on the same "no
                // other edit in flight" rule as the context-menu Edit just
                // below, so a drag can't be started while an edit/rename
                // elsewhere is mid-flight (see EffectMoveJob's comment for
                // why moves are otherwise independent of that machinery).
                if (!effIsNew && !AnyEditInFlight() && ImGui::BeginDragDropSource())
                {
                    s_dragPayload.sinName       = sinName;
                    s_dragPayload.originalPath  = pathSoFar;
                    s_dragPayload.effectName    = effName;
                    s_dragPayload.originalIndex = effIndex;
                    ImGui::SetDragDropPayload("VFXD_EFFECT", &kEffectDragMarker, sizeof(kEffectDragMarker));
                    ImGui::Text("Move \"%s\"", effName.c_str());
                    ImGui::EndDragDropSource();
                }

                // Only offer to start a new edit when none is already in
                // flight anywhere -- see EditState's comment for why --
                // and never on an effect that only exists in a pending-
                // update overlay: it isn't in the real file yet (that's
                // what applying the update would do), so there's nothing
                // to re-find and edit until then.
                if (!effIsNew && !AnyEditInFlight() && ImGui::BeginPopupContextItem("effect_ctx"))
                {
                    if (ImGui::MenuItem("Edit"))
                        BeginEdit(sinName, pathSoFar, effIndex, effect);
                    ImGui::EndPopup();
                }

                // "-" delete, rendered after the context menu above for
                // the same "don't steal last item" reason as the
                // category's own -/+ buttons. Never grayed out for
                // emptiness (an effect has no "contents" to protect,
                // unlike a category) -- only temporarily disabled while
                // some other edit/delete/create/rename is in flight
                // elsewhere. Not offered on a pending-update overlay
                // effect, same reasoning as Edit just above.
                if (!effIsNew)
                {
                    bool deleteDisabled = AnyEditInFlight() && !isDeletingThisEffect;
                    ImGui::SameLine();
                    if (deleteDisabled)
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                    bool deleteClicked = ImGui::SmallButton("-##deleff");
                    if (deleteDisabled)
                        ImGui::PopStyleVar();
                    if (deleteClicked && !deleteDisabled)
                        BeginDeleteConfirm(sinName, pathSoFar, effIndex, /*isCategory=*/false, effName);
                }

                // Same reasoning as the category's own confirm prompt:
                // this sits right below the row, which is visible whether
                // or not `nodeOpen` is true, so the prompt stays visible
                // either way too.
                if (isDeletingThisEffect)
                    RenderDeleteConfirm();

                if (nodeOpen)
                {
                    if (isEditingThis)
                    {
                        RenderEffectEditor();
                    }
                    else
                    {
                        if (effIsDupe)
                            ImGui::TextColored(kDuplicateColor,
                                "One or more of this effect's GUIDs is also used by another installed effect. "
                                "This shouldn't normally happen and updates are blocked for this file until it's resolved.");
                        else if (effIsNew)
                            ImGui::TextColored(kNewColor, "New from a pending update -- not yet applied.");
                        else if (effIsRework)
                            ImGui::TextColored(kReworkColor, "GUIDs would be refreshed by a pending update -- name/category/settings stay as they are.");

                        if (effect.contains("description") && effect["description"].is_string())
                        {
                            std::string desc = effect["description"].get<std::string>();
                            if (!desc.empty())
                                ImGui::TextWrapped("%s", desc.c_str());
                        }

                        std::vector<std::string> guids;
                        if (effect.contains("guids") && effect["guids"].is_array())
                            for (const auto& g : effect["guids"])
                                if (g.is_string())
                                    guids.push_back(g.get<std::string>());

                        if (effIsRework)
                        {
                            // Show what's there now (default color) and
                            // what a pending update would change it to
                            // (kReworkColor), stacked so both are visible
                            // at once without needing to apply first.
                            std::vector<std::string> newGuids;
                            if (effect.contains("__vfxd_new_guids") && effect["__vfxd_new_guids"].is_array())
                                for (const auto& g : effect["__vfxd_new_guids"])
                                    if (g.is_string())
                                        newGuids.push_back(g.get<std::string>());

                            RenderGuidList("Current GUIDs", guids);
                            RenderGuidList("GUIDs after update", newGuids, &kReworkColor);
                        }
                        else
                        {
                            RenderGuidList("guids", guids);
                        }

                        if (effect.contains("behaviors") && effect["behaviors"].is_array())
                        {
                            ImGui::TextDisabled("Behaviors (owned by VfxDenoiser):");
                            for (const auto& behavior : effect["behaviors"])
                                RenderBehavior(behavior);
                        }

                        // Anything beyond the confirmed schema (name/
                        // description/guids/behaviors) is unexpected --
                        // surface it generically rather than drop it.
                        for (const auto& [key, value] : effect.items())
                        {
                            if (key == "name" || key == "description" || key == "guids" || key == "behaviors"
                                || key == "__vfxd_new" || key == "__vfxd_rework" || key == "__vfxd_new_guids"
                                || key == "__vfxd_hasnew" || key == "__vfxd_hasrework"
                                || key == "__vfxd_dupe_guid" || key == "__vfxd_hasdupe")
                                continue;
                            RenderJsonValue(key, value);
                        }
                    }

                    ImGui::TreePop();
                }
                else if (isEditingThis)
                {
                    // Collapsing the node that's mid-edit cancels the edit,
                    // same reasoning as the category case above.
                    CancelEdit();
                }

                ImGui::PopID();
            }
        }

        if (category.contains("categories") && category["categories"].is_array())
        {
            int i = 0;
            for (const auto& sub : category["categories"])
            {
                ImGui::PushID(i);
                RenderCategoryTree(sinName, sub, pathSoFar, i, categoryMatchesDirectly);
                ImGui::PopID();
                ++i;
            }
        }

        ImGui::TreePop();
    }
    else
    {
        // This node is collapsed -- nothing inside it (including its own
        // rename UI, the "add subcategory" prompt, or any effect editor
        // further down) is being drawn this frame. Cancel rather than let
        // an edit keep running invisibly until the user reopens the node.
        // PathHasPrefix with equal-length paths also covers "this category
        // is the one being renamed"/"...the one a subcategory is being
        // added to", so there's no separate isRenamingThis/isCreatingHere
        // check needed. DeleteConfirmState is NOT included here on
        // purpose -- see its own comment for why: the "-" button and the
        // confirm prompt it opens sit on this row itself, which stays
        // visible whether or not the node below it is collapsed, so
        // there's nothing for collapsing to hide.
        if (s_categoryEdit.active && s_categoryEdit.sinName == sinName && PathHasPrefix(s_categoryEdit.path, pathSoFar))
            CancelCategoryEdit();
        if (s_edit.active && s_edit.sinName == sinName && PathHasPrefix(s_edit.originalPath, pathSoFar))
            CancelEdit();
        if (s_createCategory.active && s_createCategory.sinName == sinName && PathHasPrefix(s_createCategory.parentPath, pathSoFar))
            CancelCreateCategory();
    }

    pathSoFar.pop_back();
}

// Draws the "Installed Effects" section: one top-level TreeNode per
// installed sin file (Gluttony / Pride / Sloth, whichever are actually
// present), each expanding into that file's real category tree via
// RenderCategoryTree. Read-only browsing by default; right-clicking an
// effect offers "Edit" (see EditState/BeginEdit/RenderEffectEditor above).
// Independent of whether a GitHub update is available.
void RenderInstalledEffects()
{
    if (!s_installedTreeLoaded)
        LoadInstalledEffectsTree();

    if (ImGui::Button("Refresh##installed_tree"))
        LoadInstalledEffectsTree();

    ImGui::TextDisabled("Drag an effect onto a category to move it there (or to the end of its own category), "
                         "or onto another effect to place it just above that one. Categories can be dragged the "
                         "same way to reorder them among their own siblings.");

    // Tree search box -- filters every installed sin file's tree at once by
    // name, category name, description, or GUID, case-insensitively. Just
    // recomputes the lowercased query used by RenderCategoryTree's matching
    // helpers; the actual filtering/expansion happens down there.
    ImGui::InputTextWithHint("##installed_tree_search", "Search name / category / description / GUID...",
                              s_treeSearchBuf, sizeof(s_treeSearchBuf));
    if (s_treeSearchBuf[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##installed_tree_search"))
            s_treeSearchBuf[0] = '\0';
    }
    std::string typedLower = s_treeSearchBuf;
    std::transform(typedLower.begin(), typedLower.end(), typedLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (!typedLower.empty() && typedLower.size() < kMinTreeSearchLength)
        ImGui::TextDisabled("Keep typing... (search starts at %zu characters)", kMinTreeSearchLength);

    // Below the minimum length, treat the query as empty -- no filtering,
    // no forced expansion, same as an empty search box.
    std::string newQueryLower = (typedLower.size() >= kMinTreeSearchLength) ? typedLower : std::string();

    // Only true on this one frame if the query is different from what it
    // was last frame -- see s_treeSearchQueryChanged's own comment for why
    // RenderCategoryTree cares about this distinction rather than just
    // "search box has text in it".
    s_treeSearchQueryChanged = (newQueryLower != s_treeSearchQueryLower);
    s_treeSearchQueryLower   = std::move(newQueryLower);

    if (!s_editResultMessage.empty())
        ImGui::TextWrapped("%s", s_editResultMessage.c_str());

    if (s_installedSins.empty())
    {
        ImGui::TextDisabled("No Visual Sins effect files found in VfxDenoiser's folder.");
        return;
    }

    // A Ready diff plan overlays pending-update coloring onto this same
    // tree via BuildDiffOverlayTree, rather than a separate list -- see
    // RenderSinDiffStatus in the top action row. Sins with no plan yet (or
    // an empty one) just render the plain on-disk tree, same as always.
    std::vector<SinDiffInfo> diffs = GetSinDiffInfo();
    bool anyOverlayShown = false;

    for (const auto& sin : s_installedSins)
    {
        ImGui::PushID(sin.sinName.c_str());

        auto it = s_installedJson.find(sin.sinName);
        if (it == s_installedJson.end())
        {
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s: couldn't read %s", sin.sinName.c_str(), sin.fileName.c_str());
            ImGui::PopID();
            continue;
        }

        const SinDiffInfo* diff = nullptr;
        for (const auto& d : diffs)
            if (d.sinName == sin.sinName)
                diff = &d;

        bool hasOverlay = diff && diff->status == EDiffStatus::Ready && !diff->plan.IsEmpty();

        auto dupIt    = s_duplicateGuidsBySin.find(sin.sinName);
        bool hasDupes = dupIt != s_duplicateGuidsBySin.end() && !dupIt->second.empty();

        // Build up whichever overlays apply, in order: duplicate-guid
        // tagging first (a property of the file itself), then the
        // pending-update diff on top of that same copy -- both markers can
        // coexist on one node (e.g. a duplicated effect that also has a
        // pending rework), and RenderCategoryTree picks duplicate-red over
        // rework-orange/new-green when both are present. Only ever a copy;
        // s_installedJson itself is never touched by either pass.
        //
        // Cached in s_overlayCache rather than rebuilt every frame -- see
        // that struct's own comment for why (this used to be the direct
        // cause of a reported scrolling bug).
        const nlohmann::ordered_json* fileToRender = &it->second;

        if (hasDupes || hasOverlay)
        {
            EDiffStatus statusForCache = diff ? diff->status : EDiffStatus::NotLoaded;
            OverlayCacheEntry& cached = s_overlayCache[sin.sinName];
            bool stale = cached.generation != s_installedTreeGeneration || cached.diffStatus != statusForCache;

            if (stale)
            {
                nlohmann::ordered_json built = it->second;
                if (hasDupes)
                    built = BuildDuplicateOverlayTree(built, dupIt->second);
                if (hasOverlay)
                    built = BuildDiffOverlayTree(built, diff->plan);

                cached.generation = s_installedTreeGeneration;
                cached.diffStatus = statusForCache;
                cached.file       = std::move(built);
            }

            fileToRender = &cached.file;
            if (hasOverlay)
                anyOverlayShown = true;
        }

        if (hasDupes)
        {
            ImGui::TextColored(kDuplicateColor,
                "%s: duplicate GUID(s) detected in this file -- updates are blocked until this is resolved (see red entries below).",
                sin.sinName.c_str());
        }

        // While a tree search is active, check up front whether this file
        // has any match at all -- lets the root row force itself open (so a
        // match isn't hidden behind an unexpanded file) and, further down,
        // lets an empty result say so rather than claim there are no
        // categories in a file that actually has plenty.
        bool searchActive   = !s_treeSearchQueryLower.empty();
        bool anyMatchInFile = false;
        if (searchActive && fileToRender->contains("categories") && (*fileToRender)["categories"].is_array())
            for (const auto& cat : (*fileToRender)["categories"])
                if (CategorySubtreeMatchesSearch(cat, s_treeSearchQueryLower))
                {
                    anyMatchInFile = true;
                    break;
                }

        // Set explicitly either way on a query change, not just when true
        // -- a file that matched a shorter/different query but no longer
        // has anything under a narrower one needs to be forced back shut,
        // same reasoning as the category/effect force-open comments.
        if (s_treeSearchQueryChanged && searchActive)
            ImGui::SetNextItemOpen(anyMatchInFile, ImGuiCond_Always);

        if (ImGui::TreeNode("root", "%s (%s)", sin.sinName.c_str(), sin.fileName.c_str()))
        {
            std::vector<int> path; // this sin file's top level -- empty path, same convention as CreateCategoryState::parentPath

            // Drop target for a top-level category being reordered (see
            // RenderCategoryTree's category-row target for the nested
            // case) -- this root row is the "shared parent's own row" a
            // top-level category doesn't otherwise have anything to drop
            // onto, since there's no category node above it. Reorder-only,
            // same as RenderCategoryTree's target: only offered when the
            // dragged category is already top-level *in this same sin
            // file* -- a top-level category from a different sin file, or
            // a nested category from anywhere, would both be reparenting,
            // not offered yet (see this file's drag-and-drop header
            // comment).
            if (ImGui::BeginDragDropTarget())
            {
                if (ImGui::AcceptDragDropPayload("VFXD_CATEGORY"))
                {
                    if (s_categoryDragPayload.sinName == sin.sinName && s_categoryDragPayload.path.size() == 1)
                    {
                        int  draggedIndex = s_categoryDragPayload.path.back();
                        bool alreadyLast  = fileToRender->contains("categories") && (*fileToRender)["categories"].is_array() &&
                                            draggedIndex == static_cast<int>((*fileToRender)["categories"].size()) - 1;
                        if (!alreadyLast)
                        {
                            CategoryMoveJob job;
                            job.sinName          = s_categoryDragPayload.sinName;
                            job.originalPath     = s_categoryDragPayload.path;
                            job.destinationIndex = -1;

                            s_pendingCategoryMove    = std::move(job);
                            s_hasPendingCategoryMove = true;
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool isCreatingAtTopLevel = s_createCategory.active && s_createCategory.sinName == sin.sinName &&
                                        s_createCategory.parentPath.empty();

            bool createDisabled = AnyEditInFlight() && !isCreatingAtTopLevel;
            ImGui::SameLine();
            if (createDisabled)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            bool createClicked = ImGui::SmallButton("+##addcat_top");
            if (createDisabled)
                ImGui::PopStyleVar();
            if (createClicked && !createDisabled)
                BeginCreateCategory(sin.sinName, path);

            if (isCreatingAtTopLevel)
                RenderCreateCategoryEditor();

            const nlohmann::ordered_json& file = *fileToRender;
            if (file.contains("categories") && file["categories"].is_array())
            {
                if (searchActive && !anyMatchInFile)
                {
                    ImGui::TextDisabled("(no matches in this file)");
                }
                else
                {
                    int i = 0;
                    for (const auto& cat : file["categories"])
                    {
                        ImGui::PushID(i);
                        RenderCategoryTree(sin.sinName, cat, path, i);
                        ImGui::PopID();
                        ++i;
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("(no categories in this file)");
            }
            ImGui::TreePop();
        }
        else if (s_createCategory.active && s_createCategory.sinName == sin.sinName && s_createCategory.parentPath.empty())
        {
            // Same "collapsing cancels" reasoning as RenderCategoryTree's
            // own collapsed branch -- the "+" button and prompt above only
            // exist inside this TreeNode, so collapsing it hides them.
            CancelCreateCategory();
        }

        ImGui::PopID();
    }

    if (anyOverlayShown)
    {
        ImGui::Spacing();
        ImGui::TextColored(kNewColor,    "* New effect from a pending update, not yet applied");
        ImGui::TextColored(kReworkColor, "* GUIDs would be refreshed under this name");
    }
    if (!s_duplicateGuidsBySin.empty())
    {
        bool anyDupes = false;
        for (const auto& [name, guids] : s_duplicateGuidsBySin)
            if (!guids.empty()) { anyDupes = true; break; }
        if (anyDupes)
        {
            ImGui::Spacing();
            ImGui::TextColored(kDuplicateColor, "* Duplicate GUID shared with another installed effect -- resolve before updating");
        }
    }

    // Apply any edit that was saved during this frame's tree walk above --
    // deferred to here, after every category/effect array has finished
    // being iterated, so nothing is ever mutated mid-walk.
    if (s_hasPendingSave)
        ApplyPendingEdit();
    if (s_hasPendingCategoryRename)
        ApplyPendingCategoryRename();
    if (s_hasPendingMove)
        ApplyPendingMove();
    if (s_hasPendingCategoryMove)
        ApplyPendingCategoryMove();
    if (s_hasPendingDelete)
        ApplyPendingDelete();
    if (s_hasPendingCreateCategory)
        ApplyPendingCreateCategory();
}

// Shows the result of a per-sin diff load (StartLoadDiff) right under the
// top row's own button for that sin. This used to be a separate block
// (RenderSinDiff) shown in its own section below "Check now" at the very
// bottom of the panel -- which is why clicking "Update available" up here
// used to make the changes appear scrolled far away, under a button that
// had nothing to do with the click. That bottom section is gone now (see
// OptionsRenderCallback); the top row's button already doubles as the
// load-then-apply control (see RenderSinActionRow), so this only ever
// needs to print status/result text, never a second Apply/Retry button --
// retrying just means clicking the button above again, which already
// re-fires StartLoadDiff for NotLoaded/Error/Blocked states.
static void RenderSinDiffStatus(const SinDiffInfo* diff)
{
    if (!diff || diff->status == EDiffStatus::NotLoaded)
        return; // button above already reads "Update available"; nothing more to say yet

    if (diff->status == EDiffStatus::Loading)
    {
        ImGui::TextDisabled("Downloading changes...");
        return;
    }

    if (diff->status == EDiffStatus::Error)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Couldn't load changes -- click above to retry.");
        return;
    }

    if (diff->status == EDiffStatus::Blocked)
    {
        // Set by StartLoadDiff, which deliberately never even downloaded
        // anything for this sin -- see its own comment. Same underlying
        // condition RenderInstalledEffects already shows in red on the
        // tree above; this is the same gate surfacing on the update side
        // rather than a second, independent check.
        ImGui::TextColored(kDuplicateColor,
            "Duplicate GUID (see Installed Effects tree above) -- resolve it, then click above to retry.");
        return;
    }

    // diff->status == Ready from here on.
    const MergePlan& plan = diff->plan;

    if (plan.IsEmpty())
    {
        // Version bumped upstream but nothing this addon tracks actually
        // changed (e.g. only metadata outside the merge rules changed) --
        // still safe/useful to let the user bump the stored version.
        ImGui::TextDisabled("No effect changes -- just a version bump.");
    }
    else
    {
        // Per-item detail (which effects, old/new guids, category
        // placement) is shown as coloring directly in the "Installed
        // Effects" tree below -- BuildDiffOverlayTree overlays this same
        // plan onto it -- rather than a second, separate list here.
        ImGui::TextDisabled(
            "%d new, %d refreshed -- see Installed Effects below (green = new, orange = refreshed).",
            (int)plan.inserts.size(), (int)plan.reworks.size());
    }
}

// ---------------------------------------------------------------------------
// Manual "report an effect back" form -- reworked per
// HANDOFF_LiveLogEnrichment.md's "Report redesign -- decided plan": a
// reporter identity line (Account/Character name, or anonymous), zero or
// more per-GUID blocks (each with its own Type and a snapshot of that
// GUID's self-context, if it has one), and one required free-text note.
// Validation itself lives in report.cpp's StartSendReport; this section's
// job is the form widgets, composing each GUID's display block, and
// showing whatever outcome comes back.
//
// One row here holds everything editable about a single GUID entry,
// including its self context -- which used to be an all-or-nothing
// snapshot hidden entirely when absent, but is now four independently
// editable fields (HANDOFF's "manually adjustable" follow-up), each
// pre-filled from a live-log snapshot when "report new" was clicked and
// that GUID had one, or left at its own "not set" default otherwise.
// Snapshotted once at that point either way -- not re-read live -- so
// this form doesn't change under the user's feet while they're still
// filling it out; from there the user can edit any of it by hand.
// ---------------------------------------------------------------------------
namespace {

// Race has no None/Unknown enumerator in Mumble.h (see HANDOFF's "Known
// gap"), so -1 is this form's own sentinel for "not set" -- never a real
// ERace value. Profession, unlike Race, already has a real None (0), so
// it doubles as both the enum's own "no profession" value AND this
// field's placeholder -- no separate sentinel needed there.
constexpr int kRaceUnset = -1;
constexpr Mumble::ERace kRaceValues[] = {
    Mumble::ERace::Asura, Mumble::ERace::Charr, Mumble::ERace::Human,
    Mumble::ERace::Norn,  Mumble::ERace::Sylvari,
};
constexpr Mumble::EProfession kProfessionValues[] = {
    Mumble::EProfession::Guardian,     Mumble::EProfession::Warrior,
    Mumble::EProfession::Engineer,     Mumble::EProfession::Ranger,
    Mumble::EProfession::Thief,        Mumble::EProfession::Elementalist,
    Mumble::EProfession::Mesmer,       Mumble::EProfession::Necromancer,
    Mumble::EProfession::Revenant,
};

int RaceToIndex(Mumble::ERace race)
{
    for (int i = 0; i < (int)std::size(kRaceValues); ++i)
        if (kRaceValues[i] == race)
            return i;
    return kRaceUnset; // shouldn't happen -- ERace has no values outside kRaceValues
}

} // namespace

struct ReportFormRow
{
    char guid[128] = {};
    char typeText[8] = {}; // digits (0-11) or blank/"Not set" -- see ParseReportTypeText

    // Self context -- individually editable now (HANDOFF's "manually
    // adjustable" follow-up), not gated behind a single hasSelfContext
    // bool anymore. Each field has its own "not set" default and is
    // pre-filled independently from a live-log snapshot when one
    // exists; a manually-added row (or one for a GUID that never took
    // the isSelfEvent branch) simply starts every field at its default,
    // still fully editable by hand.
    int                 mapID          = 0;           // 0 doubles as "not set" -- MapID stays a raw number either way (HANDOFF)
    int                 raceIndex      = kRaceUnset;   // index into kRaceValues, or kRaceUnset
    Mumble::EProfession profession     = Mumble::EProfession::None; // None doubles as "not set"
    char                specializationText[64] = {};  // autocomplete box's live text; resolved to an id at compose time
};

static bool                       s_reportAnonymous = false;
static char                       s_reportAccountNameBuf[128]   = {};
static char                       s_reportCharacterNameBuf[128] = {};
static std::vector<ReportFormRow> s_reportRows;
static char                       s_reportNoteBuf[1024] = {};
static std::string                s_reportFormError; // validation error shown until the next attempt, cleared on success

// Re-reads Account/Character Name from GameState and overwrites the
// report form's name buffers with whatever's live right now. Called
// only from AddReportRowFromLiveLogEntry (i.e. every "report new"
// click) -- NOT on section render/addon load, and NOT for a manually-
// added row. This is the fix for this session's ask: auto-fill (and
// re-fill on a later click, if the account/character has since changed
// -- e.g. switched characters between two "report new" clicks) is tied
// to the button, not to opening the Report tab. Unconditional overwrite
// rather than "only if still blank" -- a later click is meant to
// re-sync with whoever's actually playing right now, even if the user
// had typed something else into the boxes in the meantime.
void RefreshReportNameFieldsFromGameState()
{
    std::string acct = GameState_GetAccountName();
    std::string chr  = GameState_GetCharacterName();
    std::snprintf(s_reportAccountNameBuf, sizeof(s_reportAccountNameBuf), "%s", acct.c_str());
    std::snprintf(s_reportCharacterNameBuf, sizeof(s_reportCharacterNameBuf), "%s", chr.c_str());
}

// Appends a row auto-filled from a live-log entry -- this is what the
// live log's per-entry "report new" button (RenderLiveLogSection) calls.
// Also (re-)syncs the reporter-identity name fields from GameState right
// here -- see RefreshReportNameFieldsFromGameState's comment for why
// that's tied to this call specifically. Exposed at file scope (not
// static) so that section, defined earlier in this same translation
// unit, can reach it.
void AddReportRowFromLiveLogEntry(const LiveLogEntry& entry)
{
    RefreshReportNameFieldsFromGameState();

    ReportFormRow row;
    std::snprintf(row.guid, sizeof(row.guid), "%s", entry.guid_b64.c_str());
    std::snprintf(row.typeText, sizeof(row.typeText), "%d", entry.type);
    if (entry.hasSelfContext)
    {
        row.mapID      = (int)entry.mapID;
        row.raceIndex  = RaceToIndex(entry.race);
        row.profession = entry.profession;
        if (entry.specialization != 0)
        {
            const char* name = SpecializationName(entry.specialization);
            if (name)
                std::snprintf(row.specializationText, sizeof(row.specializationText), "%s", name);
            else
                std::snprintf(row.specializationText, sizeof(row.specializationText), "%u", entry.specialization);
        }
    }
    // else: leave every field at its default -- nothing was ever
    // observed for this GUID, same as before, just no longer hidden.
    s_reportRows.push_back(row);
}

namespace {

// Local to this file's report-form code -- report.cpp has its own Trim
// for the same purpose, but it's private to that translation unit, and
// this is the one new spot in addon.cpp that needs it (user-typed name
// fields, below).
std::string TrimReportText(const std::string& s)
{
    size_t start = 0, end = s.size();
    while (start < end && std::isspace((unsigned char)s[start])) ++start;
    while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(start, end - start);
}

// "Not set" tri-state per the HANDOFF's "Type gets its own independent
// 'Not set' state" note -- blank (or, case-insensitively, the literal
// text "not set") means unset; otherwise must parse as 0-11. Returns
// false (with outError set) for anything else, e.g. stray text or a
// number outside that range.
bool ParseReportTypeText(const std::string& text, bool& outIsSet, int& outValue, std::string& outError)
{
    std::string trimmed = text;
    size_t start = 0, end = trimmed.size();
    while (start < end && std::isspace((unsigned char)trimmed[start])) ++start;
    while (end > start && std::isspace((unsigned char)trimmed[end - 1])) --end;
    trimmed = trimmed.substr(start, end - start);

    std::string lower = trimmed;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);

    if (trimmed.empty() || lower == "not set")
    {
        outIsSet = false;
        outValue = -1;
        return true;
    }

    try
    {
        size_t consumed = 0;
        int value = std::stoi(trimmed, &consumed);
        if (consumed != trimmed.size() || value < 0 || value > 11)
        {
            outError = "Type must be blank/\"Not set\" or a number 0-11.";
            return false;
        }
        outIsSet = true;
        outValue = value;
        return true;
    }
    catch (...)
    {
        outError = "Type must be blank/\"Not set\" or a number 0-11.";
        return false;
    }
}

// Every known (id, name) pair, built once by probing SpecializationName()
// across an id range comfortably past today's 81-entry table (see
// specialization_names.h) -- so a newly-added elite spec shows up in the
// autocomplete list automatically the next time that table's updated,
// without touching this loop again. Kept as strings (not const char*)
// since SpecializationName() only promises its return value is valid for
// the call's duration, not for the lifetime of this cache.
const std::vector<std::pair<unsigned int, std::string>>& AllSpecializations()
{
    static const std::vector<std::pair<unsigned int, std::string>> all = []
    {
        std::vector<std::pair<unsigned int, std::string>> v;
        for (unsigned int id = 1; id <= 200; ++id)
            if (const char* name = SpecializationName(id))
                v.emplace_back(id, name);
        return v;
    }();
    return all;
}

std::string LowerCopy(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Resolves the autocomplete box's free-typed text to a specialization id:
// blank -> 0 ("not set"); an all-digits string -> that raw id, taken as
// typed (still allowed, same as the old hasSelfContext-snapshot path --
// e.g. a future elite spec not yet in this table); otherwise an exact
// case-insensitive name match against AllSpecializations(). Anything
// else (a typo, a partial word) resolves to 0/"not set" rather than
// rejecting the whole submission -- this field is a convenience, not a
// validated one, same spirit as leaving a GUID row's Type blank.
unsigned int ResolveSpecializationId(const std::string& text)
{
    std::string trimmed = TrimReportText(text);
    if (trimmed.empty())
        return 0;

    bool allDigits = std::all_of(trimmed.begin(), trimmed.end(),
                                  [](unsigned char c) { return std::isdigit(c) != 0; });
    if (allDigits)
        return (unsigned int)std::stoul(trimmed);

    std::string needle = LowerCopy(trimmed);
    for (const auto& entry : AllSpecializations())
        if (LowerCopy(entry.second) == needle)
            return entry.first;

    return 0;
}

// Renders the exact per-GUID block template from the HANDOFF's "Payload
// shape sent to the relay" section. This is the one place that turns raw
// enum/numeric self-context values into the human-readable names the
// live log's own tree already uses (GameState_RaceName/ProfessionName,
// SpecializationName) -- report.cpp never sees anything but this
// finished string.
//
// Self context used to be all-or-nothing (HANDOFF's original "one
// bundled block" write rule, for the *live-capture* path -- unaffected
// by this change). This form now lets the user adjust any of the four
// fields independently, so the composed text follows suit: only when
// every field is still at its own default does this print the original
// "Not observed" line; otherwise it prints all four, substituting
// "Unknown" for whichever ones are still unset -- the same word
// GameState_RaceName/ProfessionName already fall back to for an
// out-of-range value, reused here for "not filled in" instead.
std::string ComposeReportGuidBlock(const std::string& guid, bool typeIsSet, int typeValue, const ReportFormRow& row)
{
    std::ostringstream out;
    out << "GUID: " << guid << "\n";
    out << "  Type: " << (typeIsSet ? std::to_string(typeValue) : std::string("Not set")) << "\n";

    unsigned int specId  = ResolveSpecializationId(row.specializationText);
    bool         raceSet = (row.raceIndex != kRaceUnset);
    bool         profSet = (row.profession != Mumble::EProfession::None);
    bool         specSet = (specId != 0);
    bool         mapSet  = (row.mapID != 0);

    if (!mapSet && !raceSet && !profSet && !specSet)
    {
        out << "  Self context: Not observed\n";
    }
    else
    {
        std::string specName = "Unknown";
        if (specSet)
        {
            if (const char* n = SpecializationName(specId))
                specName = n;
            else
                specName = std::to_string(specId);
        }

        out << "  Self context: MapID " << row.mapID << ", "
            << (raceSet ? GameState_RaceName(kRaceValues[row.raceIndex]) : "Unknown") << ", "
            << (profSet ? GameState_ProfessionName(row.profession) : "Unknown") << ", "
            << specName << "\n";
    }
    return out.str();
}

} // namespace

void RenderReportSection()
{
    // The tree isn't necessarily loaded yet if the user opens this header
    // before ever expanding "Installed Effects" -- load it here too so
    // per-GUID display names elsewhere in this addon have something to
    // check against, same lazy-load-on-first-open pattern
    // RenderInstalledEffects already uses.
    if (!s_installedTreeLoaded)
        LoadInstalledEffectsTree();

    const float kFieldWidth = 320.0f;
    EReportStatus reportStatus = GetReportStatus();
    bool sending = (reportStatus == EReportStatus::Sending);

    // --- Reporter identity -----------------------------------------
    // Auto-filled (and re-synced) only by "report new" -- see
    // RefreshReportNameFieldsFromGameState/AddReportRowFromLiveLogEntry
    // above -- NOT here on render/section-open, and NOT re-read every
    // frame either way. This section just displays/edits whatever's
    // currently in the buffers; it never populates them itself. Until
    // the first "report new" click of the session, both boxes simply
    // start blank (showing their hint text) -- there's nothing to
    // auto-fill from yet.
    ImGui::PushItemWidth(kFieldWidth);
    if (s_reportAnonymous)
    {
        // Anonymous wins outright while checked: show fixed text, not an
        // editable box the user could half-clear and retype into,
        // which would defeat "anonymous". Whatever's in the buffers is
        // preserved underneath and reappears if the box is unchecked.
        ImGui::TextDisabled("Account Name (RTAPI only):");
        ImGui::TextDisabled("(anonymous)");
        ImGui::TextDisabled("Character Name:");
        ImGui::TextDisabled("(anonymous)");
    }
    else
    {
        ImGui::TextDisabled("Account Name (RTAPI only):");
        ImGui::InputTextWithHint("##report_account_name", "(unavailable -- type your own)",
                                  s_reportAccountNameBuf, sizeof(s_reportAccountNameBuf));
        ImGui::TextDisabled("Character Name:");
        ImGui::InputTextWithHint("##report_character_name", "(unavailable -- type your own)",
                                  s_reportCharacterNameBuf, sizeof(s_reportCharacterNameBuf));
    }
    ImGui::PopItemWidth();

    ImGui::Checkbox("Send anonymously", &s_reportAnonymous);
    ImGui::Separator();

    // --- Per-GUID blocks --------------------------------------------
    ImGui::TextWrapped(
        "Attach zero or more GUIDs -- click \"report new\" next to an entry in the "
        "Live Log, or add one by hand below.");

    int removeIndex = -1;
    for (size_t i = 0; i < s_reportRows.size(); ++i)
    {
        ReportFormRow& row = s_reportRows[i];
        ImGui::PushID((int)i);

        ImGui::PushItemWidth(kFieldWidth * 0.7f);
        ImGui::InputText("GUID", row.guid, sizeof(row.guid));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::PushItemWidth(50.0f);
        ImGui::InputText("Type", row.typeText, sizeof(row.typeText));
        ImGui::PopItemWidth();

        // --- Self context -- always shown now, never hidden, and every
        // field below is independently editable (HANDOFF's "manually
        // adjustable" follow-up). Each is pre-filled from a live-log
        // snapshot when "report new" populated this row and that GUID
        // had one; otherwise it just starts at its own "not set"
        // default, same as a manually-added row -- either way, nothing
        // here is read-only. All four sit on this same line by design,
        // so a report with self context reads as one unit at a glance.
        ImGui::PushItemWidth(40.0f);
        ImGui::InputInt("MapID", &row.mapID, 0, 0); // no +/- step buttons -- a raw id, not a counter
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushItemWidth(60.0f);
        const char* racePreview = (row.raceIndex == kRaceUnset) ? "Race" : GameState_RaceName(kRaceValues[row.raceIndex]);
        if (ImGui::BeginCombo("##combo_race", racePreview))
        {
            if (ImGui::Selectable("Race", row.raceIndex == kRaceUnset))
                row.raceIndex = kRaceUnset;
            for (int r = 0; r < (int)std::size(kRaceValues); ++r)
            {
                bool selected = (row.raceIndex == r);
                if (ImGui::Selectable(GameState_RaceName(kRaceValues[r]), selected))
                    row.raceIndex = r;
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushItemWidth(80.0f);
        const bool  profUnset    = (row.profession == Mumble::EProfession::None);
        const char* profPreview  = profUnset ? "Profession" : GameState_ProfessionName(row.profession);
        if (ImGui::BeginCombo("##combo_profession", profPreview))
        {
            if (ImGui::Selectable("Profession", profUnset))
                row.profession = Mumble::EProfession::None;
            for (Mumble::EProfession p : kProfessionValues)
            {
                bool selected = (row.profession == p);
                if (ImGui::Selectable(GameState_ProfessionName(p), selected))
                    row.profession = p;
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();

        // Specialization: a free-text box with live autocomplete rather
        // than a dropdown -- easier to search 80+ names by typing than
        // by scrolling, per this session's ask. Suggestions render as
        // plain Selectable() rows right below this same input (not a
        // popup) specifically so the input keeps keyboard focus while
        // the user keeps typing; a real popup would steal focus away
        // from it every frame.
        ImGui::PushItemWidth(90.0f);
        bool specTextChanged = ImGui::InputTextWithHint("##combo_specialization", "Specialization", row.specializationText, sizeof(row.specializationText));
        bool specJustActivated = ImGui::IsItemActivated();
        ImVec2 specBoxMin = ImGui::GetItemRectMin();
        ImVec2 specBoxMax = ImGui::GetItemRectMax();
        ImGui::PopItemWidth();
        ImGui::SameLine();

        if (ImGui::SmallButton("Remove"))
            removeIndex = (int)i;

        // Real floating popup instead of inline Selectable()s gated on
        // IsItemActive() -- clicking a suggestion moves focus off the
        // input in that same frame, so an IsItemActive()-gated inline
        // list can vanish (or shift layout underneath the click) before
        // the click actually lands. OpenPopup here fires once, when you
        // start typing or click into the box -- not every frame based
        // on focus -- so the popup survives the click, and Selectable()
        // inside a popup already closes it correctly on click.
        const char* specPopupId = "##spec_suggest_popup";
        if ((specJustActivated || specTextChanged) && row.specializationText[0] != '\0')
            ImGui::OpenPopup(specPopupId);

        if (ImGui::IsPopupOpen(specPopupId))
        {
            ImGui::SetNextWindowPos(ImVec2(specBoxMin.x, specBoxMax.y));
            ImGui::SetNextWindowSize(ImVec2(specBoxMax.x - specBoxMin.x, 0));
        }
        if (ImGui::BeginPopup(specPopupId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing))
        {
            if (row.specializationText[0] == '\0')
            {
                ImGui::CloseCurrentPopup();
            }
            else
            {
                std::string needle = LowerCopy(row.specializationText);
                int shown = 0;
                for (const auto& entry : AllSpecializations())
                {
                    if (shown >= 8)
                        break;
                    if (LowerCopy(entry.second).find(needle) == std::string::npos)
                        continue;
                    if (ImGui::Selectable(entry.second.c_str()))
                    {
                        std::snprintf(row.specializationText, sizeof(row.specializationText), "%s", entry.second.c_str());
                        ImGui::CloseCurrentPopup();
                    }
                    ++shown;
                }
                if (shown == 0)
                    ImGui::TextDisabled("(no match -- will send as Unknown)");
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        ImGui::Separator();
    }
    if (removeIndex >= 0)
        s_reportRows.erase(s_reportRows.begin() + removeIndex);

    if (ImGui::SmallButton("+ Add GUID manually"))
        s_reportRows.push_back(ReportFormRow{});

    ImGui::Spacing();
    ImGui::TextDisabled("Additional information (required):");
    ImGui::InputTextMultiline("##report_note", s_reportNoteBuf, sizeof(s_reportNoteBuf), ImVec2(kFieldWidth, 80));

    // imgui 1.80 doesn't have BeginDisabled/EndDisabled -- swap the label
    // and ignore clicks while busy instead, same workaround used elsewhere
    // in this file (e.g. "Check now"/"Apply this update").
    if (ImGui::Button(sending ? "Sending..." : "Send") && !sending)
    {
        std::string error;
        std::vector<ReportGuidBlock> payloadEntries;
        payloadEntries.reserve(s_reportRows.size());
        bool typeParseFailed = false;

        for (const ReportFormRow& row : s_reportRows)
        {
            bool typeIsSet = false;
            int  typeValue = -1;
            if (!ParseReportTypeText(row.typeText, typeIsSet, typeValue, error))
            {
                typeParseFailed = true;
                break;
            }

            ReportGuidBlock entry;
            entry.guid  = row.guid;
            entry.block = ComposeReportGuidBlock(row.guid, typeIsSet, typeValue, row);
            payloadEntries.push_back(std::move(entry));
        }

        if (typeParseFailed)
        {
            s_reportFormError = error;
        }
        else
        {
            std::string reporterLine;
            if (s_reportAnonymous)
            {
                reporterLine = "Reporter: (anonymous)";
            }
            else
            {
                // Use whatever's actually in the boxes now -- may be the
                // auto-filled value untouched, edited, or (if
                // auto-fill had nothing to offer) typed from scratch.
                std::string acct = TrimReportText(s_reportAccountNameBuf);
                std::string chr  = TrimReportText(s_reportCharacterNameBuf);
                if (acct.empty()) acct = "(unknown)";
                if (chr.empty())  chr  = "(unknown)";
                reporterLine = "Reporter: " + acct + " / " + chr;
            }

            if (StartSendReport(reporterLine, payloadEntries, s_reportNoteBuf, error))
            {
                s_reportFormError.clear();
                s_reportRows.clear();
                s_reportNoteBuf[0] = '\0';
            }
            else
            {
                s_reportFormError = error;
            }
        }
    }

    if (!s_reportFormError.empty())
        ImGui::TextColored(kDuplicateColor, "%s", s_reportFormError.c_str());

    std::string lastMsg = GetLastReportMessage();
    if (!lastMsg.empty())
    {
        const ImVec4* color = (reportStatus == EReportStatus::Error) ? &kDuplicateColor : nullptr;
        if (color)
            ImGui::TextColored(*color, "%s", lastMsg.c_str());
        else
            ImGui::TextWrapped("%s", lastMsg.c_str());
    }
}

// ---------------------------------------------------------------------------
// Rollback (HANDOFF's goal #2). Every write this addon makes -- applied
// update, saved edit, category rename/move -- leaves a ".bak" of what was
// there before (see "Safety on write"). At most one exists per sin at a
// time (each new write overwrites the previous one), so there's never more
// than 3 total -- not enough to need a "clean up old backups" action, just
// something worth being able to undo. This lists whatever backup.h's
// ScanBackups finds and offers a per-file "Roll back". Only one backup
// generation is ever kept (an open decision in the handoff, settled that
// way), which is why RestoreBackup -- and the wording below -- describe
// rollback as a swap rather than a one-way trip.
// ---------------------------------------------------------------------------
static std::string s_backupsActionMessage; // last rollback outcome, shown until the next action

void RenderBackupsSection()
{
    // Same lazy-load-if-needed pattern as RenderReportSection -- this
    // section can be opened without ever expanding "Installed Effects"
    // first, but a rollback needs to know the sin's *current* on-disk
    // filename (it may differ from the backup's, after an applied update)
    // to clean up the stale file once the restore succeeds.
    if (!s_installedTreeLoaded)
        LoadInstalledEffectsTree();

    std::vector<BackupInfo> backups = ScanBackups(s_denoiserAddonDir);

    if (backups.empty())
    {
        ImGui::TextDisabled("No backup files -- nothing to roll back.");
        return;
    }

    ImGui::TextWrapped(
        "Every applied update, saved edit, or category rename leaves a "
        "\".bak\" of what was there just before. Rolling one back swaps it "
        "back in -- pressing \"Roll back\" again on the same entry undoes "
        "that swap, since only one backup generation is ever kept.");

    ImGui::Separator();

    for (const auto& backup : backups)
    {
        ImGui::PushID(backup.bakPath.c_str());

        ImGui::TextWrapped("%s -- backup of %s (%.1f KB)",
            backup.sinName.c_str(),
            fs::path(backup.restorePath).filename().string().c_str(),
            static_cast<double>(backup.fileSize) / 1024.0);

        if (ImGui::Button("Roll back"))
        {
            // The sin's live file might currently sit at a different path
            // than backup.restorePath (an applied update bumps the
            // version-stamped filename) -- look up whatever's actually
            // installed for this sin right now so RestoreBackup can clean
            // up the stale one after restoring. Empty if nothing's
            // currently installed for this sin at all.
            std::string currentPath;
            for (const auto& sin : s_installedSins)
            {
                if (sin.sinName == backup.sinName)
                {
                    currentPath = sin.fullPath;
                    break;
                }
            }

            std::string error;
            if (RestoreBackup(backup, currentPath, error))
            {
                s_backupsActionMessage = "Rolled back " + backup.sinName + ".";
                s_installedTreeLoaded  = false; // disk changed -- reload next expand
            }
            else
            {
                s_backupsActionMessage = "Rollback failed for " + backup.sinName + ": " + error;
            }
        }

        ImGui::PopID();
    }

    if (!s_backupsActionMessage.empty())
        ImGui::TextWrapped("%s", s_backupsActionMessage.c_str());
}

// Own collapsible header, separate from Installed Effects / Backups /
// Report an Effect -- this is live incoming data over the Nexus event
// bridge (live_log.h/vfxd_sins_bridge.h), not anything read off disk.
// See HANDOFF's "Live log display" writeup for the full design this
// implements.
void RenderLiveLogSection()
{
    // Same lazy-load-if-needed pattern as RenderReportSection/
    // RenderBackupsSection -- resolving an incoming guid to a sin effect
    // name needs whatever's actually installed right now.
    if (!s_installedTreeLoaded)
        LoadInstalledEffectsTree();
    LiveLog_SetKnownGuidNames(CollectGuidNameMap());
    LiveLog_SetKnownGuidBehaviors(CollectGuidBehaviorMap());

    // Per-type "log this at all" filters, checked at ingestion (drop
    // before ever becoming/updating an entry) -- independent of the
    // listen toggle and "hide known" below. Deliberately not persisted:
    // resets to the built-in defaults every addon reload (see
    // live_log.cpp), since these are exploratory filters for
    // characterizing what each numeric type actually is, not settings
    // meant to stick. Wrapped to two rows of 6 rather than one long row.
    static const char* const kTypeTooltips[kLiveLogTypeCount] = {
        "Type 0: never visible, sometimes linked to sounds.",
        "Type 1: a group -- hiding this hides all the effects of that group.",
        "Type 2: most times invisible; only one occasion found related to a visible effect.",
        "Type 3: only one encounter so far, as a follow-up effect.",
        "Type 4: tether between 2 entities.",
        "Type 5: effects that change the color of the body (infusions, stealth, etc).",
        "Type 6: most effects use this.",
        "Type 7: never encountered yet.",
        "Type 8: only invisible so far, rare.",
        "Type 9: only invisible so far, most times at the end of projectile effects.",
        "Type 10: most times weapon trails.",
        "Type 11: same as type 1, maybe a newer implementation.",
    };

    ImGui::TextDisabled("Types logged:");
    for (int t = 0; t < kLiveLogTypeCount; ++t)
    {
        ImGui::PushID(t);
        bool enabled = LiveLog_GetTypeEnabled(t);
        char label[8];
        std::snprintf(label, sizeof(label), "%d", t);
        if (ImGui::Checkbox(label, &enabled))
            LiveLog_SetTypeEnabled(t, enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", kTypeTooltips[t]);
        ImGui::PopID();
        if (t != kLiveLogTypeCount - 1 && (t % 6) != 5)
            ImGui::SameLine();
    }
    ImGui::Separator();

    bool listening = LiveLog_IsListening();
    if (ImGui::Checkbox("Capture live (VfxDenoiser)", &listening))
        LiveLog_SetListening(s_api, listening);

    bool hideKnown = LiveLog_GetHideKnown();
    if (ImGui::Checkbox("Hide effects already in a sin file", &hideKnown))
        LiveLog_SetHideKnown(hideKnown);

    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        LiveLog_Clear();

    if (!listening)
        ImGui::TextDisabled("Not capturing -- toggle \"Capture live\" above while VfxDenoiser's patch (or the test stub) is running.");

    const auto& entries = LiveLog_GetEntries();
    if (entries.empty())
    {
        ImGui::TextDisabled("Nothing captured yet.");
        return;
    }

    // Rendered in the order each guid was first received (LiveLogEntry::
    // firstSeenSeq, assigned once on first sight and never touched again
    // by later "latest wins" updates) -- deliberately not alphabetical or
    // any other re-derived order, so the list doesn't reshuffle every time
    // an already-seen entry's fields update.
    std::vector<const LiveLogEntry*> sorted;
    sorted.reserve(entries.size());
    for (const auto& [guid, entry] : entries)
        sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(), [](const LiveLogEntry* a, const LiveLogEntry* b)
    {
        return a->firstSeenSeq < b->firstSeenSeq;
    });

    for (const LiveLogEntry* entry : sorted)
    {
        ImGui::PushID(entry->guid_b64.c_str());
        bool open = ImGui::TreeNode(entry->displayName.c_str());

        // Hidden for a GUID already in an installed sin file (knownInSin)
        // -- unchanged existing rule. Shown next to the row regardless of
        // whether it's expanded, since a whole-row action shouldn't
        // require opening the tree first. No "already added to this
        // pending report" guard needed here -- report.cpp's
        // reject-whole-submission-on-duplicate-guid check (client-side,
        // at send time) already covers accidentally adding the same GUID
        // twice.
        if (!entry->knownInSin)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("report new"))
                AddReportRowFromLiveLogEntry(*entry);
        }

        if (open)
        {
            // Shown either way once unfolded, just secondary when a name
            // already exists -- unknown entries already show the guid as
            // their collapsed-row label.
            if (entry->knownInSin)
            {
                ImGui::Text("GUID: %s", entry->guid_b64.c_str());
                // Looked up independently against *this user's* installed
                // sin JSON -- not read off the incoming event. This is the
                // trustworthy one for a known guid; VfxDenoiser's own
                // event-side resolution is gone entirely (see live_log.h).
                ImGui::Text("Configured behavior: %s",
                             entry->installedBehavior.empty() ? "(not configured)" : entry->installedBehavior.c_str());
            }

            // a4/a6 stay internal-only (semantically opaque, never
            // rendered) -- only Type/Duration/Target/Caster show here.
            if (ImGui::TreeNode("Data"))
            {
                ImGui::Text("Type: %d", entry->type);
                ImGui::Text("Duration: %d", entry->duration);
                ImGui::Text("Target: %s", entry->target.c_str());
                ImGui::Text("Caster: %s", entry->caster.c_str());
                ImGui::TreePop();
            }

            // Only built once this GUID has ever had a self-event
            // (hasSelfContext), not based on the *current* caster/target --
            // "last seen" means it stays visible even if this same effect
            // is later logged by/against someone else. mapID/race/
            // profession/spec are only ever written on a self-event to
            // begin with (see IngestLogLine's isSelfEvent branch) and are
            // never cleared afterward, so once true this section always
            // has real data to show. Race/Profession are named directly
            // from Mumble.h's own enum (see game_state.cpp) -- always a
            // real name. Specialization falls back to the raw numeric id
            // until specialization_names.cpp's table is filled in (see
            // that file for why it's still empty).
            if (entry->hasSelfContext && ImGui::TreeNode("Self (last seen)"))
            {
                ImGui::Text("MapID: %u", entry->mapID);
                ImGui::Text("Race: %s", GameState_RaceName(entry->race));
                ImGui::Text("Profession: %s", GameState_ProfessionName(entry->profession));
                if (const char* specName = SpecializationName(entry->specialization))
                    ImGui::Text("Specialization: %s", specName);
                else
                    ImGui::Text("Specialization: %u", entry->specialization);
                ImGui::TreePop();
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Three always-visible columns, one per known sin (kSinNames order:
// Gluttony, Pride, Sloth) -- the entry point for both "get this sin at all"
// and "see/apply a pending update," entirely from up here. Deliberately
// placed above the collapsing headers so it doesn't require expanding
// anything, and is now the ONLY place any of this lives -- there is no
// separate "Check now" section below anymore.
//
// NotInstalled calls StartInstallSin directly (a fresh file, nothing to
// preview -- there's no local copy to diff against). UpdateAvailable's
// button doubles as both steps of the check->apply cycle for just that one
// sin: first click calls StartLoadDiff, and once that resolves, the same
// button relabels to "Apply changes" and applies via StartApplyUpdate. The
// diff's result text (RenderSinDiffStatus) renders right under that same
// button once it has something to say, rather than in a separate section
// elsewhere -- see that function's own comment for why.
// ---------------------------------------------------------------------------
static void RenderSinActionRow()
{
    static const char* kSinDescriptions[kSinCount] = {
        "Hides all collected effects.",
        "Hides all collected effects from other players.",
        "Hides all collected effects from other players; insecure effects are also removed.",
    };

    ECheckStatus checkStatus = GetCheckStatus();
    EApplyStatus applyStatus = GetApplyStatus();
    bool checking = (checkStatus == ECheckStatus::Checking);
    bool applying = (applyStatus == EApplyStatus::Applying);

    // The pending-sin tag only means anything while something is actually
    // applying -- once it settles (Done/Error/Idle) the label it was
    // reserving is stale.
    if (!applying)
        s_pendingActionSin.clear();

    if (checkStatus == ECheckStatus::Error)
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "Last check failed -- showing previous results, if any.");

    std::vector<SinUpdateInfo> sinInfo = GetSinUpdateInfo();
    std::vector<SinDiffInfo>   diffs   = GetSinDiffInfo();

    // imgui 1.80 doesn't have BeginDisabled/EndDisabled -- every button
    // below follows addon.cpp's existing convention elsewhere (swap the
    // label, ignore the click) rather than true graying-out.
    ImGui::Columns(kSinCount, "sin_action_columns", false);
    for (int i = 0; i < kSinCount; ++i)
    {
        std::string sinName = kSinNames[i];
        ImGui::PushID(sinName.c_str());

        ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f), "%s", sinName.c_str());
        ImGui::TextWrapped("%s", kSinDescriptions[i]);

        const SinUpdateInfo* info = nullptr;
        for (const auto& s : sinInfo)
            if (s.sinName == sinName) { info = &s; break; }

        // No result yet at all (e.g. the very first frame or two after
        // addon load, before the on-load check has landed) reads the same
        // as NotInstalled for button purposes -- it'll settle within a
        // frame or two once GetSinUpdateInfo() has something.
        ESinUpdateState state = info ? info->state : ESinUpdateState::NotInstalled;
        bool pendingHere = (applying && s_pendingActionSin == sinName);

        if (checking)
        {
            ImGui::Button("Checking...");
        }
        else if (state == ESinUpdateState::NotInstalled)
        {
            bool hasUrl = info && !info->latestDownloadUrl.empty();
            const char* label = pendingHere ? "Installing..." : "Install";
            if (ImGui::Button(label) && !applying && hasUrl)
            {
                s_pendingActionSin = sinName;
                StartInstallSin(s_denoiserAddonDir, sinName);
            }
            if (!hasUrl && !pendingHere)
                ImGui::TextDisabled("Not available yet.");
        }
        else if (state == ESinUpdateState::UpdateAvailable)
        {
            // Same button doubles as two steps: first click loads just
            // THIS sin's diff (StartLoadDiff's per-sin filter -- doesn't
            // touch the other two outdated sins, if any), which is also
            // what makes RenderSinDiffStatus below have something to show
            // and the colored Installed Effects tree overlay appear
            // further down (both already key off GetSinDiffInfo(); no
            // separate wiring needed for that part). Once that diff is
            // Ready, the same button relabels to "Apply changes" and a
            // second click applies it via StartApplyUpdate.
            const SinDiffInfo* diff = nullptr;
            for (const auto& d : diffs)
                if (d.sinName == sinName) { diff = &d; break; }
            EDiffStatus diffStatus = diff ? diff->status : EDiffStatus::NotLoaded;

            if (info)
                ImGui::Text("v%d -> v%d", info->installedVersion, info->latestVersion);

            const char* label = "Update available";
            bool clickable = false;
            bool isApplyStep = false;
            switch (diffStatus)
            {
                case EDiffStatus::NotLoaded:
                    label = "Update available"; clickable = true; break;
                case EDiffStatus::Loading:
                    label = "Loading...";       clickable = false; break;
                case EDiffStatus::Ready:
                    label = pendingHere ? "Applying..." : "Apply changes";
                    clickable = !pendingHere; isApplyStep = true; break;
                case EDiffStatus::Error:
                    label = "Error -- retry";   clickable = true; break;
                case EDiffStatus::Blocked:
                    // Never becomes Ready until the duplicate guid this is
                    // warning about is resolved (see the Installed
                    // Effects tree below) -- clicking this button again
                    // just re-checks that.
                    label = "Blocked -- see below"; clickable = true; break;
            }

            if (ImGui::Button(label) && clickable && !applying)
            {
                if (isApplyStep)
                {
                    s_pendingActionSin = sinName;
                    StartApplyUpdate(s_denoiserAddonDir, sinName);
                }
                else
                {
                    StartLoadDiff(s_denoiserAddonDir, sinName);
                }
            }

            RenderSinDiffStatus(diff);
        }
        else // UpToDate (or Unknown, treated the same -- nothing actionable)
        {
            ImGui::Button("Up to date");
        }

        ImGui::NextColumn();
        ImGui::PopID();
    }
    ImGui::Columns(1);

    std::string lastMsg = GetLastApplyMessage();
    if (!lastMsg.empty())
        ImGui::TextWrapped("%s", lastMsg.c_str());

    // An apply/install just wrote new content to disk -- drop the
    // installed-tree cache so the next time that section is open/expanded
    // it reloads from the just-written file rather than showing what was
    // there before the update. Compared against the message text rather
    // than a one-shot flag since GetLastApplyMessage() is what's already
    // being polled every frame here.
    static std::string s_lastSeenApplyMsg;
    if (lastMsg != s_lastSeenApplyMsg)
    {
        s_lastSeenApplyMsg = lastMsg;
        if (!lastMsg.empty())
            s_installedTreeLoaded = false;
    }

    ImGui::Separator();
}

void OptionsRenderCallback()
{
    if (!s_denoiserFound.load())
    {
        ImGui::TextDisabled("VfxDenoiser isn't installed -- nothing to update.");
        return;
    }

    // imgui 1.80 doesn't have SeparatorText (added in a later version).
    ImGui::Text("Visual Sins Updater");
    ImGui::Separator();

    RenderSinActionRow();

    if (ImGui::CollapsingHeader("Installed Effects"))
        RenderInstalledEffects();

    if (ImGui::CollapsingHeader("Live Log (VfxDenoiser)"))
        RenderLiveLogSection();

    if (ImGui::CollapsingHeader("Backups"))
        RenderBackupsSection();

    if (ImGui::CollapsingHeader("Report an Effect"))
        RenderReportSection();
}

void Addon_Load(AddonAPI_t* aApi)
{
    ImGui::SetCurrentContext((ImGuiContext*)aApi->ImguiContext);

    s_api = aApi;
    SetUpdaterLogger(aApi); // so github_update.cpp's background-thread failures can also reach Nexus's log
    GameState_Init(aApi);   // caches DL_MUMBLE_LINK/_IDENTITY and DL_RTAPI pointers -- see game_state.h
    LiveLog_Init(aApi);     // subscribes EV_VFXD_SINS_LOG -- see live_log.h/vfxd_sins_bridge.h

    // "<GW2>/addons/VfxDenoiser" -- Paths_GetAddonDirectory only ever
    // *constructs* this path string (see its doc comment in Nexus.h); it
    // doesn't check whether the folder is actually there. Checking
    // fs::exists ourselves is what makes s_denoiserFound mean what its own
    // comment says it means.
    s_denoiserAddonDir = aApi->Paths_GetAddonDirectory("VfxDenoiser");
    std::error_code ec;
    bool found = !s_denoiserAddonDir.empty() && fs::is_directory(s_denoiserAddonDir, ec) && !ec;
    s_denoiserFound.store(found);

    aApi->GUI_Register(RT_OptionsRender, OptionsRenderCallback);

    if (found)
    {
        // One check on load -- version numbers only, no downloading (see
        // StartUpdateCheck's alsoLoadDiff parameter). If this finds an
        // update it just shows as a note in the options panel; the
        // "Check now" button is what actually downloads and diffs
        // anything, so nothing gets fetched until the user asks for it.
        StartUpdateCheck(s_denoiserAddonDir);
    }
    else
    {
        aApi->Log(LOGL_INFO, "VfxDSinsUpdater", "VfxDenoiser addon folder not found -- nothing to check.");
    }
}

void Addon_Unload(AddonAPI_t* aApi)
{
    if (aApi)
        aApi->GUI_Deregister(OptionsRenderCallback);

    LiveLog_Shutdown(aApi); // unsubscribes, and raises LISTEN_STOP if capture was still on (no-op if aApi is null)
    GameState_Shutdown();   // clears cached DataLink pointers -- safe even if GameState_Init was never reached

    // Unblock any WinHTTP call currently parked mid-request so the
    // background thread can exit promptly.
    CancelInFlightUpdateRequest();
    CancelInFlightReportRequest();

    // Best-effort: give any in-flight background thread a brief window to
    // notice the cancellation and exit before the DLL potentially gets
    // unloaded out from under it. This is a minimal safety net, not a
    // guarantee -- a production version of this should track its threads
    // explicitly (e.g. the reference project's WaitForBackgroundThreads)
    // rather than relying on a fixed sleep.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}