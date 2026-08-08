//################################################################################
// sin_files.cpp
//--------------------------------------------------------------------------------
// See sin_files.h for the module contract.
//--------------------------------------------------------------------------------

#include "sin_files.h"

#include "nlohmann_json.hpp"

#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

void ExtractNameAndVersion(const std::string& stem, std::string& outName, int& outVersion)
{
    static const std::regex kVersionSuffix(R"(^(.+)[-_]v(\d+)$)");

    std::smatch m;
    if (std::regex_match(stem, m, kVersionSuffix))
    {
        outName    = m[1].str();
        outVersion = std::stoi(m[2].str());
    }
    else
    {
        outName    = stem;
        outVersion = -1;
    }
}

std::vector<InstalledSinFile> ScanInstalledSinFiles(const std::string& denoiserAddonDir)
{
    std::vector<InstalledSinFile> out;

    std::error_code ec;
    if (!fs::exists(denoiserAddonDir, ec) || ec)
        return out;   //. not installed, nothing to find

    //_ Matches "VfxD_<Name>[-v<N>|_v<N>].json" -- both separators are seen
    // in the wild. <Name> is any letters/digits run, not a fixed list (see
    // sin_files.h), so a hand-edited "VfxD_Greed.json" with no version matches too.
    static const std::regex kVfxdPattern(R"(^VfxD_([A-Za-z0-9]+)(?:[-_]v(\d+))?\.json$)");

    for (const auto& entry : fs::directory_iterator(denoiserAddonDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        const fs::path& path = entry.path();
        if (path.extension() != ".json") continue;

        std::string fileName = path.filename().string();

        InstalledSinFile sin;
        sin.fileName = fileName;
        sin.fullPath = path.string();

        std::smatch m;
        if (std::regex_match(fileName, m, kVfxdPattern))
        {
            //_ Classic VfxD_<Name>[-v<N>].json naming -- filename alone
            // is enough, no need to open the file just to confirm what
            // it already told us.
            sin.sinName = m[1].str();
            sin.version = m[2].matched ? std::stoi(m[2].str()) : -1;
        }
        else
        {
            //_ Anything else only counts if its content says so -- a
            // top-level "version" key, same shape VfxD writes (numbers
            // never checked, see sin_files.h) -- so any arbitrarily-named file matches too.
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;

            nlohmann::json probe;
            try
            {
                in >> probe;
            }
            catch (const nlohmann::json::exception&)
            {
                continue;   //. not valid JSON at all -- not ours
            }

            if (!probe.is_object() || !probe.contains("version")) continue;

            ExtractNameAndVersion(path.stem().string(), sin.sinName, sin.version);
        }

        out.push_back(std::move(sin));
    }

    return out;
}