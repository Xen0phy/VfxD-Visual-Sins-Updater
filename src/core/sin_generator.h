//################################################################################
// sin_generator.h
//--------------------------------------------------------------------------------
// Reads the local effect db (effects/effect_meta/categories, all already open
// via EffectDb_EnsureOpenForBrowsing) and produces the same top-level
// {"version": N, "categories": [...]} shape a real VfxDenoiser sin file has --
// Gluttony as a straight walk, Pride/Sloth with the same caster-normalization
// transform generate_sins.py already applies to the hand-maintained master.
//
// Writes to its own separate output path (see the addon.cpp call site), never
// into the real installed/managed sin files, and never touches
// github_update.cpp's diff/apply/version pipeline.
//
// Ordering: walks effects in ascending effect_meta.sort_order and materializes
// each category on first reference in that same order -- construction order IS
// the final order (no alphabetical re-sort the way db_tree_view.cpp's DB tab
// does; that sort is DB-tab-display-only and would defeat sort_order's whole
// purpose here). Only effects with a non-empty category_path are included,
// matching how "Uncategorized" is a DB-tab-only display convenience (see
// db_tree_view.cpp), not something that belongs in a generated file. Two
// assumptions are baked in here, not re-derived: every real effect carries
// exactly one behavior, and no empty (zero-effect) category exists in the
// curated source data.
//--------------------------------------------------------------------------------

#pragma once

#include "../../include/nlohmann_json.hpp"

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ESinGeneratorVariant
//--------------------------------------------------------------------------------
// Matches kSinNames (sin_files.h) exactly -- Gluttony is the unmodified walk;
// Pride/Sloth apply NormalizeCaster below. Kept as its own enum instead of
// reusing a string so a caller can't typo a variant name past compile time.
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
// major/minor are written verbatim into the output's top-level "version":
// {"major", "minor"} key, matching the shape VfxD itself writes (see
// sin_files.h's ScanInstalledSinFiles doc). This module has no opinion on what
// those numbers should be, same as generate_sins.py leaves it to its caller.
// Returns an empty object (not `null`) if the db isn't open or has no
// categorized effects, so a caller can always safely dump() the result; check
// EffectDb_GetAllEffects() separately first if "nothing to generate" needs its
// own message.
//--------------------------------------------------------------------------------
nlohmann::ordered_json SinGenerator_Generate(ESinGeneratorVariant variant, int major, int minor);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SinGenerator_CountEmittedGuids
//--------------------------------------------------------------------------------
// The guid count SinGenerator_Generate(variant, ...) would actually emit for
// that sin: effects is one row per guid_b64 (see effect_db.h), so counting rows
// surviving this variant's filtering (non-empty categoryPath; for Sloth, also
// not under a top-level "Caution" category) is exactly that count, without
// building the full json tree. Not the same as a raw `SELECT COUNT(*) FROM
// effects`/allEffects.size(): rows failing this filter are counted by that but
// excluded from generation. This is the number sql_update.h's
// update-availability check compares against a sin's installed filename
// version.
//--------------------------------------------------------------------------------
int SinGenerator_CountEmittedGuids(ESinGeneratorVariant variant);