//################################################################################
// sin_files.cpp   (see: sin_files.h)
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

    //_ Kind 1 pattern from sin_files.h -- both "-v"/"_v" separators match.
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
            //_ Filename alone is enough -- no need to open the file.
            sin.sinName = m[1].str();
            sin.version = m[2].matched ? std::stoi(m[2].str()) : -1;
        }
        else
        {
            //_ Kind 2 from sin_files.h -- a top-level "version" key counts.
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;

            nlohmann::json probe;
            try
            {
                in >> probe;
            }
            catch (const nlohmann::json::exception&)
            {
                continue;   //. invalid JSON -- not ours
            }

            if (!probe.is_object() || !probe.contains("version")) continue;

            ExtractNameAndVersion(path.stem().string(), sin.sinName, sin.version);
        }

        out.push_back(std::move(sin));
    }

    return out;
}