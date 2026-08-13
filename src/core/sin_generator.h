//################################################################################
// sin_generator.h
//--------------------------------------------------------------------------------
// SinGenerator_Generate(sinName, version)   builds one sin's JSON from SQL
//--------------------------------------------------------------------------------
// Implements TODO_B.md item 7 ("SQL -> JSON generation"), design settled in
// EFFECT_DB_SOURCE_OF_TRUTH_HANDOFF.md's companion TODO. Reads the local
// effect db (effects/effect_meta/categories, all already open via
// EffectDb_EnsureOpenForBrowsing) and produces the same top-level
// {"version": N, "categories": [...]} shape a real VfxDenoiser sin file
// has -- Gluttony as a straight walk, Pride/Sloth with the same
// caster-normalization transform generate_sins.py already applies to the
// hand-maintained master (see HANDOFF_VfxSins.md, not in this repo).
//
// Deliberately test-only for now, per the handoff's "not decided yet"
// list: this writes to its own separate output path (see the addon.cpp
// call site), never into the real installed/managed sin files, and never
// touches github_update.cpp's diff/apply/version pipeline. Whether/how a
// future non-test path replaces generate_sins.py is explicitly left open.
//
// Ordering: walks effects in ascending effect_meta.sort_order and
// materializes each category on first reference in that same order --
// construction order IS the final order (no alphabetical re-sort the way
// db_tree_view.cpp's DB tab does; that sort is DB-tab-display-only and
// would defeat sort_order's whole purpose here). Only effects with a
// non-empty category_path are included, matching how "Uncategorized" is
// a DB-tab-only display convenience (see db_tree_view.cpp), not something
// that belongs in a generated file. Owner-confirmed assumptions baked in
// here, not re-derived: every real effect carries exactly one behavior,
// and no empty (zero-effect) category exists in the curated source data
// -- see TODO_B.md item 7 for both.
//--------------------------------------------------------------------------------

#pragma once

#include "../../include/nlohmann_json.hpp"

#include <string>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ESinGeneratorVariant
//--------------------------------------------------------------------------------
// Matches kSinNames (sin_files.h) exactly -- Gluttony is the unmodified
// walk; Pride/Sloth apply NormalizeBehaviorForVariant below. Kept as its
// own enum (rather than reusing a string) so a caller can't typo a
// variant name past compile time.
//--------------------------------------------------------------------------------
enum class ESinGeneratorVariant
{
    Gluttony,
    Pride,
    Sloth,
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SinGenerator_Generate
//--------------------------------------------------------------------------------
// Builds one sin's full file content from the currently-open effect db.
// major/minor are written verbatim into the output's top-level "version"
// key as {"version": {"major": ..., "minor": ...}} -- the same object
// shape VfxD itself writes at the top of every file it manages (see
// sin_files.h's ScanInstalledSinFiles doc, confirmed against a real
// VfxD_Greed.json: {"version": {"major": 1, "minor": 10}, ...}). This
// module has no opinion on what those numbers should be, same as
// generate_sins.py leaves it to its caller. Returns an empty object
// (not `null`) if the db isn't open or has no categorized effects, so a
// caller can always safely dump() the result; check EffectDb_GetAllEffects()
// separately first if "nothing to generate" needs its own message.
//--------------------------------------------------------------------------------
nlohmann::ordered_json SinGenerator_Generate(ESinGeneratorVariant variant, int major, int minor);
