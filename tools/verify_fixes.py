"""
Runtime replica of the two effect_db.cpp / db_tree_view.cpp changes:

  1. EffectDb_RecordEvent's known-guid branch now backfills
     block_group/block_member/type on a row still sitting at the
     type=-1 placeholder (kBackfillCapture), guarded so it can never
     overwrite a real value afterward.
  2. BuildDbTree's "in_json" badge is now cross-referenced live against
     CollectInstalledGuids() instead of the stale effects.in_json column.

Same spirit as the harness TODO_B.md's item 2 already describes: exercise
the actual SQL against a real sqlite3 connection, not just eyeball the
C++.
"""
import sqlite3

SCHEMA = """
CREATE TABLE effects (
  guid_b64      TEXT PRIMARY KEY,
  effect_id     INTEGER NOT NULL,
  block_group   TEXT NOT NULL DEFAULT '',
  block_member  TEXT NOT NULL DEFAULT '',
  type          INTEGER NOT NULL DEFAULT -1,
  in_json       INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE effect_meta (
  effect_id         INTEGER PRIMARY KEY,
  name              TEXT NOT NULL DEFAULT '',
  category_path     TEXT NOT NULL DEFAULT '',
  description       TEXT NOT NULL DEFAULT '',
  behavior_type     TEXT NOT NULL DEFAULT '',
  behavior_caster   TEXT NOT NULL DEFAULT '',
  behavior_duration INTEGER
);
"""

BACKFILL_SQL = (
    "UPDATE effects SET block_group = ?, block_member = ?, type = ? "
    "WHERE guid_b64 = ? AND type = -1"
)


def lookup_effect_id(con, guid):
    row = con.execute("SELECT effect_id FROM effects WHERE guid_b64 = ?", (guid,)).fetchone()
    return row[0] if row else None


def record_event(con, guid, block_group, block_member, type_, name="Seeded"):
    """Mirrors EffectDb_RecordEvent's effects/effect_meta branch exactly."""
    existing = lookup_effect_id(con, guid)
    if existing is None:
        cur = con.execute("INSERT INTO effect_meta (effect_id, name) VALUES (NULL, ?)", (name,))
        new_id = cur.lastrowid
        con.execute(
            "INSERT INTO effects (guid_b64, effect_id, block_group, block_member, type) VALUES (?, ?, ?, ?, ?)",
            (guid, new_id, block_group, block_member, type_),
        )
        return "inserted"
    else:
        cur = con.execute(BACKFILL_SQL, (block_group, block_member, type_, guid))
        return "backfilled" if cur.rowcount > 0 else "no-op"


con = sqlite3.connect(":memory:")
con.executescript(SCHEMA)

# --- Test 1: brand-new guid still inserts with a real type, unaffected ---
result = record_event(con, "guidNEW==", "BlockA", "MemberA", 7, name="New Effect")
row = con.execute("SELECT block_group, block_member, type FROM effects WHERE guid_b64=?", ("guidNEW==",)).fetchone()
assert result == "inserted", result
assert row == ("BlockA", "MemberA", 7), row
print("[OK] brand-new guid: inserts real type directly ->", row)

# --- Test 2: externally-seeded placeholder (type=-1) gets backfilled on first real capture ---
con.execute("INSERT INTO effect_meta (effect_id, name) VALUES (99, 'Seeded Effect')")
con.execute("INSERT INTO effects (guid_b64, effect_id, block_group, block_member, type) VALUES (?, 99, '', '', -1)",
            ("guidSEEDED==",))

pre = con.execute("SELECT block_group, block_member, type FROM effects WHERE guid_b64=?", ("guidSEEDED==",)).fetchone()
assert pre == ("", "", -1), pre
print("[OK] seeded row starts at placeholder ->", pre)

result = record_event(con, "guidSEEDED==", "Foo", "Bar", 5)
post = con.execute("SELECT block_group, block_member, type FROM effects WHERE guid_b64=?", ("guidSEEDED==",)).fetchone()
assert result == "backfilled", result
assert post == ("Foo", "Bar", 5), post
print("[OK] first real capture of a seeded guid backfills placeholder ->", post)

# --- Test 3: first-seen-wins still holds -- a second, DIFFERENT sighting never overwrites ---
result2 = record_event(con, "guidSEEDED==", "Different", "Other", 99)
post2 = con.execute("SELECT block_group, block_member, type FROM effects WHERE guid_b64=?", ("guidSEEDED==",)).fetchone()
assert result2 == "no-op", result2
assert post2 == ("Foo", "Bar", 5), post2  # unchanged from Test 2
print("[OK] repeat capture after backfill is a true no-op (first-seen-wins preserved) ->", post2)

# --- Test 4: effect_meta.name from the seed is untouched by any of this (capture never re-seeds it) ---
name = con.execute("SELECT name FROM effect_meta WHERE effect_id=99").fetchone()[0]
assert name == "Seeded Effect", name
print("[OK] effect_meta.name from external seed untouched ->", name)

con.close()
print("\nAll type=-1 backfill checks passed.")


# ------------------------------------------------------------------
# in_json live cross-reference (BuildDbTree change)
# ------------------------------------------------------------------
def build_in_json_badge(member_guids, installed_guids):
    """Mirrors BuildDbTree's node["in_json"] any()-over-members logic,
    now against the live installed_guids set instead of a stale column."""
    return any(g in installed_guids for g in member_guids)


# Multi-guid effect: one sibling guid was added to JSON, the other wasn't.
# The stale `effects.in_json` column (set once at external-seed time) would
# show False for both, since JSON didn't exist yet when the db was seeded.
installed_guids_live = {"guidSibling2=="}   # what's actually in the loaded sin file right now
siblings = ["guidSibling1==", "guidSibling2=="]

badge_live = build_in_json_badge(siblings, installed_guids_live)
assert badge_live is True
print("[OK] in_json badge reflects a guid added to JSON after the db was seeded ->", badge_live)

# And the inverse: a guid that WAS in effects.in_json=1 at seed time, but has
# since been removed from every loaded sin file, now correctly reads False.
badge_after_removal = build_in_json_badge(["guidRemoved=="], set())
assert badge_after_removal is False
print("[OK] in_json badge reflects a guid removed from JSON since the db was seeded ->", badge_after_removal)

print("\nAll in_json live cross-reference checks passed.")
