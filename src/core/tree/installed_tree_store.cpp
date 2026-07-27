// installed_tree_store.cpp
#include "core/tree/installed_tree_store.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    bool                                                            s_installedTreeLoaded = false;
    std::vector<InstalledSinFile>                                   s_installedSins;
    std::unordered_map<std::string, nlohmann::ordered_json>         s_installedJson; // sinName -> parsed file, only present if it parsed OK

    // Bumped every time s_installedJson is (re)loaded from disk -- see
    // GetInstalledTreeGeneration's doc comment in the header for why.
    int s_installedTreeGeneration = 0;

    // Set once, via InstalledTreeStore_SetApi (called from Addon_Init), to
    // the same AddonAPI_t pointer entry.cpp got from Nexus. Only used for
    // aApi->Log calls from SaveInstalledSinFile's write-failure path --
    // never reassigned afterward, so reading it later is safe without a
    // lock.
    AddonAPI_t* s_api = nullptr;

    std::unordered_map<std::string, std::vector<std::string>> s_duplicateGuidsBySin;

    // nlohmann::json::dump() always emits bare '\n' line endings, but every
    // VfxDenoiser file shipped/edited in the wild uses CRLF. Converting
    // here (rather than leaving dump()'s output as-is) keeps a saved
    // file's line endings consistent with what it had on disk before the
    // edit, instead of silently flipping the whole file to LF the first
    // time someone edits a single effect.
    std::string ToCrlf(const std::string& lfText)
    {
        std::string out;
        out.reserve(lfText.size() + lfText.size() / 20);
        for (char c : lfText)
        {
            if (c == '\n')
                out += '\r';
            out += c;
        }
        return out;
    }

    // Recursively walks every effect anywhere under `category`, keeping
    // each effect's name alongside its guids -- see CollectGuidNameMap.
    void CollectGuidNamesRecursive(const nlohmann::ordered_json& category,
                                    std::unordered_map<std::string, std::string>& out)
    {
        if (category.contains("effects") && category["effects"].is_array())
        {
            for (const auto& eff : category["effects"])
            {
                if (!eff.contains("guids") || !eff["guids"].is_array() ||
                    !eff.contains("name") || !eff["name"].is_string())
                    continue;

                std::string name = eff["name"].get<std::string>();
                for (const auto& g : eff["guids"])
                    if (g.is_string())
                        out[g.get<std::string>()] = name;
            }
        }

        if (category.contains("categories") && category["categories"].is_array())
            for (const auto& sub : category["categories"])
                CollectGuidNamesRecursive(sub, out);
    }

    // Flattens one effect's "behaviors" array into a single display string.
    // An effect can legitimately carry more than one behavior at once
    // (e.g. Hide for Others + Show for Self), so entries are joined with
    // "; " rather than assuming exactly one.
    std::string FormatBehaviors(const nlohmann::ordered_json& behaviors)
    {
        std::string out;
        for (const auto& behavior : behaviors)
        {
            std::string type   = behavior.value("type", std::string("?"));
            std::string caster = behavior.value("caster", std::string("?"));

            std::string one;
            if (type == "SetDuration" && behavior.contains("duration") && behavior["duration"].is_number())
                one = "Set duration: " + std::to_string(behavior["duration"].get<double>()) + "ms for " + caster;
            else
                one = type + " for " + caster;

            if (!out.empty())
                out += "; ";
            out += one;
        }
        return out;
    }

    // Same recursive walk as CollectGuidNamesRecursive, but keeping each
    // effect's own formatted "behaviors" summary instead of its name --
    // see CollectGuidBehaviorMap. An effect with no "behaviors" array
    // still gets an (empty-string) entry, so a known guid is
    // distinguishable from one that's merely unconfigured.
    void CollectGuidBehaviorsRecursive(const nlohmann::ordered_json& category,
                                        std::unordered_map<std::string, std::string>& out)
    {
        if (category.contains("effects") && category["effects"].is_array())
        {
            for (const auto& eff : category["effects"])
            {
                if (!eff.contains("guids") || !eff["guids"].is_array())
                    continue;

                std::string summary = (eff.contains("behaviors") && eff["behaviors"].is_array())
                                           ? FormatBehaviors(eff["behaviors"])
                                           : std::string();

                for (const auto& g : eff["guids"])
                    if (g.is_string())
                        out[g.get<std::string>()] = summary;
            }
        }

        if (category.contains("categories") && category["categories"].is_array())
            for (const auto& sub : category["categories"])
                CollectGuidBehaviorsRecursive(sub, out);
    }
} // namespace

void InstalledTreeStore_SetApi(AddonAPI_t* aApi)
{
    s_api = aApi;
}

void LoadInstalledEffectsTree(const std::string& denoiserAddonDir)
{
    s_installedSins = ScanInstalledSinFiles(denoiserAddonDir);
    s_installedJson.clear();
    s_duplicateGuidsBySin.clear();

    for (const auto& sin : s_installedSins)
    {
        std::ifstream in(sin.fullPath, std::ios::binary);
        if (!in)
            continue;

        nlohmann::ordered_json parsed;
        try
        {
            in >> parsed;
        }
        catch (const nlohmann::ordered_json::exception&)
        {
            continue; // malformed file on disk -- leave it out of the map, not fatal
        }

        // Checked once here, against the real on-disk file, independent of
        // whether an update is even available -- this is a property of
        // this file in isolation (see merge.h's FindDuplicateGuids doc
        // comment), not something StartLoadDiff/ResolveMergePlan need to
        // discover on their own. github_update.cpp's StartLoadDiff runs
        // this same check again on its own read of the file before
        // touching the network, so a duplicate found here and one found
        // there are consistent -- neither trusts the other's cache.
        s_duplicateGuidsBySin[sin.sinName] = FindDuplicateGuids(parsed);

        s_installedJson[sin.sinName] = std::move(parsed);
    }

    s_installedTreeLoaded = true;
    ++s_installedTreeGeneration;
}

void InvalidateInstalledTree()
{
    s_installedTreeLoaded = false;
}

bool IsInstalledTreeLoaded()
{
    return s_installedTreeLoaded;
}

int GetInstalledTreeGeneration()
{
    return s_installedTreeGeneration;
}

const std::vector<InstalledSinFile>& GetInstalledSins()
{
    return s_installedSins;
}

const std::unordered_map<std::string, nlohmann::ordered_json>& GetInstalledJson()
{
    return s_installedJson;
}

const nlohmann::ordered_json* FindInstalledJson(const std::string& sinName)
{
    auto it = s_installedJson.find(sinName);
    return it == s_installedJson.end() ? nullptr : &it->second;
}

nlohmann::ordered_json* FindInstalledJsonMutable(const std::string& sinName)
{
    auto it = s_installedJson.find(sinName);
    return it == s_installedJson.end() ? nullptr : &it->second;
}

const std::unordered_map<std::string, std::vector<std::string>>& GetDuplicateGuidsBySin()
{
    return s_duplicateGuidsBySin;
}

std::unordered_map<std::string, std::string> CollectGuidNameMap()
{
    std::unordered_map<std::string, std::string> out;
    for (const auto& [sinName, file] : s_installedJson)
    {
        if (file.contains("categories") && file["categories"].is_array())
            for (const auto& cat : file["categories"])
                CollectGuidNamesRecursive(cat, out);
    }
    return out;
}

std::unordered_map<std::string, std::string> CollectGuidBehaviorMap()
{
    std::unordered_map<std::string, std::string> out;
    for (const auto& [sinName, file] : s_installedJson)
    {
        if (file.contains("categories") && file["categories"].is_array())
            for (const auto& cat : file["categories"])
                CollectGuidBehaviorsRecursive(cat, out);
    }
    return out;
}

// Writes s_installedJson[sinName] back to the file it was loaded from,
// using the same backup-then-tmp-then-rename safety pattern as
// github_update.cpp's StartApplyUpdate: never touch the real file
// directly, so a crash or failed write can't corrupt or lose the user's
// data. Unlike an applied GitHub update, an edit never changes the
// filename (no version bump), so this always writes back to the exact
// path it read from.
bool SaveInstalledSinFile(const std::string& sinName, std::string& outError)
{
    auto jsonIt = s_installedJson.find(sinName);
    if (jsonIt == s_installedJson.end())
    {
        outError = "no in-memory copy of this file to save";
        return false;
    }

    std::string fullPath;
    for (const auto& sin : s_installedSins)
    {
        if (sin.sinName == sinName)
        {
            fullPath = sin.fullPath;
            break;
        }
    }
    if (fullPath.empty())
    {
        outError = "couldn't find this file's path on disk";
        return false;
    }

    std::error_code ec;
    fs::path backupPath = fs::path(fullPath).concat(".bak");
    fs::copy_file(fullPath, backupPath, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        outError = "couldn't create .bak";
        return false;
    }

    fs::path tmpPath = fs::path(fullPath).concat(".tmp");
    try
    {
        std::ofstream out(tmpPath, std::ios::binary);
        if (!out)
        {
            outError = "couldn't open temp file for writing";
            if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
            return false;
        }

        // dump() always emits bare '\n'; VfxDenoiser's own files are CRLF,
        // so convert here rather than silently flipping every line ending
        // to LF the moment a file gets edited and saved.
        out << ToCrlf(jsonIt->second.dump(1, '\t'));
        if (!out)
        {
            outError = "write to temp file failed (disk full?)";
            if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
            return false;
        }

        out.close();
        if (!out)
        {
            outError = "temp file didn't flush to disk cleanly (disk full?)";
            if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
            return false;
        }
    }
    catch (...)
    {
        outError = "couldn't write temp file";
        if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", (sinName + ": " + outError).c_str());
        return false;
    }

    fs::rename(tmpPath, fullPath, ec);
    if (ec)
    {
        outError = "couldn't rename into place";
        return false;
    }

    return true;
}
