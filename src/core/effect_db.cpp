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

//_ True while a transaction opened by EffectDb_RecordEvent hasn't been
// committed yet -- see EffectDb_FlushPendingWrites. Batches every
// RecordEvent call between two flushes into one WAL commit instead of
// one commit per event, so a burst of same-frame guids doesn't stall
// the render thread with N sequential BEGIN/COMMIT round-trips.
bool         s_txnOpen = false;

sqlite3_stmt* s_insertEffectStmt      = nullptr;
sqlite3_stmt* s_insertEffectMetaStmt  = nullptr;
sqlite3_stmt* s_insertOccurrenceStmt  = nullptr;
sqlite3_stmt* s_insertGroupMemberStmt = nullptr;
sqlite3_stmt* s_selectEffectStmt     = nullptr;
sqlite3_stmt* s_selectEffectIdStmt   = nullptr;  //. guid_b64 -> effect_id, see LookupEffectId
sqlite3_stmt* s_selectKnownStmt      = nullptr;
sqlite3_stmt* s_selectOccurrenceStmt = nullptr;
sqlite3_stmt* s_selectGroupsStartedStmt   = nullptr;
sqlite3_stmt* s_selectGroupsMemberOfStmt  = nullptr;
sqlite3_stmt* s_updateNameStmt       = nullptr;
sqlite3_stmt* s_updateCategoryStmt   = nullptr;
sqlite3_stmt* s_backfillCaptureStmt  = nullptr;  //. see kBackfillCapture

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
// LookupEffectId
//--------------------------------------------------------------------------------
// guid_b64 -> its effect_id, if guid_b64 is known at all. Shared by
// EffectDb_RecordEvent (branches new-effect-row vs. known-guid) and
// EffectDb_SetName/EffectDb_SetCategoryPath (resolve which effect_meta
// row a rename/placement actually applies to -- see effect_db.h's
// comments on both for why this is effect_id-scoped, not guid-scoped).
//--------------------------------------------------------------------------------
bool LookupEffectId(const std::string& guid_b64, int64_t& outEffectId)
{
    sqlite3_reset(s_selectEffectIdStmt);
    sqlite3_clear_bindings(s_selectEffectIdStmt);
    sqlite3_bind_text(s_selectEffectIdStmt, 1, guid_b64.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(s_selectEffectIdStmt) != SQLITE_ROW)
        return false;

    outEffectId = sqlite3_column_int64(s_selectEffectIdStmt, 0);
    return true;
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
    sqlite3_finalize(s_insertEffectMetaStmt);  s_insertEffectMetaStmt  = nullptr;
    sqlite3_finalize(s_insertOccurrenceStmt);  s_insertOccurrenceStmt  = nullptr;
    sqlite3_finalize(s_insertGroupMemberStmt); s_insertGroupMemberStmt = nullptr;
    sqlite3_finalize(s_selectEffectStmt);      s_selectEffectStmt      = nullptr;
    sqlite3_finalize(s_selectEffectIdStmt);   s_selectEffectIdStmt   = nullptr;
    sqlite3_finalize(s_selectKnownStmt);      s_selectKnownStmt      = nullptr;
    sqlite3_finalize(s_selectOccurrenceStmt); s_selectOccurrenceStmt = nullptr;
    sqlite3_finalize(s_selectGroupsStartedStmt);  s_selectGroupsStartedStmt  = nullptr;
    sqlite3_finalize(s_selectGroupsMemberOfStmt); s_selectGroupsMemberOfStmt = nullptr;
    sqlite3_finalize(s_updateNameStmt);       s_updateNameStmt       = nullptr;
    sqlite3_finalize(s_updateCategoryStmt);   s_updateCategoryStmt   = nullptr;
    sqlite3_finalize(s_backfillCaptureStmt);  s_backfillCaptureStmt  = nullptr;
}

bool PrepareAllStatements(std::string& outError)
{
    //_ effect_id is now bound, not left for SQLite to compute -- the
    // caller (EffectDb_RecordEvent) only ever reaches this insert after
    // already confirming via kSelectEffectId that guid_b64 is genuinely
    // new, so a plain INSERT (not OR IGNORE) is correct and lets a real
    // failure surface as a real failure rather than a silent no-op.
    static const char* kInsertEffect =
        "INSERT INTO effects (guid_b64, effect_id, block_group, block_member, type) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";

    //_ effect_id left NULL -- effect_meta.effect_id is INTEGER PRIMARY
    // KEY, so SQLite auto-assigns the next rowid; EffectDb_RecordEvent
    // reads it back via sqlite3_last_insert_rowid right after. Only ever
    // used for a guid effect_id never allocated to it before -- capture
    // can't know about sibling guids of the same effect (see the header's
    // file comment), so a brand-new guid always starts as its own
    // singleton effect_id/effect_meta row.
    //_ sort_order stamped as MAX(sort_order)+1 at insert time (TODO_B.md
    // item 7's "not-yet-curated capture can't jump ahead of real content"
    // rule) -- a brand-new capture-discovered guid was never in the
    // curated source Greed used to seed this db, so it has no natural
    // file position and sorts last if it's ever placed into a category.
    // COALESCE covers the very first row ever inserted (MAX over an empty
    // table is NULL).
    static const char* kInsertEffectMeta =
        "INSERT INTO effect_meta (effect_id, name, sort_order) "
        "VALUES (NULL, ?1, (SELECT COALESCE(MAX(sort_order), 0) + 1 FROM effect_meta))";

    static const char* kSelectEffectId =
        "SELECT effect_id FROM effects WHERE guid_b64 = ?1";

    //_ Upsert -- race and specialization are no longer part of the UNIQUE
    // key (see effect_db.h), so a repeat ORs their bits into the existing
    // row's masks. WHERE keeps an already-covered repeat a true no-op write.
    static const char* kInsertOccurrence =
        "INSERT INTO occurrences "
        "(guid_b64, duration, a4, a6, self_mask, race_mask, specialization_mask_lo, specialization_mask_hi) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(guid_b64, duration, a4, a6, self_mask) "
        "DO UPDATE SET race_mask = race_mask | excluded.race_mask, "
        "specialization_mask_lo = specialization_mask_lo | excluded.specialization_mask_lo, "
        "specialization_mask_hi = specialization_mask_hi | excluded.specialization_mask_hi "
        "WHERE (race_mask & excluded.race_mask) != excluded.race_mask "
        "   OR (specialization_mask_lo & excluded.specialization_mask_lo) != excluded.specialization_mask_lo "
        "   OR (specialization_mask_hi & excluded.specialization_mask_hi) != excluded.specialization_mask_hi";

    static const char* kInsertGroupMember =
        "INSERT OR IGNORE INTO group_members (starter_guid_b64, duration, a4, member_guid_b64) "
        "VALUES (?1, ?2, ?3, ?4)";

    static const char* kSelectEffect =
        "SELECT e.effect_id, e.block_group, e.block_member, e.type, e.in_json, "
        "       COALESCE(m.name, ''), COALESCE(m.category_path, ''), COALESCE(m.description, ''), "
        "       COALESCE(m.behavior_type, ''), COALESCE(m.behavior_caster, ''), m.behavior_duration, "
        "       COALESCE(m.sort_order, 0) "
        "FROM effects e LEFT JOIN effect_meta m ON m.effect_id = e.effect_id "
        "WHERE e.guid_b64 = ?1";

    static const char* kSelectKnown =
        "SELECT 1 FROM effects WHERE guid_b64 = ?1 LIMIT 1";

    static const char* kSelectOccurrence =
        "SELECT duration, a4, a6, self_mask, race_mask, specialization_mask_lo, specialization_mask_hi "
        "FROM occurrences WHERE guid_b64 = ?1";

    //_ Ordered by (duration, a4, member) rather than left to sqlite's
    // natural row order -- callers rely on this for stable, deterministic
    // iteration (see the header's doc comment on both functions).
    static const char* kSelectGroupsStarted =
        "SELECT duration, a4, member_guid_b64 FROM group_members "
        "WHERE starter_guid_b64 = ?1 ORDER BY duration, a4, member_guid_b64";

    //_ starter_guid_b64 != member_guid_b64: excludes this guid's own
    // starter row, which GetGroupsStarted already covers -- see that
    // function's own doc comment in the header.
    static const char* kSelectGroupsMemberOf =
        "SELECT starter_guid_b64, duration, a4 FROM group_members "
        "WHERE member_guid_b64 = ?1 AND starter_guid_b64 != ?1 "
        "ORDER BY starter_guid_b64, duration, a4";

    //_ Keyed by effect_id, not guid_b64 -- see effect_db.h's EffectDb_SetName
    // comment. Caller (EffectDb_SetName) resolves guid_b64 -> effect_id
    // via kSelectEffectId first and binds the effect_id here.
    static const char* kUpdateName =
        "UPDATE effect_meta SET name = ?1 WHERE effect_id = ?2";

    static const char* kUpdateCategory =
        "UPDATE effect_meta SET category_path = ?1 WHERE effect_id = ?2";

    //_ Backfills block_group/block_member/type on a guid whose `effects`
    // row already exists but was never actually captured -- i.e. an
    // externally-seeded row still sitting at the type=-1 placeholder
    // (see effect_db.h's EffectDbEffect::type comment and the handoff's
    // "type sentinel changes from 0 to -1" note). The `type = -1` guard
    // is what keeps this first-seen-wins: once a real capture has set a
    // real type, this becomes a permanent no-op for that guid, same as
    // the brand-new-guid path below never runs twice.
    static const char* kBackfillCapture =
        "UPDATE effects SET block_group = ?1, block_member = ?2, type = ?3 "
        "WHERE guid_b64 = ?4 AND type = -1";

    struct { const char* sql; sqlite3_stmt** out; } stmts[] = {
        { kInsertEffect,      &s_insertEffectStmt      },
        { kInsertEffectMeta,  &s_insertEffectMetaStmt  },
        { kInsertOccurrence,  &s_insertOccurrenceStmt  },
        { kInsertGroupMember, &s_insertGroupMemberStmt },
        { kSelectEffect,      &s_selectEffectStmt      },
        { kSelectEffectId,   &s_selectEffectIdStmt   },
        { kSelectKnown,      &s_selectKnownStmt      },
        { kSelectOccurrence, &s_selectOccurrenceStmt },
        { kSelectGroupsStarted,  &s_selectGroupsStartedStmt  },
        { kSelectGroupsMemberOf, &s_selectGroupsMemberOfStmt },
        { kUpdateName,       &s_updateNameStmt       },
        { kUpdateCategory,   &s_updateCategoryStmt   },
        { kBackfillCapture,  &s_backfillCaptureStmt  },
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
        "  effect_id     INTEGER NOT NULL,"
        "  block_group   TEXT NOT NULL DEFAULT '',"
        "  block_member  TEXT NOT NULL DEFAULT '',"
        "  type          INTEGER NOT NULL DEFAULT -1,"
        "  in_json       INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_effects_effect_id ON effects(effect_id);"
        "CREATE TABLE IF NOT EXISTS effect_meta ("
        "  effect_id         INTEGER PRIMARY KEY,"
        "  name              TEXT NOT NULL DEFAULT '',"
        "  category_path     TEXT NOT NULL DEFAULT '',"
        "  description       TEXT NOT NULL DEFAULT '',"
        "  behavior_type     TEXT NOT NULL DEFAULT '',"
        "  behavior_caster   TEXT NOT NULL DEFAULT '',"
        "  behavior_duration INTEGER,"
        "  sort_order        INTEGER NOT NULL DEFAULT 0"
        ");"
        //_ New per TODO_B.md item 7 -- categories aren't entities anywhere
        // else in this schema (category_path is just a string on each
        // effect_meta row), so generation has nowhere else to read a
        // category's own description/order from. Populated externally by
        // the seed script only, same as effect_meta -- this module never
        // writes a row here itself, see SinGenerator_Generate.
        "CREATE TABLE IF NOT EXISTS categories ("
        "  category_path TEXT PRIMARY KEY,"
        "  description   TEXT NOT NULL DEFAULT '',"
        "  sort_order    INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS occurrences ("
        "  guid_b64                 TEXT NOT NULL REFERENCES effects(guid_b64),"
        "  duration                 INTEGER NOT NULL,"
        "  a4                       INTEGER NOT NULL,"
        "  a6                       TEXT NOT NULL,"
        "  self_mask                INTEGER NOT NULL,"
        "  race_mask                INTEGER NOT NULL,"
        "  specialization_mask_lo   INTEGER NOT NULL,"
        "  specialization_mask_hi   INTEGER NOT NULL,"
        "  UNIQUE(guid_b64, duration, a4, a6, self_mask)"
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

    //_ CREATE TABLE IF NOT EXISTS is a no-op against a table that already
    // exists under the OLD (pre-effect_id) shape -- a db written by a
    // build before this change would silently keep its old `effects`
    // columns (no effect_id/in_json), and every prepared statement below
    // would then fail with a confusing "no such column" instead of a
    // clear message. Cheap direct check for the new column rather than a
    // migration -- per the handoff, there's nothing to migrate, a
    // mismatched file just needs to be replaced.
    char* checkErr = nullptr;
    if (sqlite3_exec(s_db, "SELECT effect_id, in_json FROM effects LIMIT 1;", nullptr, nullptr, &checkErr) != SQLITE_OK)
    {
        outError = "effect db at this path predates the effect_id schema (" +
                   std::string(checkErr ? checkErr : "unknown error") +
                   ") -- delete or replace vfxd_effect_db.sqlite3 with a freshly-seeded one";
        sqlite3_free(checkErr);
        return false;
    }

    //_ Same idea, for the sort_order column added by TODO_B.md item 7 --
    // a db seeded before that change has effect_meta but no sort_order,
    // and generation would otherwise fail deep inside SinGenerator_Generate
    // instead of here. Reseeding (not a migration) is still the fix, per
    // the handoff's "nothing to migrate" stance.
    char* sortOrderErr = nullptr;
    if (sqlite3_exec(s_db, "SELECT sort_order FROM effect_meta LIMIT 1;", nullptr, nullptr, &sortOrderErr) != SQLITE_OK)
    {
        outError = "effect db at this path predates the sort_order/categories schema (" +
                   std::string(sortOrderErr ? sortOrderErr : "unknown error") +
                   ") -- reseed vfxd_effect_db.sqlite3 with an up-to-date seed_effect_db.py";
        sqlite3_free(sortOrderErr);
        return false;
    }
    return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ConfigurePragmas
//--------------------------------------------------------------------------------
// SQLite's defaults are journal_mode=DELETE + synchronous=FULL, which means
// every autocommitted INSERT does a blocking fsync() (plus a journal file
// create/delete) before returning. EffectDb_RecordEvent runs on the
// render/update thread (see file header) and would issue two such autocommits
// per captured line (one for `effects`, one for `occurrences`) if left at
// SQLite's defaults -- two synchronous disk syncs on the render thread per event.
//
// WAL + synchronous=NORMAL removes the fsync-per-commit requirement (WAL
// commits are a sequential append; checkpointing back into the main db file
// happens later, off the hot path), at the cost of a crash losing the last
// WAL-committed write -- an acceptable trade for capture data, since it can't
// corrupt the database. Originally paired with wrapping RecordEvent's inserts
// in one explicit transaction per event; now paired with EffectDb_RecordEvent/
// EffectDb_FlushPendingWrites batching an entire frame's worth of events into
// one transaction instead -- see that pair's comments for why a per-event
// commit was still stalling the render thread under a same-frame burst even
// without the fsync.
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

} //. namespace

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
    EffectDb_FlushPendingWrites();   //. see that function's comment -- sqlite3_close would
                                      //. otherwise silently roll back an open transaction
    FinalizeAllStatements();
    if (s_db) sqlite3_close(s_db);
    s_db = nullptr;
    s_enabled = false;
}

bool EffectDb_EnsureOpenForBrowsing(const std::string& denoiserAddonDir, std::string& outError)
{
    if (s_db)
        return true;   //. already open, whether from here or EffectDb_SetEnabled -- see this function's own comment

    return EffectDb_Open(denoiserAddonDir, outError);
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

    //_ Both inserts below join whatever transaction is currently open,
    // rather than each event opening and committing its own -- see
    // EffectDb_FlushPendingWrites for where/when that transaction
    // actually commits, and s_txnOpen's comment for why. Only the FIRST
    // event since the last flush pays for a real BEGIN IMMEDIATE.
    if (!s_txnOpen)
    {
        sqlite3_exec(s_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
        s_txnOpen = true;
    }

    //_ effects/effect_meta: first-seen-wins, same spirit as the old
    // INSERT OR IGNORE but now a real branch instead of one statement,
    // since a brand-new guid needs a brand-new effect_id allocated (via
    // effect_meta's autoincrement) before its effects row can even be
    // built -- effect_id is NOT NULL there, so the two inserts can't
    // trade places. A guid already known (whether from an earlier real
    // capture or from an externally-seeded db -- see the handoff) never
    // reaches the insert branch below; the insert branch is genuinely
    // new-guid-only.
    int64_t existingEffectId = 0;
    if (!LookupEffectId(ev.guid_b64, existingEffectId))
    {
        sqlite3_reset(s_insertEffectMetaStmt);
        sqlite3_clear_bindings(s_insertEffectMetaStmt);
        sqlite3_bind_text(s_insertEffectMetaStmt, 1, ev.name.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(s_insertEffectMetaStmt) != SQLITE_DONE)
        {
            LogFailure(std::string("EffectDb_RecordEvent: effect_meta insert failed: ") + sqlite3_errmsg(s_db));
        }
        else
        {
            int64_t newEffectId = sqlite3_last_insert_rowid(s_db);

            sqlite3_reset(s_insertEffectStmt);
            sqlite3_clear_bindings(s_insertEffectStmt);
            sqlite3_bind_text(s_insertEffectStmt, 1, ev.guid_b64.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s_insertEffectStmt, 2, newEffectId);
            sqlite3_bind_text(s_insertEffectStmt, 3, ev.blockGroup.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s_insertEffectStmt, 4, ev.blockMember.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s_insertEffectStmt, 5, ev.type);

            if (sqlite3_step(s_insertEffectStmt) != SQLITE_DONE)
                LogFailure(std::string("EffectDb_RecordEvent: effect insert failed: ") + sqlite3_errmsg(s_db));
            else
                ++s_generation;   //. a genuinely new guid -- the tree overlay needs to pick this up
        }
    }
    else
    {
        //_ Guid already has an effects row -- either an earlier real
        // capture, or an externally-seeded placeholder still sitting at
        // type=-1 (see kBackfillCapture's comment). The WHERE type=-1
        // guard makes this a true no-op once a real type has ever been
        // written, so a placeholder's *first* live sighting fills it in
        // and every sighting after that is inert, matching the
        // first-seen-wins rule the rest of this table already follows.
        sqlite3_reset(s_backfillCaptureStmt);
        sqlite3_clear_bindings(s_backfillCaptureStmt);
        sqlite3_bind_text(s_backfillCaptureStmt, 1, ev.blockGroup.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s_backfillCaptureStmt, 2, ev.blockMember.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s_backfillCaptureStmt, 3, ev.type);
        sqlite3_bind_text(s_backfillCaptureStmt, 4, ev.guid_b64.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(s_backfillCaptureStmt) != SQLITE_DONE)
            LogFailure(std::string("EffectDb_RecordEvent: effect backfill failed: ") + sqlite3_errmsg(s_db));
        else if (sqlite3_changes(s_db) > 0)
            ++s_generation;   //. a placeholder guid just got its real type -- DB tab needs to pick this up
    }

    //_ occurrences: race and specialization are no longer part of the
    // UNIQUE key -- a repeat tuple upserts, OR'ing ev.race's bit and the
    // resolved EffectDb_SpecBit into the row's masks (see kInsertOccurrence).
    EffectDbSpecializationMask specBit = EffectDb_SpecBit(ev.profession, ev.specialization);

    sqlite3_reset(s_insertOccurrenceStmt);
    sqlite3_clear_bindings(s_insertOccurrenceStmt);
    sqlite3_bind_text(s_insertOccurrenceStmt, 1, ev.guid_b64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s_insertOccurrenceStmt, 2, ev.duration);
    sqlite3_bind_int(s_insertOccurrenceStmt, 3, static_cast<int>(ev.a4));
    sqlite3_bind_text(s_insertOccurrenceStmt, 4, ev.a6.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s_insertOccurrenceStmt, 5, static_cast<int>(ev.selfMask));
    sqlite3_bind_int64(s_insertOccurrenceStmt, 6, static_cast<sqlite3_int64>(EffectDb_RaceBit(ev.race)));
    sqlite3_bind_int64(s_insertOccurrenceStmt, 7, static_cast<sqlite3_int64>(specBit.lo));
    sqlite3_bind_int64(s_insertOccurrenceStmt, 8, static_cast<sqlite3_int64>(specBit.hi));
    if (sqlite3_step(s_insertOccurrenceStmt) != SQLITE_DONE)
        LogFailure(std::string("EffectDb_RecordEvent: occurrence insert failed: ") + sqlite3_errmsg(s_db));
    else if (sqlite3_changes(s_db) > 0)
        ++s_generation;   //. the DB tab's capture-data view needs to pick this up -- see EffectDb_GetGeneration

    //_ group_members: only when the caller resolved this event as part of
    // a currently-open group (see EffectDbRawEvent::groupStarterGuid's
    // doc comment on why this module never re-derives that itself).
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
        else if (sqlite3_changes(s_db) > 0)
            ++s_generation;
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_FlushPendingWrites
//--------------------------------------------------------------------------------
// Commits the transaction EffectDb_RecordEvent left open, if any; a cheap
// no-op otherwise (a single boolean check). Meant to be called once per
// real frame -- see entry.cpp's RT_PostRender registration -- so a burst
// of same-frame RecordEvent calls collapses into a single WAL commit
// instead of one per event. Also called from the top of EffectDb_Close():
// sqlite3_close() implicitly rolls back an open transaction rather than
// committing it, so skipping this there would silently drop the last
// unflushed batch instead of writing it. Widens the existing "a crash
// can lose the last WAL-committed write" trade-off documented in
// ConfigurePragmas to "up to one frame's worth of writes" -- still can't
// corrupt the db, only lose a sliver of recent capture data.
//--------------------------------------------------------------------------------
void EffectDb_FlushPendingWrites()
{
    if (!s_db || !s_txnOpen)
        return;

    sqlite3_exec(s_db, "COMMIT;", nullptr, nullptr, nullptr);
    s_txnOpen = false;
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

    out.guid_b64         = guid_b64;
    out.effect_id         = sqlite3_column_int64(s_selectEffectStmt, 0);
    out.blockGroup        = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 1));
    out.blockMember        = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 2));
    out.type               = sqlite3_column_int(s_selectEffectStmt, 3);
    out.in_json             = sqlite3_column_int(s_selectEffectStmt, 4) != 0;
    out.name               = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 5));
    out.categoryPath       = SplitCategoryPath(reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 6)));
    out.description         = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 7));
    out.behaviorType       = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 8));
    out.behaviorCaster     = reinterpret_cast<const char*>(sqlite3_column_text(s_selectEffectStmt, 9));
    out.behaviorDuration   = sqlite3_column_int(s_selectEffectStmt, 10);  //. NULL -> 0, only meaningful when behaviorType == "SetDuration"
    out.sortOrder           = sqlite3_column_int(s_selectEffectStmt, 11);
    return true;
}

std::vector<EffectDbEffect> EffectDb_GetAllEffects()
{
    std::vector<EffectDbEffect> out;
    if (!s_db) return out;

    sqlite3_stmt* stmt = nullptr;
    //_ Same column set/order as kSelectEffect, plus guid_b64 up front --
    // see that statement's comment for why the join is LEFT and every
    // effect_meta column is COALESCE'd.
    static const char* kSelectAll =
        "SELECT e.guid_b64, e.effect_id, e.block_group, e.block_member, e.type, e.in_json, "
        "       COALESCE(m.name, ''), COALESCE(m.category_path, ''), COALESCE(m.description, ''), "
        "       COALESCE(m.behavior_type, ''), COALESCE(m.behavior_caster, ''), m.behavior_duration, "
        "       COALESCE(m.sort_order, 0) "
        "FROM effects e LEFT JOIN effect_meta m ON m.effect_id = e.effect_id";

    if (sqlite3_prepare_v2(s_db, kSelectAll, -1, &stmt, nullptr) != SQLITE_OK)
        return out;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        EffectDbEffect e;
        e.guid_b64         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.effect_id         = sqlite3_column_int64(stmt, 1);
        e.blockGroup        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        e.blockMember        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        e.type               = sqlite3_column_int(stmt, 4);
        e.in_json             = sqlite3_column_int(stmt, 5) != 0;
        e.name               = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        e.categoryPath       = SplitCategoryPath(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
        e.description         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        e.behaviorType       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        e.behaviorCaster     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        e.behaviorDuration   = sqlite3_column_int(stmt, 11);
        e.sortOrder           = sqlite3_column_int(stmt, 12);
        out.push_back(std::move(e));
    }

    sqlite3_finalize(stmt);
    return out;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// EffectDb_GetAllCategories
//--------------------------------------------------------------------------------
// Full `categories` table -- externally seeded only (see the schema
// comment in CreateSchemaIfNeeded), read here purely so SinGenerator can
// look up a category's own description/sort_order when it materializes
// that category node, the same way BuildDbTree looks up effect_meta rows.
// Not on the hot path (called once per Generate click, not per frame), so
// an ad-hoc prepare/step/finalize here is fine -- same reasoning as
// EffectDb_GetAllEffects just above.
//--------------------------------------------------------------------------------
std::vector<EffectDbCategory> EffectDb_GetAllCategories()
{
    std::vector<EffectDbCategory> out;
    if (!s_db) return out;

    sqlite3_stmt* stmt = nullptr;
    static const char* kSelectAllCategories =
        "SELECT category_path, description, sort_order FROM categories";

    if (sqlite3_prepare_v2(s_db, kSelectAllCategories, -1, &stmt, nullptr) != SQLITE_OK)
        return out;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        EffectDbCategory c;
        c.categoryPath = SplitCategoryPath(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        c.description  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        c.sortOrder     = sqlite3_column_int(stmt, 2);
        out.push_back(std::move(c));
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
        o.duration                 = sqlite3_column_int(s_selectOccurrenceStmt, 0);
        o.a4                       = static_cast<unsigned int>(sqlite3_column_int(s_selectOccurrenceStmt, 1));
        o.a6                       = reinterpret_cast<const char*>(sqlite3_column_text(s_selectOccurrenceStmt, 2));
        o.self_mask                = static_cast<EffectDbSelfMask>(sqlite3_column_int(s_selectOccurrenceStmt, 3));
        o.raceMask                 = static_cast<EffectDbRaceMask>(sqlite3_column_int64(s_selectOccurrenceStmt, 4));
        o.specializationMask.lo    = static_cast<uint64_t>(sqlite3_column_int64(s_selectOccurrenceStmt, 5));
        o.specializationMask.hi    = static_cast<uint64_t>(sqlite3_column_int64(s_selectOccurrenceStmt, 6));
        out.push_back(o);
    }
    return out;
}

std::vector<Mumble::ERace> EffectDb_RacesInMask(EffectDbRaceMask mask)
{
    std::vector<Mumble::ERace> out;
    for (unsigned bit = 0; bit < sizeof(EffectDbRaceMask) * 8; ++bit)
        if (mask & (EffectDbRaceMask{1} << bit))
            out.push_back(static_cast<Mumble::ERace>(bit));
    return out;
}

std::vector<unsigned int> EffectDb_SpecOrCoreIdsInMask(const EffectDbSpecializationMask& mask)
{
    std::vector<unsigned int> out;
    for (unsigned bit = 0; bit < 64; ++bit)
        if (mask.lo & (uint64_t{1} << bit))
            out.push_back(bit);
    for (unsigned bit = 0; bit < 64; ++bit)
        if (mask.hi & (uint64_t{1} << bit))
            out.push_back(bit + 64);
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
    // consecutive rows sharing (duration, a4) into one instance rather
    // than a map, since the ORDER BY already guarantees they're contiguous.
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
    if (!s_db) return false;

    int64_t effectId = 0;
    if (!LookupEffectId(guid_b64, effectId))
        return false;

    sqlite3_reset(s_updateNameStmt);
    sqlite3_clear_bindings(s_updateNameStmt);
    sqlite3_bind_text(s_updateNameStmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s_updateNameStmt, 2, effectId);

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
    if (!s_db) return false;

    int64_t effectId = 0;
    if (!LookupEffectId(guid_b64, effectId))
        return false;

    std::string joined = JoinCategoryPath(categoryPath);

    sqlite3_reset(s_updateCategoryStmt);
    sqlite3_clear_bindings(s_updateCategoryStmt);
    sqlite3_bind_text(s_updateCategoryStmt, 1, joined.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s_updateCategoryStmt, 2, effectId);

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