//################################################################################
// sin_files.cpp
//--------------------------------------------------------------------------------
// See sin_files.h for the module contract.
//--------------------------------------------------------------------------------

#include "sin_files.h"

#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

std::vector<InstalledSinFile> ScanInstalledSinFiles(const std::string& denoiserAddonDir)
{
    std::vector<InstalledSinFile> out;

    std::error_code ec;
    if (!fs::exists(denoiserAddonDir, ec) || ec)
        return out;   //. not installed, nothing to find

    //_ Matches "VfxD_Gluttony.json", "..._v3883.json" and "...-v3883.json"
    // -- both separators are observed in the wild, so both are accepted.
    static const std::regex kPattern(R"(^VfxD_(Gluttony|Pride|Sloth)(?:[-_]v(\d+))?\.json$)");

    for (const auto& entry : fs::directory_iterator(denoiserAddonDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        std::string fileName = entry.path().filename().string();
        std::smatch m;
        if (!std::regex_match(fileName, m, kPattern)) continue;

        InstalledSinFile sin;
        sin.sinName  = m[1].str();
        sin.fileName = fileName;
        sin.fullPath = entry.path().string();
        sin.version  = m[2].matched ? std::stoi(m[2].str()) : -1;   //. no suffix, always oldest
        out.push_back(std::move(sin));
    }

    return out;
}