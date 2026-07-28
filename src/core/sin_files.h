//################################################################################
// sin_files.h
//--------------------------------------------------------------------------------
// The three VfxD "Visual Sins" effect files this addon knows how to
// update. kSinNames/kSinCount must be kept in sync with
// github_update.cpp's asset-name matching.
//--------------------------------------------------------------------------------

#pragma once

#include <string>
#include <vector>

inline const char* const kSinNames[] = { "Gluttony", "Pride", "Sloth" };
inline constexpr int kSinCount = 3;

//********************************************************************************
// InstalledSinFile
//--------------------------------------------------------------------------------
// sinName    e.g. "Gluttony"
// fullPath   absolute path to the file on disk
// fileName   just the filename, e.g. "VfxD_Gluttony-v3883.json"
// version    -1 = no version suffix ("VfxD_Gluttony.json"), always
//            treated as older than any real version
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
// Scans denoiserAddonDir (e.g. "<GW2>/addons/VfxDenoiser") for any of the
// three known VfxD_<Sin>.json / VfxD_<Sin>_v<N>.json / VfxD_<Sin>-v<N>.json
// files -- both separators are accepted since the addon isn't consistent
// about which one it uses. A bare, unsuffixed file gets version = -1 so
// it always compares as older than whatever's on GitHub. Does filesystem
// stat() calls -- call on demand, never every frame.
//--------------------------------------------------------------------------------
std::vector<InstalledSinFile> ScanInstalledSinFiles(const std::string& denoiserAddonDir);