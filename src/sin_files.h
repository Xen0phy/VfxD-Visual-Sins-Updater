#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The three VfxD "Visual Sins" effect files this addon knows how to update.
// Keep this list in sync with github_update.cpp's asset-name matching.
// ---------------------------------------------------------------------------
inline const char* const kSinNames[] = { "Gluttony", "Pride", "Sloth" };
inline constexpr int kSinCount = 3;

struct InstalledSinFile
{
    std::string sinName;      // e.g. "Gluttony"
    std::string fullPath;     // absolute path to the file on disk
    std::string fileName;     // just the filename, e.g. "VfxD_Gluttony-v3883.json"
    int         version = -1; // -1 = no version suffix at all ("VfxD_Gluttony.json"),
                               // which is always treated as older than any real version.
};

// Scans `denoiserAddonDir` (e.g. "<GW2>/addons/VfxDenoiser") for any of the
// three known VfxD_<Sin>.json / VfxD_<Sin>_v<N>.json / VfxD_<Sin>-v<N>.json
// files. Both separators are accepted since the addon isn't consistent
// about which one it uses. A bare, unsuffixed file gets version = -1 so it
// always compares as older than whatever's on GitHub.
//
// This does a handful of filesystem stat() calls -- call it on demand
// (addon load, after an update completes), never every frame.
std::vector<InstalledSinFile> ScanInstalledSinFiles(const std::string& denoiserAddonDir);
