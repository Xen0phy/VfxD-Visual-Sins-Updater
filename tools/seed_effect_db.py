#!/usr/bin/env python3
"""
seed_effect_db.py -- turn a VfxD installed-effects JSON file (e.g.
VfxD_Greed.json) into a fresh SQLite db matching the new effect_id/
effect_meta schema described in EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md.

This is the "external tool" that handoff explicitly says is NOT the
addon's own job -- the addon reads an already-populated db, it doesn't
build one from JSON itself. This script is that one-time bootstrap.

Usage:
    python3 seed_effect_db.py VfxD_Greed.json -o effect_db.sqlite3
    python3 seed_effect_db.py VfxD_Greed.json --dry-run

Schema written (matches the handoff exactly):

    effects (
      guid_b64      TEXT PRIMARY KEY,
      effect_id     INTEGER NOT NULL,
      block_group   TEXT NOT NULL DEFAULT '',
      block_member  TEXT NOT NULL DEFAULT '',
      type          INTEGER NOT NULL DEFAULT -1,
      in_json       INTEGER NOT NULL DEFAULT 0
    )

    effect_meta (
      effect_id     INTEGER PRIMARY KEY,
      name          TEXT NOT NULL DEFAULT '',
      category_path TEXT NOT NULL DEFAULT '',   -- \x1f-joined, same as effect_db.cpp
      description   TEXT NOT NULL DEFAULT '',
      behavior_type     TEXT NOT NULL DEFAULT '',
      behavior_caster   TEXT NOT NULL DEFAULT '',
      behavior_duration INTEGER
    )

    occurrences / group_members -- created empty, schema only. Nothing in
    a JSON file can populate these; they only ever get real rows from live
    capture, once the addon itself is pointed at this db.

Field mapping, JSON -> SQL:
    effect["name"]                 -> effect_meta.name
    effect["description"]          -> effect_meta.description (default '')
    category path walked to reach
    the effect                     -> effect_meta.category_path (\x1f-joined)
    effect["guids"]                -> one effects row per guid, all sharing
                                       one new effect_id
    effect["behaviors"]            -> effect_meta.behavior_type/caster/duration
                                       ONLY when there's exactly one entry
                                       (SQL holds a single default, never a
                                       list -- see handoff). Zero entries ->
                                       left blank. More than one entry ->
                                       left blank AND reported, since picking
                                       one arbitrarily would misrepresent
                                       what's actually installed; needs a
                                       human to resolve which one is "the"
                                       default, if any.

effects.block_group / block_member / type are never in JSON (confirmed:
real installed effect objects only ever have name/guids/behaviors/
description) -- left at their capture-pending defaults ('', '', -1).
in_json is set to 1 for every row this script creates, since by
definition every guid it sees came from JSON; the addon recomputes this
itself on its own next load regardless (see handoff), so this is just a
correct starting value, not load-bearing.

Skips (matching merge.h's own case-0 convention): an effect with no guids
at all is ignored entirely -- nothing to key an identity on.

Hard-errors on a guid seen under more than one effect object -- guid_b64
is meant to be globally unique; a real duplicate is a data problem in the
source JSON, not something this script should silently paper over.
"""

import argparse
import json
import sqlite3
import sys
from pathlib import Path

CATEGORY_DELIM = "\x1f"  # matches effect_db.cpp's kCategoryDelim exactly

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
  effect_id     INTEGER PRIMARY KEY,
  name          TEXT NOT NULL DEFAULT '',
  category_path TEXT NOT NULL DEFAULT '',
  description   TEXT NOT NULL DEFAULT '',
  behavior_type     TEXT NOT NULL DEFAULT '',
  behavior_caster   TEXT NOT NULL DEFAULT '',
  behavior_duration INTEGER,
  sort_order        INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE categories (
  category_path TEXT PRIMARY KEY,
  description   TEXT NOT NULL DEFAULT '',
  sort_order    INTEGER NOT NULL
);

CREATE TABLE occurrences (
  guid_b64                 TEXT NOT NULL REFERENCES effects(guid_b64),
  duration                 INTEGER NOT NULL,
  a4                       INTEGER NOT NULL,
  a6                       TEXT NOT NULL,
  self_mask                INTEGER NOT NULL,
  race_mask                INTEGER NOT NULL,
  specialization_mask_lo   INTEGER NOT NULL,
  specialization_mask_hi   INTEGER NOT NULL,
  UNIQUE(guid_b64, duration, a4, a6, self_mask)
);
CREATE INDEX idx_occurrences_guid ON occurrences(guid_b64);

CREATE TABLE group_members (
  starter_guid_b64 TEXT NOT NULL,
  duration         INTEGER NOT NULL,
  a4               INTEGER NOT NULL,
  member_guid_b64  TEXT NOT NULL REFERENCES effects(guid_b64),
  UNIQUE(starter_guid_b64, duration, a4, member_guid_b64)
);
CREATE INDEX idx_group_members_member ON group_members(member_guid_b64);
CREATE INDEX idx_group_members_starter ON group_members(starter_guid_b64, duration, a4);
"""


class Effect:
    __slots__ = ("name", "description", "category_path", "guids", "behaviors")

    def __init__(self, name, description, category_path, guids, behaviors):
        self.name = name
        self.description = description
        self.category_path = category_path  # list[str]
        self.guids = guids  # list[str]
        self.behaviors = behaviors  # list[dict]


def walk_categories(node, path_so_far, out_effects, out_categories, seen_categories, warnings):
    """Recurse the same shape IndexDiffCategory/RenderCategoryTree walk --
    push this category's own name, recurse into subcategories, then walk
    this category's own effects with the now-complete path.

    out_categories collects (path_tuple, description) in first-reference
    DFS order -- the same "materialize on first reference" order
    sin_generator.cpp's FindOrCreateCategory uses when it later walks SQL
    rows back out in sort_order, so a category's sort_order here has to
    be assigned at the moment its path is first seen, not at the moment
    an effect happens to land in it (a category can be entered via a
    subcategory before it ever gets a direct effect)."""
    name = node.get("name")
    if name is not None:
        path_so_far = path_so_far + [name]
        path_tuple = tuple(path_so_far)
        if path_tuple not in seen_categories:
            seen_categories.add(path_tuple)
            out_categories.append((path_tuple, node.get("description", "") or ""))

    for sub in node.get("categories", []) or []:
        walk_categories(sub, path_so_far, out_effects, out_categories, seen_categories, warnings)

    for eff in node.get("effects", []) or []:
        guids = eff.get("guids") or []
        if not guids:
            # Matches merge.h's own case-0 convention: no guids, no
            # identity to key on, never shown/tracked anywhere.
            continue

        behaviors = eff.get("behaviors") or []
        out_effects.append(Effect(
            name=eff.get("name", ""),
            description=eff.get("description", "") or "",
            category_path=path_so_far,
            guids=guids,
            behaviors=behaviors,
        ))


def build_effects(root_categories):
    out_effects = []
    out_categories = []
    seen_categories = set()
    warnings = []
    for cat in root_categories:
        walk_categories(cat, [], out_effects, out_categories, seen_categories, warnings)
    return out_effects, out_categories, warnings


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input_json", type=Path, help="e.g. VfxD_Greed.json")
    ap.add_argument("-o", "--output", type=Path, default=None,
                     help="output sqlite path (default: <input stem>.sqlite3)")
    ap.add_argument("--dry-run", action="store_true",
                     help="parse and report, but don't write a db file")
    ap.add_argument("--force", action="store_true",
                     help="overwrite the output file if it already exists")
    args = ap.parse_args()

    if args.output is None:
        args.output = args.input_json.with_suffix(".sqlite3")

    if not args.dry_run and args.output.exists() and not args.force:
        print(f"error: {args.output} already exists (use --force to overwrite)", file=sys.stderr)
        sys.exit(1)

    with open(args.input_json, "r", encoding="utf-8") as f:
        data = json.load(f)

    if "categories" not in data or not isinstance(data["categories"], list):
        print("error: input JSON has no top-level \"categories\" array -- is this a VfxD file?", file=sys.stderr)
        sys.exit(1)

    effects, categories, walk_warnings = build_effects(data["categories"])

    # ---- integrity check: a guid must belong to exactly one effect ----
    guid_owner = {}
    dup_warnings = []
    for eff in effects:
        for g in eff.guids:
            if g in guid_owner:
                dup_warnings.append(
                    f'guid {g!r} claimed by both "{guid_owner[g].name}" '
                    f'and "{eff.name}" -- SKIPPING "{eff.name}" entirely'
                )
            else:
                guid_owner[g] = eff

    skip_effects = set()
    if dup_warnings:
        # Re-scan to find every effect touched by a duplicate and skip all
        # of them, rather than guessing which one is "right".
        seen_once = set()
        for eff in effects:
            for g in eff.guids:
                if g in seen_once:
                    skip_effects.add(id(eff))
                    for other in effects:
                        if g in other.guids:
                            skip_effects.add(id(other))
                seen_once.add(g)

    # ---- behavior-collapse check ----
    multi_behavior_warnings = []
    for eff in effects:
        if id(eff) in skip_effects:
            continue
        if len(eff.behaviors) > 1:
            multi_behavior_warnings.append(
                f'"{eff.name}" has {len(eff.behaviors)} behaviors set '
                f"in JSON -- SQL holds a single default, leaving behavior "
                f"columns blank for this effect (needs a human pick)"
            )

    kept_effects = [e for e in effects if id(e) not in skip_effects]

    # ---- report ----
    print(f"Parsed {args.input_json}")
    print(f"  effects found (with >=1 guid): {len(effects)}")
    print(f"  effects skipped (duplicate guid across effects): {len(skip_effects)}")
    print(f"  effects to write: {len(kept_effects)}")
    total_guids = sum(len(e.guids) for e in kept_effects)
    print(f"  total guid rows to write: {total_guids}")
    print(f"  categories found (first-reference order): {len(categories)}")
    with_cat_desc = sum(1 for _, d in categories if d)
    print(f"  categories with a description: {with_cat_desc}")
    with_desc = sum(1 for e in kept_effects if e.description)
    print(f"  effects with a description: {with_desc}")
    with_one_behavior = sum(1 for e in kept_effects if len(e.behaviors) == 1)
    print(f"  effects with exactly 1 behavior (seeded into SQL): {with_one_behavior}")
    print(f"  effects with 0 behaviors (left blank): "
          f"{sum(1 for e in kept_effects if len(e.behaviors) == 0)}")
    print(f"  effects with >1 behavior (left blank, see warnings): {len(multi_behavior_warnings)}")

    all_warnings = dup_warnings + multi_behavior_warnings
    if all_warnings:
        print(f"\n{len(all_warnings)} warning(s):")
        for w in all_warnings:
            print(f"  - {w}")

    if args.dry_run:
        print("\n--dry-run: no db written.")
        return

    if args.output.exists() and args.force:
        args.output.unlink()

    conn = sqlite3.connect(str(args.output))
    conn.executescript(SCHEMA)

    for order, (path_tuple, description) in enumerate(categories):
        conn.execute(
            "INSERT INTO categories (category_path, description, sort_order) VALUES (?, ?, ?)",
            (CATEGORY_DELIM.join(path_tuple), description, order),
        )

    next_effect_id = 1
    for sort_order, eff in enumerate(kept_effects):
        effect_id = next_effect_id
        next_effect_id += 1

        category_path = CATEGORY_DELIM.join(eff.category_path)

        if len(eff.behaviors) == 1:
            b = eff.behaviors[0]
            behavior_type = b.get("type", "") or ""
            behavior_caster = b.get("caster", "") or ""
            behavior_duration = b.get("duration")
        else:
            behavior_type = ""
            behavior_caster = ""
            behavior_duration = None

        conn.execute(
            "INSERT INTO effect_meta (effect_id, name, category_path, description, "
            "behavior_type, behavior_caster, behavior_duration, sort_order) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (effect_id, eff.name, category_path, eff.description,
             behavior_type, behavior_caster, behavior_duration, sort_order),
        )

        for g in eff.guids:
            conn.execute(
                "INSERT INTO effects (guid_b64, effect_id, block_group, block_member, type, in_json) "
                "VALUES (?, ?, '', '', -1, 1)",
                (g, effect_id),
            )

    conn.commit()
    conn.close()

    print(f"\nWrote {args.output} "
          f"({len(kept_effects)} effects, {total_guids} guid rows, {len(categories)} categories).")


if __name__ == "__main__":
    main()
