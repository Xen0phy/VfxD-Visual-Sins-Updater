//################################################################################
// sin_files.h
//--------------------------------------------------------------------------------
// The VfxD "Visual Sins" effect files this addon can find on disk.
// kSinNames/kSinCount is NOT that whole set -- it's the smaller subset
// that also gets GitHub-hosted updates (see its own comment below).
// ScanInstalledSinFiles itself discovers any VfxD_<Name>.json on disk,
// update-tracked or not -- plus, now, any other .json file in the same
// directory whose content looks like a VfxD file (see its own comment).
//--------------------------------------------------------------------------------

#pragma once

#include <string>
#include <vector>

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// kSinNames / kSinCount
//--------------------------------------------------------------------------------
// Only the sins with a matching GitHub release asset -- consumed by
// github_update.cpp's per-release check loop and addon.cpp's install/
// check/apply action row. Deliberately separate from whatever
// ScanInstalledSinFiles finds on disk: a hand-edited-only file is a
// perfectly normal installed sin that just never appears here. Keep in
// sync with github_update.cpp's asset-name matching, NOT with
// ScanInstalledSinFiles, which no longer hardcodes this list.
//--------------------------------------------------------------------------------
inline const char* const kSinNames[] = { "Gluttony", "Pride", "Sloth" };
inline constexpr int kSinCount = 3;

//********************************************************************************
// InstalledSinFile
//--------------------------------------------------------------------------------
// sinName    e.g. "Gluttony", or "Greed" for a hand-edited-only file --
//            whatever ScanInstalledSinFiles's own pattern captured, not
//            necessarily one of kSinNames. For a file that doesn't
//            follow the VfxD_<Name> convention at all, this is just its
//            filename stem (extension and any trailing -v<N>/_v<N>
//            stripped) -- see ScanInstalledSinFiles.
// fullPath   absolute path to the file on disk
// fileName   just the filename, e.g. "VfxD_Gluttony-v3883.json"
// version    -1 = no version suffix ("VfxD_Gluttony.json"), always
//            treated as older than any real version -- also simply the
//            permanent state of a hand-edited-only file that never
//            takes a version suffix at all
//--------------------------------------------------------------------------------
struct InstalledSinFile
{
    std::string sinName;
    std::string fullPath;
    std::string fileName;
    int         version = -1;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ScanInstalledSinFiles
//--------------------------------------------------------------------------------
// Scans denoiserAddonDir (e.g. "<GW2>/addons/VfxDenoiser") for two kinds
// of file:
//
//   1. VfxD_<Name>.json / VfxD_<Name>_v<N>.json / VfxD_<Name>-v<N>.json --
//      both separators are accepted since the addon isn't consistent
//      about which one it uses. <Name> is whatever's actually there, NOT
//      limited to kSinNames -- this is what makes a hand-edited-only file
//      (e.g. "VfxD_Greed.json", no version suffix, no GitHub release
//      backing it) show up for editing/backup/live-log/report purposes
//      right alongside the three update-tracked sins, with zero code
//      changes needed to add one. Matched by filename alone, no need to
//      open the file.
//
//   2. Any other *.json file in the same directory whose *content* has a
//      top-level "version" key -- same shape VfxD itself writes at the
//      top of every file it manages, e.g. {"version": {"major": 1,
//      "minor": 9}, "categories": [...]}. Only the key's presence is
//      checked; the numbers inside it are never inspected here. This is
//      what lets a file with no "VfxD_" naming convention at all still be
//      browsed/edited/backed up/live-logged like any other installed
//      sin. Its sinName falls back to the filename stem (see
//      InstalledSinFile), and it never has a GitHub-tracked update since
//      it can't be in kSinNames.
//
// A bare, unsuffixed file (either kind) gets version = -1 so it always
// compares as older than whatever's on GitHub, for the subset that even
// has a GitHub side to compare against (see kSinNames). Does filesystem
// stat() calls, and for kind 2 a full JSON parse per candidate file --
// call on demand, never every frame.
//--------------------------------------------------------------------------------
std::vector<InstalledSinFile> ScanInstalledSinFiles(const std::string& denoiserAddonDir);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ExtractNameAndVersion
//--------------------------------------------------------------------------------
// Naming fallback shared by ScanInstalledSinFiles (for a .json file that
// doesn't match the classic VfxD_<Name> pattern) and ScanBackups in
// backup.cpp (for a .bak file whose underlying .json didn't either):
// splits `stem` -- a filename with its extension(s) already stripped by
// the caller -- into a base name and an optional trailing -v<N>/_v<N>
// version suffix. "MyEffects" -> {"MyEffects", -1}. "MyEffects_v7" ->
// {"MyEffects", 7}.
//--------------------------------------------------------------------------------
void ExtractNameAndVersion(const std::string& stem, std::string& outName, int& outVersion);