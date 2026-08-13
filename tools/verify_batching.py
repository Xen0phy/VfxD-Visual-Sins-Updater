"""
Runtime replica of the batching change to EffectDb_RecordEvent /
EffectDb_FlushPendingWrites / EffectDb_Close, mirroring the exact
BEGIN-gated-by-flag / COMMIT-on-flush logic now in effect_db.cpp.

Same spirit as verify_fixes.py: exercise real sqlite3 semantics, not
just eyeball the C++.
"""
import sqlite3


class FakeEffectDb:
    """Mirrors s_db/s_txnOpen + RecordEvent/FlushPendingWrites/Close exactly."""

    def __init__(self):
        self.con = sqlite3.connect(":memory:")
        self.con.execute("CREATE TABLE occurrences (guid TEXT, duration INTEGER)")
        self.txn_open = False
        self.begin_count = 0
        self.commit_count = 0

    def record_event(self, guid, duration):
        if not self.txn_open:
            self.con.execute("BEGIN IMMEDIATE;")
            self.begin_count += 1
            self.txn_open = True
        self.con.execute("INSERT INTO occurrences (guid, duration) VALUES (?, ?)", (guid, duration))

    def flush_pending_writes(self):
        if not self.txn_open:
            return
        self.con.execute("COMMIT;")
        self.commit_count += 1
        self.txn_open = False

    def close(self, *, flush_first=True):
        if flush_first:
            self.flush_pending_writes()
        self.con.close()

    def visible_rows(self):
        return self.con.execute("SELECT guid, duration FROM occurrences ORDER BY guid").fetchall()


# --- Test 1: a same-frame burst of N events collapses into ONE commit ---
db = FakeEffectDb()
for i in range(30):
    db.record_event(f"guid{i:02d}==", i)

assert db.begin_count == 1, db.begin_count      # only the first event opened a transaction
assert db.commit_count == 0                     # nothing committed yet -- frame hasn't ended
assert len(db.visible_rows()) == 30              # same-connection read-your-own-writes, uncommitted
db.flush_pending_writes()
assert db.commit_count == 1, db.commit_count    # one commit for the whole 30-event burst
print(f"[OK] 30 same-frame events -> {db.begin_count} BEGIN, {db.commit_count} COMMIT (was 30/30 before this change)")

# --- Test 2: a second frame's burst opens exactly one more transaction ---
db.record_event("guid30==", 30)
db.record_event("guid31==", 31)
assert db.begin_count == 2, db.begin_count
db.flush_pending_writes()
assert db.commit_count == 2, db.commit_count
print(f"[OK] next frame's burst -> {db.begin_count} total BEGINs, {db.commit_count} total COMMITs across 2 frames")

# --- Test 3: no pending writes -> flush is a true no-op (no BEGIN/COMMIT issued at all) ---
begins_before, commits_before = db.begin_count, db.commit_count
db.flush_pending_writes()
db.flush_pending_writes()
assert db.begin_count == begins_before and db.commit_count == commits_before
print("[OK] flushing with nothing pending issues no SQL at all (cheap idle frames)")

db.con.close()


# --- Test 4: the bug this fix specifically had to avoid -- closing WITHOUT
# flushing first silently drops the last buffered batch (sqlite3_close
# implicitly rolls back an open transaction). This is why EffectDb_Close()
# now calls EffectDb_FlushPendingWrites() itself, unconditionally, first. ---
db2 = FakeEffectDb()
db2.record_event("guidLOST==", 99)
assert db2.txn_open is True
db2.close(flush_first=False)   #. simulates the OLD EffectDb_Close (no flush call)

reopened = sqlite3.connect(":memory:")  # can't reopen :memory:, so re-derive from a file to prove the point
import tempfile, os
path = tempfile.mktemp(suffix=".sqlite3")
try:
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE occurrences (guid TEXT, duration INTEGER)")
    con.execute("PRAGMA journal_mode=WAL;")
    con.execute("BEGIN IMMEDIATE;")
    con.execute("INSERT INTO occurrences (guid, duration) VALUES ('guidLOST==', 99)")
    con.close()   #. no COMMIT -- same as the old EffectDb_Close with no flush call

    con2 = sqlite3.connect(path)
    rows = con2.execute("SELECT * FROM occurrences").fetchall()
    con2.close()
    assert rows == [], rows   #. confirms: unflushed data really is gone after close
    print("[OK] confirmed: close-without-flush silently drops the pending batch "
          "(guidLOST== never made it to disk) -- exactly why EffectDb_Close() "
          "now flushes first, unconditionally, before FinalizeAllStatements/sqlite3_close")
finally:
    for ext in ("", "-wal", "-shm"):
        try:
            os.remove(path + ext)
        except OSError:
            pass

print("\nAll batching/flush checks passed.")
