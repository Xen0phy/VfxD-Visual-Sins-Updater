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

    //_ Matches "VfxD_Gluttony.json", "..._v3883.json" and "...-v3883.json"
    // -- both separators are observed in the wild, so both are accepted.
    // <Name> itself is any run of letters/digits, not a fixed list (see
    // sin_files.h) -- this is what lets a hand-edited-only file like
    // "VfxD_Greed.json" show up here too, with no version suffix at all.
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
            //_ Anything else only counts as an installed sin if its
            // content says so: a top-level "version" key, the same
            // shape VfxD itself writes -- the numbers inside it are
            // never checked (see sin_files.h). This is what lets an
            // arbitrarily-named file be edited/backed up/live-logged
            // right alongside the classic VfxD_<Name> ones.
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
