//################################################################################
// sin_files.h
//--------------------------------------------------------------------------------
// The VfxD "Visual Sins" effect files this addon can find on disk.
// kSinNames/kSinCount is NOT that whole set -- it's the smaller subset that
// also gets GitHub-hosted updates (see its own comment below).
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
// check/apply action row. Separate from whatever ScanInstalledSinFiles
// finds on disk: a hand-edited-only file is a perfectly normal installed
// sin that just never appears here. Keep in sync with github_update.cpp's
// asset-name matching, NOT with ScanInstalledSinFiles, which no longer
// hardcodes this list.
//--------------------------------------------------------------------------------
inline const char* const kSinNames[] = { "Gluttony", "Pride", "Sloth" };
inline constexpr int kSinCount = 3;

//********************************************************************************
// InstalledSinFile
//--------------------------------------------------------------------------------
// sinName    parsed sin/effect name (see ScanInstalledSinFiles for the
//            fallback rule when the file isn't VfxD_<Name>-named)
// fullPath   absolute path to the file on disk
// fileName   just the filename, e.g. "VfxD_Gluttony-v3883.json"
// version    -1 = no version suffix (see ScanInstalledSinFiles)
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
// Scans denoiserAddonDir for two kinds of file: classic
// VfxD_<Name>[-v<N>|_v<N>].json (any <Name>, not limited to kSinNames --
// filename alone is enough), and any other *.json whose content has a top-level
// "version" key like VfxD itself writes (key presence only, numbers never
// checked). Kind 2 falls back to the filename stem for sinName (see
// InstalledSinFile) and never has a GitHub-tracked update since it can't be in
// kSinNames. Unsuffixed files get version = -1, always sorting older than a
// real version. Does stat() calls, and for kind 2 a full JSON parse per
// candidate -- call on demand, never every frame.
//--------------------------------------------------------------------------------
std::vector<InstalledSinFile> ScanInstalledSinFiles(const std::string& denoiserAddonDir);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ExtractNameAndVersion
//--------------------------------------------------------------------------------
// Naming fallback shared by ScanInstalledSinFiles (for a .json file that
// doesn't match the classic VfxD_<Name> pattern) and ScanBackups in backup.cpp
// (for a .bak file whose underlying .json didn't either): splits `stem` -- a
// filename with its extension(s) already stripped by the caller -- into a base
// name and an optional trailing -v<N>/_v<N> version suffix. "MyEffects" ->
// {"MyEffects", -1}. "MyEffects_v7" -> {"MyEffects", 7}.
//--------------------------------------------------------------------------------
void ExtractNameAndVersion(const std::string& stem, std::string& outName, int& outVersion);