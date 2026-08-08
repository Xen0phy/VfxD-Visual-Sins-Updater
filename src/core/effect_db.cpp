//################################################################################
// effect_db.cpp
//--------------------------------------------------------------------------------
// See effect_db.h for the module contract and schema writeup. SQLite
// backend, one connection, opened/closed with the module's own lifecycle
// rather than per-call. Category paths are stored as a single TEXT
// column, segments joined with '\x1f' (unit separator) rather than "/"
// or " / " -- installed_tree_overlay.h's JoinPath's " / " is a *display*
// join and a real category name could itself contain either character;
// \x1f can't collide with anything a user types in the tree UI.
//
// Not thread-safe by design, same assumption live_log.cpp already makes:
// every function here is expected to run on the render/update thread
// that owns the Live Log panel and its ingestion, never from
// github_update.cpp's background threads (which is also why those
// threads are locked out entirely while "for science" is enabled -- see
// EffectDb_IsEnabled's callers in github_update.cpp).
//--------------------------------------------------------------------------------

#include "effect_db.h"

#include "sqlite3.h" //. vendored amalgamation -- see CMakeLists.txt

#include <chrono>
#include <filesystem>

namespace {

sqlite3*     s_db  = nullptr;
AddonAPI_t*  s_api = nullptr;
bool         s_enabled = false;
int          s_generation = 0;

sqlite3_stmt* s_insertEffectStmt      = nullptr;
sqlite3_stmt* s_insertOccurrenceStmt  = nullptr;
sqlite3_stmt* s_insertGroupMemberStmt = nullptr;
sqlite3_stmt* s_selectEffectStmt     = nullptr;
sqlite3_stmt* s_selectKnownStmt      = nullptr;
sqlite3_stmt* s_selectOccurrenceStmt = nullptr;
sqlite3_stmt* s_selectGroupsStartedStmt   = nullptr;
sqlite3_stmt* s_selectGroupsMemberOfStmt  = nullptr;
sqlite3_stmt* s_updateNameStmt       = nullptr;
sqlite3_stmt* s_updateCategoryStmt   = nullptr;

std::chrono::steady_clock::time_point s_lastPoll{};

constexpr char kCategoryDelim = '\x1f';

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LogFailure
//--------------------------------------------------------------------------------
// Same aApi->Log(LOGL_CRITICAL, ...) convention every other module in
// this addon already uses for its own write-failure paths (see
// installed_tree_store.cpp).
//--------------------------------------------------------------------------------
void LogFailure(const std::string& msg)
{
    if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", msg.c_str());
}

std::string JoinCategoryPath(const std::vector<std::string>& path)
{
    std::string out;
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (i) out += kCategoryDelim;
        out += path[i];
    }
    return out;
}

std::vector<std::string> SplitCategoryPath(const std::string& joined)
{
    std::vector<std::string> out;
    if (joined.empty()) return out;

    size_t start = 0;
    while (true)
    {
        size_t pos = joined.find(kCategoryDelim, start);
        if (pos == std::string::npos)
        {
            out.push_back(joined.substr(start));
            break;
        }
        out.push_back(joined.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// FinalizeAllStatements / PrepareAllStatements
//--------------------------------------------------------------------------------
// One place owning every prepared statement's lifetime, paired with
// EffectDb_Open/Close -- statements are prepared once per connection and
// reused (sqlite3_reset + sqlite3_clear_bindings between calls) rather
// than re-prepared per event, since EffectDb_RecordEvent can run once
// per self-effect line while capture is on.
//--------------------------------------------------------------------------------
void FinalizeAllStatements()
{
    sqlite3_finalize(s_insertEffectStmt);      s_insertEffectStmt      = nullptr;
    sqlite3_finalize(s_insertOccurrenceStmt);  s_insertOccurrenceStmt  = nullptr;
    sqlite3_finalize(s_insertGroupMemberStmt); s_insertGroupMemberStmt = nullptr;
    sqlite3_finalize(s_selectEffectStmt);      s_selectEffectStmt      = nullptr;
    sqlite3_finalize(s_selectKnownStmt);      s_selectKnownStmt      = nullptr;
    sqlite3_finalize(s_selectOccurrenceStmt); s_selectOccurrenceStmt = nullptr;
    sqlite3_finalize(s_selectGroupsStartedStmt);  s_selectGroupsStartedStmt  = nullptr;
    sqlite3_finalize(s_selectGroupsMemberOfStmt); s_selectGroupsMemberOfStmt = nullptr;
    sqlite3_finalize(s_updateNameStmt);       s_updateNameStmt       = nullptr;
    sqlite3_finalize(s_updateCategoryStmt);   s_updateCategoryStmt   = nullptr;
}

bool PrepareAllStatements(std::string& outError)
{
    static const char* kInsertEffect =
        "INSERT OR IGNORE INTO effects (guid_b64, name, block_group, block_member, type, category_path) "
        "VALUES (?1, ?2, ?3, ?4, ?5, '')";

    static const char* kInsertOccurrence =
        "INSERT OR IGNORE INTO occurrences "
        "(guid_b64, duration, a4, a6, self_mask, profession, race, specialization) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)";

    static const char* kInsertGroupMember =
        "INSERT OR IGNORE INTO group_members (starter_guid_b64, duration, a4, member_guid_b64) "
        "VALUES (?1, ?2, ?3, ?4)";

    static const char* kSelectEffect =
        "SELECT name, block_group, block_member, type, category_path FROM effects WHERE guid_b64 = ?1";

    static const char* kSelectKnown =
        "SELECT 1 FROM effects WHERE guid_b64 = ?1 LIMIT 1";

    static const char* kSelectOccurrence =
        "SELECT duration, a4, a6, self_mask, profession, race, specialization "
        "FROM occurrences WHERE guid_b64 = ?1";

    //_ Ordered by (duration, a4, member) rather than left to sqlite's
    //. natural row order -- callers rely on this for stable, deterministic
    //. iteration (see the header's doc comment on both functions).
    static const char* kSelectGroupsStarted =
        "SELECT duration, a4, member_guid_b64 FROM group_members "
        "WHERE starter_guid_b64 = ?1 ORDER BY duration, a4, member_guid_b64";

    //_ starter_guid_b64 != member_guid_b64: excludes this guid's own
    //. starter row, which GetGroupsStarted already covers -- see that
    //. function's own doc comment in the header.
    static const char* kSelectGroupsMemberOf =
        "SELECT starter_guid_b64, duration, a4 FROM group_members "
        "WHERE member_guid_b64 = ?1 AND starter_guid_b64 != ?1 "
        "ORDER BY starter_guid_b64, duration, a4";

    static const char* kUpdateName =
        "UPDATE effects SET name = ?1 WHERE guid_b64 = ?2";

    static const char* kUpdateCategory =
        "UPDATE effects SET category_path = ?1 WHERE guid_b64 = ?2";

    struct { const char* sql; sqlite3_stmt** out; } stmts[] = {
        { kInsertEffect,      &s_insertEffectStmt      },
        { kInsertOccurrence,  &s_insertOccurrenceStmt  },
        { kInsertGroupMember, &s_insertGroupMemberStmt },
        { kSelectEffect,      &s_selectEffectStmt      },
        { kSelectKnown,      &s_selectKnownStmt      },
        { kSelectOccurrence, &s_selectOccurrenceStmt },
        { kSelectGroupsStarted,  &s_selectGroupsStartedStmt  },
        { kSelectGroupsMemberOf, &s_selectGroupsMemberOfStmt },
        { kUpdateName,       &s_updateNameStmt       },
        { kUpdateCategory,   &s_updateCategoryStmt   },
    };

    for (auto& s : stmts)
    {
        if (sqlite3_prepare_v2(s_db, s.sql, -1, s.out, nullptr) != SQLITE_OK)
        {
            outError = sqlite3_errmsg(s_db);
            FinalizeAllStatements();
            return false;
        }
    }
    return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// CreateSchemaIfNeeded
//--------------------------------------------------------------------------------
// IF NOT EXISTS everywhere -- safe to call on every open, including
// against a db file that already has data from a prior session.
//--------------------------------------------------------------------------------
bool CreateSchemaIfNeeded(std::string& outError)
{
    static const char* kSchema =
        "CREATE TABLE IF NOT EXISTS effects ("
        "  guid_b64      TEXT PRIMARY KEY,"
        "  name          TEXT NOT NULL DEFAULT '',"
        "  block_group   TEXT NOT NULL DEFAULT '',"
        "  block_member  TEXT NOT NULL DEFAULT '',"
        "  type          INTEGER NOT NULL DEFAULT 0,"
        "  category_path TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS occurrences ("
        "  guid_b64       TEXT NOT NULL REFERENCES effects(guid_b64),"
        "  duration       INTEGER NOT NULL,"
        "  a4             INTEGER NOT NULL,"
        "  a6             TEXT NOT NULL,"
        "  self_mask      INTEGER NOT NULL,"
        "  profession     INTEGER NOT NULL,"
        "  race           INTEGER NOT NULL,"
        "  specialization INTEGER NOT NULL,"
        "  UNIQUE(guid_b64, duration, a4, a6, self_mask, profession, race, specialization)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_occurrences_guid ON occurrences(guid_b64);"
        "CREATE TABLE IF NOT EXISTS group_members ("
        "  starter_guid_b64 TEXT NOT NULL,"
        "  duration         INTEGER NOT NULL,"
        "  a4               INTEGER NOT NULL,"
        "  member_guid_b64  TEXT NOT NULL REFERENCES effects(guid_b64),"
        "  UNIQUE(starter_guid_b64, duration, a4, member_guid_b64)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_group_members_member ON group_members(member_guid_b64);"
        "CREATE INDEX IF NOT EXISTS idx_group_members_starter ON group_members(starter_guid_b64, duration, a4);";

    char* errMsg = nullptr;
    if (sqlite3_exec(s_db, kSchema, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        outError = errMsg ? errMsg : "unknown schema error";
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ConfigurePragmas
//--------------------------------------------------------------------------------
// The actual fix for the per-capture stutter (a different bug than the tree-
// rebuild stutter documented in EFFECT_DB_HANDOFF.md, and unrelated to it --
// that fix is still in place and should stay in place). SQLite's defaults are
// journal_mode=DELETE + synchronous=FULL, which means every autocommitted
// INSERT does a blocking fsync() (plus a journal file create/delete) before
// returning. EffectDb_RecordEvent runs on the render/update thread by this
// module's own contract (see file header), and used to issue two such
// autocommits per captured line (one for `effects`, one for `occurrences`) --
// two synchronous disk syncs on the render thread per event, which is exactly
// what a "stuck for a short moment" hitch during live capture looks like.
//
// WAL + synchronous=NORMAL removes the fsync-per-commit requirement (WAL
// commits are a sequential append; checkpointing back into the main db file
// happens later, off the hot path) without weakening the guarantee that
// actually matters here -- a crash can lose the last WAL-committed write, but
// can't corrupt the database, which is an acceptable trade for capture data.
// Paired with wrapping RecordEvent's two inserts in one explicit transaction
// (see below) so a single event is at most one commit instead of two.
//--------------------------------------------------------------------------------
bool ConfigurePragmas(std::string& outError)
{
    static const char* kPragmas =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;";

    char* errMsg = nullptr;
    if (sqlite3_exec(s_db, kPragmas, nullptr, nullptr, &errMsg) != SQLITE_OK)
    {
        outError = errMsg ? errMsg : "unknown pragma error";
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

} // namespace

void EffectDb_SetApi(AddonAPI_t* aApi)
{
    s_api = aApi;
}

bool EffectDb_Open(const std::string& denoiserAddonDir, std::string& outError)
{
    EffectDb_Close();

    std::string path = denoiserAddonDir + "/vfxd_effect_db.sqlite3";
    if (sqlite3_open(path.c_str(), &s_db) != SQLITE_OK)
    {
        outError = s_db ? sqlite3_errmsg(s_db) : "sqlite3_open failed";
        EffectDb_Close();
        return false;
    }

    if (!ConfigurePragmas(outError) || !CreateSchemaIfNeeded(outError) || !PrepareAllStatements(outError))
    {
        EffectDb_Close();
        return false;
    }

    return true;
}

void EffectDb_Close()
{
    FinalizeAllStatements();
    if (s_db) sqlite3_close(s_db);
    s_db = nullptr;
    s_enabled = false;
}

bool EffectDb_GreedFileExists(const std::string& denoiserAddonDir)
{
    std::error_code ec;
    return std::filesystem::exists(denoiserAddonDir + "/VfxD_Greed.json", ec);
}

bool EffectDb_SetEnabled(bool enabled, const std::string& denoiserAddonDir)
{
    if (!enabled)
    {
        s_enabled = false;
        return true;
    }

    if (!EffectDb_GreedFileExists(denoiserAddonDir))
        return false; //. hard gate -- caller's UI surfaces why, never auto-created

    if (!s_db)
    {
        std::string err;
        if (!EffectDb_Open(denoiserAddonDir, err))
        {
            LogFailure("EffectDb_SetEnabled: open failed: " + err);
            return false;
        }
    }

    s_enabled = true;
    s_lastPoll = std::chrono::steady_clock::now();
    return true;
}

bool EffectDb_IsEnabled()
{
    return s_enabled;
}

std::string EffectDb_Poll(const std::string& denoiserAddonDir)
{
    if (!s_enabled) return "";

    auto now = std::chrono::steady_clock::now();
    if (now - s_lastPoll < std::chrono::seconds(1))
        return "";
    s_lastPoll = now;

    if (!EffectDb_GreedFileExists(denoiserAddonDir))
    {
        s_enabled = false;
        return "VfxD_Greed.json is missing -- \"for science\" capture stopped.";
    }
    return "";
}

void EffectDb_RecordEvent(const EffectDbRawEvent& ev)
{
    if (!s_enabled || !s_db) return;

    //. Both inserts below as one explicit transaction rather than two
    //. autocommits -- at most one commit (and, under WAL, one cheap WAL
    //. append rather than a blocking fsync) per captured line instead of
    //. two. See ConfigurePragmas' doc comment for the full story.
    sqlite3_exec(s_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

    //. effects: first-seen-wins, INSERT OR IGNORE handles that for free
    sqlite3_reset(s_insertEffectStmt);
    sqlite3_clear_bindings(s_insertEffectStmt);
    sqlite3_bind_text(s_insertEffectStmt, 1, ev.guid_b64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s_insertEffectStmt, 2, ev.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s_insertEffectStmt, 3, ev.blockGroup.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s_insertEffectStmt, 4, ev.blockMember.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s_insertEffectStmt, 5, ev.type);
    if (sqlite3_step(s_insertEffectStmt) != SQLITE_DONE)
        LogFailure(std::string("EffectDb_RecordEvent: effect insert failed: ") + sqlite3_errmsg(s_db));
    else if (sqlite3_changes(s_db) > 0)
        ++s_generation;   //. a genuinely new guid -- the tree overlay needs to pick this up

    //. occurrences: UNIQUE constraint makes a repeat tuple a silent no-op
    sqlite3_reset(s_insertOccurrenceStmt);
    sqlite3_clear_bindings(s_insertOccurrenceStmt);
    sqlite3_bind_text(s_insertOccurrenceStmt, 1, ev.guid_b64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s_insertOccurrenceStmt, 2, ev.duration);
    sqlite3_bind_int(s_insertOccurrenceStmt, 3, static_cast<int>(ev.a4));
    sqlite3_bind_text(s_insertOccurrenceStmt, 4, ev.a6.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s_insertOccurrenceStmt, 5, static_cast<int>(ev.selfMask));
    sqlite3_bind_int(s_insertOccurrenceStmt, 6, static_cast<int>(static_cast<unsigned char>(ev.profession)));
    sqlite3_bind_int(s_insertOccurrenceStmt, 7, static_cast<int>(static_cast<unsigned char>(ev.race)));
    sqlite3_bind_int(s_insertOccurrenceStmt, 8, static_cast<int>(ev.specialization));
    if (sqlite3_step(s_insertOccurrenceStmt) != SQLITE_DONE)
        LogFailure(std::string("EffectDb_RecordEvent: occurrence insert failed: ") + sqlite3_errmsg(s_db));

    //. group_members: only when the caller resolved this event as part of
    //. a currently-open group (see EffectDbRawEvent::groupStarterGuid's
    //. doc comment on why this module never re-derives that itself).
    //. Same silent-no-op-on-repeat shape as occurrences, via UNIQUE.
    if (!ev.groupStarterGuid.empty())
    {
        sqlite3_reset(s_insertGroupMemberStmt);
        sqlite3_clear_bindings(s_insertGroupMemberStmt);
        sqlite3_bind_text(s_insertGroupMemberStmt, 1, ev.groupStarterGuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s_insertGroupMemberStmt, 2, ev.duration);
        sqlite3_bind_int(s_insertGroupMemberStmt, 3, static_cast<int>(ev.a4));
        sqlite3_bind_text(s_insertGroupMemberStmt, 4, ev.guid_b64.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s_insertGroupMemberStmt) != SQLITE_DONE)
            LogFailure(std::string("EffectDb_RecordEvent: group_members insert failed: ") + sqlite3_errmsg(s_db));
    }

    sqlite3_exec(s_db, "COMMIT;", nullptr, nullptr, nullptr);
}

bool EffectDb_IsKnownGuid(const std::string& guid_b64)
{
    if (!s_db) return false;

    sqlite3_reset(s_selectKnownStmt);
    sqlite3_clear_bindings(s_selectKnownStmt);
    sqlite3_bind_text(s_selectKnownStmt, 1, guid_b64.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(s_selectKnownStmt) == SQLITE_ROW;
}

bool EffectDb_GetEffect(const std::string& guid_b64, EffectDbEffect& out)
{
    if (!s_db) return false;

    sqlite3_reset(s_selectEffectStmt);
    sqlite3_clear_bindings(s_selectEffectStmt);
    sqlite3_bind_text(s_selectEffectStmt, 1, guid_b64.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(s_selectEffectStmt) != SQLITE_ROW)
        return false;

    out.guid_b64     = guid_b64;
    out.name         = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 0));
    out.blockGroup   = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 1));
    out.blockMember  = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 2));
    out.type         = sqlite3_column_int(s_selectEffectStmt, 3);
    out.categoryPath = SplitCategoryPath(reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 4)));
    return true;
}

std::vector<EffectDbEffect> EffectDb_GetAllEffects()
{
    std::vector<EffectDbEffect> out;
    if (!s_db) return out;

    sqlite3_stmt* stmt = nullptr;
    static const char* kSelectAll =
        "SELECT guid_b64, name, block_group, block_member, type, category_path FROM effects";

    if (sqlite3_prepare_v2(s_db, kSelectAll, -1, &stmt, nullptr) != SQLITE_OK)
        return out;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        EffectDbEffect e;
        e.guid_b64     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.name         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        e.blockGroup   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        e.blockMember  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        e.type         = sqlite3_column_int(stmt, 4);
        e.categoryPath = SplitCategoryPath(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        out.push_back(std::move(e));
    }

    sqlite3_finalize(stmt);
    return out;
}

std::vector<EffectDbOccurrence> EffectDb_GetOccurrences(const std::string& guid_b64)
{
    std::vector<EffectDbOccurrence> out;
    if (!s_db) return out;

    sqlite3_reset(s_selectOccurrenceStmt);
    sqlite3_clear_bindings(s_selectOccurrenceStmt);
    sqlite3_bind_text(s_selectOccurrenceStmt, 1, guid_b64.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(s_selectOccurrenceStmt) == SQLITE_ROW)
    {
        EffectDbOccurrence o;
        o.duration       = sqlite3_column_int(s_selectOccurrenceStmt, 0);
        o.a4             = static_cast<unsigned int>(sqlite3_column_int(s_selectOccurrenceStmt, 1));
        o.a6             = reinterpret_cast<const char*>(sqlite3_column_text(s_selectOccurrenceStmt, 2));
        o.self_mask      = static_cast<EffectDbSelfMask>(sqlite3_column_int(s_selectOccurrenceStmt, 3));
        o.profession     = static_cast<Mumble::EProfession>(sqlite3_column_int(s_selectOccurrenceStmt, 4));
        o.race           = static_cast<Mumble::ERace>(sqlite3_column_int(s_selectOccurrenceStmt, 5));
        o.specialization = static_cast<unsigned int>(sqlite3_column_int(s_selectOccurrenceStmt, 6));
        out.push_back(o);
    }
    return out;
}

std::vector<EffectDbGroupInstance> EffectDb_GetGroupsStarted(const std::string& guid_b64)
{
    std::vector<EffectDbGroupInstance> out;
    if (!s_db) return out;

    sqlite3_reset(s_selectGroupsStartedStmt);
    sqlite3_clear_bindings(s_selectGroupsStartedStmt);
    sqlite3_bind_text(s_selectGroupsStartedStmt, 1, guid_b64.c_str(), -1, SQLITE_TRANSIENT);

    //_ Rows arrive pre-sorted by (duration, a4, member) -- fold
    //. consecutive rows sharing (duration, a4) into one instance rather
    //. than a map, since the ORDER BY already guarantees they're
    //. contiguous.
    while (sqlite3_step(s_selectGroupsStartedStmt) == SQLITE_ROW)
    {
        int          duration = sqlite3_column_int(s_selectGroupsStartedStmt, 0);
        unsigned int a4       = static_cast<unsigned int>(sqlite3_column_int(s_selectGroupsStartedStmt, 1));
        std::string  member   = reinterpret_cast<const char*>(sqlite3_column_text(s_selectGroupsStartedStmt, 2));

        if (out.empty() || out.back().duration != duration || out.back().a4 != a4)
        {
            EffectDbGroupInstance inst;
            inst.duration = duration;
            inst.a4       = a4;
            out.push_back(std::move(inst));
        }
        out.back().memberGuids.push_back(std::move(member));
    }
    return out;
}

std::vector<EffectDbGroupMembership> EffectDb_GetGroupsMemberOf(const std::string& guid_b64)
{
    std::vector<EffectDbGroupMembership> out;
    if (!s_db) return out;

    sqlite3_reset(s_selectGroupsMemberOfStmt);
    sqlite3_clear_bindings(s_selectGroupsMemberOfStmt);
    sqlite3_bind_text(s_selectGroupsMemberOfStmt, 1, guid_b64.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(s_selectGroupsMemberOfStmt) == SQLITE_ROW)
    {
        EffectDbGroupMembership m;
        m.starterGuid_b64 = reinterpret_cast<const char*>(sqlite3_column_text(s_selectGroupsMemberOfStmt, 0));
        m.duration         = sqlite3_column_int(s_selectGroupsMemberOfStmt, 1);
        m.a4               = static_cast<unsigned int>(sqlite3_column_int(s_selectGroupsMemberOfStmt, 2));
        out.push_back(std::move(m));
    }
    return out;
}

bool EffectDb_SetName(const std::string& guid_b64, const std::string& name)
{
    if (!s_db || !EffectDb_IsKnownGuid(guid_b64)) return false;

    sqlite3_reset(s_updateNameStmt);
    sqlite3_clear_bindings(s_updateNameStmt);
    sqlite3_bind_text(s_updateNameStmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s_updateNameStmt, 2, guid_b64.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(s_updateNameStmt) != SQLITE_DONE)
    {
        LogFailure(std::string("EffectDb_SetName: update failed: ") + sqlite3_errmsg(s_db));
        return false;
    }
    bool changed = sqlite3_changes(s_db) > 0;
    if (changed) ++s_generation;
    return changed;
}

bool EffectDb_SetCategoryPath(const std::string& guid_b64, const std::vector<std::string>& categoryPath)
{
    if (!s_db || !EffectDb_IsKnownGuid(guid_b64)) return false;

    std::string joined = JoinCategoryPath(categoryPath);

    sqlite3_reset(s_updateCategoryStmt);
    sqlite3_clear_bindings(s_updateCategoryStmt);
    sqlite3_bind_text(s_updateCategoryStmt, 1, joined.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s_updateCategoryStmt, 2, guid_b64.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(s_updateCategoryStmt) != SQLITE_DONE)
    {
        LogFailure(std::string("EffectDb_SetCategoryPath: update failed: ") + sqlite3_errmsg(s_db));
        return false;
    }
    bool changed = sqlite3_changes(s_db) > 0;
    if (changed) ++s_generation;
    return changed;
}

int EffectDb_GetGeneration()
{
    return s_generation;
}