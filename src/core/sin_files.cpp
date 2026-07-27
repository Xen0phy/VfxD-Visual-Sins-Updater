#include "core/sin_files.h"
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

std::vector<InstalledSinFile> ScanInstalledSinFiles(const std::string& denoiserAddonDir)
{
    std::vector<InstalledSinFile> out;

    std::error_code ec;
    if (!fs::exists(denoiserAddonDir, ec) || ec)
        return out; // VfxDenoiser isn't installed / folder doesn't exist yet -- not an error, just nothing to find

    // Matches "VfxD_Gluttony.json", "VfxD_Gluttony_v3883.json" and
    // "VfxD_Gluttony-v3883.json" -- both separators have been observed in
    // the wild, so both must be accepted here.
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
        sin.version  = m[2].matched ? std::stoi(m[2].str()) : -1; // no suffix -> always "oldest"
        out.push_back(std::move(sin));
    }

    return out;
}
