//################################################################################
// sql_update.cpp
//--------------------------------------------------------------------------------
// See sql_update.h for the module contract. Everything here runs on whatever
// thread calls it (the render thread, in practice) -- no std::thread, no atomics,
// no mutex, since nothing here can block: every read is local SQLite (already-open
// connection) and every write is a local file. The only shared state is
// s_diffCache, and it's only ever touched from that one thread, same assumption
// effect_db.cpp already makes about its own render-thread-only callers.
//--------------------------------------------------------------------------------

#include "effect_db.h"
#include "merge.h"
#include "nlohmann_json.hpp"
#include "sin_files.h"
#include "sin_generator.h"
#include "sql_update.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

using json = nlohmann::ordered_json;

namespace fs = std::filesystem;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ToCrlf
//--------------------------------------------------------------------------------
// Same conversion github_update.cpp/installed_tree_store.cpp each keep their own
// copy of -- see either of their comments for why (every real VfxDenoiser file on
// disk uses CRLF; nlohmann::json::dump() always emits bare '\n').
//--------------------------------------------------------------------------------
static std::string ToCrlf(const std::string& lfText)
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

//_ Set once before Addon_Load hands off, never reassigned -- same as github_update.cpp's s_api
static AddonAPI_t* s_api = nullptr;

void SetSqlUpdateLogger(AddonAPI_t* aApi)
{
    s_api = aApi;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// LogCritical
//--------------------------------------------------------------------------------
// No-op if SetSqlUpdateLogger was never called.
//--------------------------------------------------------------------------------
static void LogCritical(const std::string& msg)
{
    if (s_api) s_api->Log(LOGL_CRITICAL, "VfxDSinsUpdater", msg.c_str());
}

static ESinGeneratorVariant VariantForSin(const std::string& sinName)
{
    if (sinName == "Pride") return ESinGeneratorVariant::Pride;
    if (sinName == "Sloth") return ESinGeneratorVariant::Sloth;
    return ESinGeneratorVariant::Gluttony;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// SharedMasterGuidCount
//--------------------------------------------------------------------------------
// The shared master guid count every sin's filename is compared against and
// stamped with -- not this sin's own filtered count. See sql_update.h's top
// comment for the full reasoning.
//--------------------------------------------------------------------------------
static int SharedMasterGuidCount()
{
    return SinGenerator_CountEmittedGuids(ESinGeneratorVariant::Gluttony);
}

//********************************************************************************
// SqlDiffCacheEntry
//--------------------------------------------------------------------------------
// Everything ApplySqlUpdate needs to apply exactly what LoadSqlDiff resolved and
// displayed, without regenerating or re-deciding anything -- same shape/reasoning
// as github_update.cpp's own DiffCacheEntry. installedPath/latestVersion are not
// part of the public SinDiffInfo, same as the GitHub path.
//--------------------------------------------------------------------------------
struct SqlDiffCacheEntry
{
    EDiffStatus status = EDiffStatus::NotLoaded;
    MergePlan   plan;
    json        oldFile;
    std::string installedPath;
    int         latestVersion = -1; //. SinGenerator_CountEmittedGuids at LoadSqlDiff time
};

//_ Keyed by sinName. Render-thread-only, see file header -- no mutex.
static std::unordered_map<std::string, SqlDiffCacheEntry> s_diffCache;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// WriteJsonAtomic
//--------------------------------------------------------------------------------
// Shared tmp-then-rename write path for both ApplySqlUpdate and InstallSqlSin --
// same pattern StartApplyUpdate/StartInstallSin use in github_update.cpp. Returns
// "" on success, a human-readable failure reason otherwise.
//--------------------------------------------------------------------------------
static std::string WriteJsonAtomic(const fs::path& newPath, const json& content)
{
    fs::path tmpPath = newPath;
    tmpPath += ".tmp";

    try
    {
        std::ofstream out(tmpPath, std::ios::binary);
        if (!out) return "couldn't open temp file for writing";

        out << ToCrlf(content.dump(1, '\t'));
        if (!out) return "write to temp file failed (disk full?)";

        out.close();
        if (!out) return "temp file didn't flush to disk cleanly (disk full?)";
    }
    catch (...) { return "couldn't write temp file"; }

    std::error_code ec;
    fs::rename(tmpPath, newPath, ec);
    if (ec) return "couldn't rename into place";

    return "";
}

std::vector<SqlSinUpdateInfo> CheckSqlUpdates(const std::string& denoiserAddonDir, std::string& outError)
{
    outError.clear();

    std::string openError;
    if (!EffectDb_EnsureOpenForBrowsing(denoiserAddonDir, openError))
    {
        outError = "Couldn't open effect db: " + openError;
        return {};
    }

    auto installed = ScanInstalledSinFiles(denoiserAddonDir);
    std::unordered_map<std::string, InstalledSinFile> installedByName;
    for (const auto& f : installed)
        installedByName[f.sinName] = f;

    //_ Computed once; every sin compares against this same number, not its own count
    int masterCount = SharedMasterGuidCount();

    std::vector<SqlSinUpdateInfo> results;
    results.reserve(kSinCount);

    for (int i = 0; i < kSinCount; ++i)
    {
        std::string sinName = kSinNames[i];

        SqlSinUpdateInfo info;
        info.sinName       = sinName;
        info.latestVersion = masterCount;

        auto instIt = installedByName.find(sinName);
        bool isInstalled = (instIt != installedByName.end());
        if (isInstalled)
        {
            info.installedPath    = instIt->second.fullPath;
            info.installedVersion = instIt->second.version;
            info.state = (info.installedVersion < info.latestVersion)
                ? ESinUpdateState::UpdateAvailable
                : ESinUpdateState::UpToDate;
        }
        else
        {
            info.state = ESinUpdateState::NotInstalled;
        }

        results.push_back(std::move(info));
    }

    return results;
}

SinDiffInfo LoadSqlDiff(const std::string& denoiserAddonDir, const std::string& sinName)
{
    SinDiffInfo out;
    out.sinName = sinName;

    auto fail = [&](EDiffStatus status)
    {
        s_diffCache[sinName].status = status;
        out.status = status;
        return out;
    };

    std::string openError;
    if (!EffectDb_EnsureOpenForBrowsing(denoiserAddonDir, openError))
        return fail(EDiffStatus::Error);

    auto installed = ScanInstalledSinFiles(denoiserAddonDir);
    const InstalledSinFile* installedFile = nullptr;
    for (const auto& f : installed)
        if (f.sinName == sinName) { installedFile = &f; break; }

    if (!installedFile)
        return fail(EDiffStatus::Error); //. not installed, use InstallSqlSin

    json oldFile;
    try
    {
        std::ifstream in(installedFile->fullPath, std::ios::binary);
        if (!in) return fail(EDiffStatus::Error);
        in >> oldFile;
    }
    catch (...) { return fail(EDiffStatus::Error); }

    //_ Same guard StartLoadDiff applies before trusting a guid-first merge -- see merge.h
    if (!FindDuplicateGuids(oldFile).empty())
        return fail(EDiffStatus::Blocked);

    ESinGeneratorVariant variant = VariantForSin(sinName);
    json newFile = SinGenerator_Generate(variant, /*major*/ 1, /*minor*/ 10);

    bool ok = false;
    MergePlan plan = ResolveMergePlan(oldFile, newFile, ok);
    if (!ok) return fail(EDiffStatus::Error);

    SqlDiffCacheEntry entry;
    entry.status        = EDiffStatus::Ready;
    entry.plan          = plan;
    entry.oldFile        = std::move(oldFile);
    entry.installedPath = installedFile->fullPath;
    entry.latestVersion = SharedMasterGuidCount();
    s_diffCache[sinName] = std::move(entry);

    out.status = EDiffStatus::Ready;
    out.plan   = std::move(plan);
    return out;
}

SinDiffInfo GetSqlDiffInfo(const std::string& sinName)
{
    SinDiffInfo out;
    out.sinName = sinName;

    auto it = s_diffCache.find(sinName);
    if (it == s_diffCache.end())
        return out; //. NotLoaded

    out.status = it->second.status;
    if (out.status == EDiffStatus::Ready)
        out.plan = it->second.plan;
    return out;
}

std::vector<SinDiffInfo> GetSqlDiffInfo()
{
    std::vector<SinDiffInfo> out;
    out.reserve(s_diffCache.size());
    for (const auto& [sinName, entry] : s_diffCache)
    {
        SinDiffInfo info;
        info.sinName = sinName;
        info.status  = entry.status;
        if (info.status == EDiffStatus::Ready)
            info.plan = entry.plan;
        out.push_back(std::move(info));
    }
    return out;
}

bool ApplySqlUpdate(const std::string& denoiserAddonDir, const std::string& sinName, std::string& outMessage)
{
    (void)denoiserAddonDir; //. already cached from LoadSqlDiff

    if (EffectDb_IsEnabled())
    {
        outMessage = "Can't apply while \"for science\" capture is enabled.";
        return false;
    }

    auto it = s_diffCache.find(sinName);
    if (it == s_diffCache.end() || it->second.status != EDiffStatus::Ready)
    {
        outMessage = "No SQL-sourced changes loaded for " + sinName + " -- load the diff first.";
        return false;
    }

    SqlDiffCacheEntry entry = it->second; //. own copy, cache erased below

    json oldFile = entry.oldFile;

    //_ Backs up first so a user's tuning is never destroyed silently
    std::error_code ec;
    fs::path backupPath = fs::path(entry.installedPath).concat(".bak");
    fs::copy_file(entry.installedPath, backupPath, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        outMessage = "Failed: " + sinName + " (couldn't create .bak)";
        LogCritical(outMessage);
        return false;
    }

    ApplyMergePlan(oldFile, entry.plan);

    fs::path dir = fs::path(entry.installedPath).parent_path();
    std::string newFileName = "VfxD_" + sinName + "-v" + std::to_string(entry.latestVersion) + ".json";
    fs::path newPath = dir / newFileName;

    std::string writeError = WriteJsonAtomic(newPath, oldFile);
    if (!writeError.empty())
    {
        outMessage = "Failed: " + sinName + " (" + writeError + ")";
        LogCritical(outMessage);
        return false;
    }

    //_ Skips removal if the version-stamped name is identical to the old one
    if (fs::path(entry.installedPath) != newPath)
        fs::remove(entry.installedPath, ec); //. best-effort; leftover file is harmless

    s_diffCache.erase(sinName);

    outMessage = "Updated (from SQL): " + sinName;
    return true;
}

bool InstallSqlSin(const std::string& denoiserAddonDir, const std::string& sinName, std::string& outMessage)
{
    bool isKnownSin = false;
    for (int i = 0; i < kSinCount; ++i)
        if (sinName == kSinNames[i]) { isKnownSin = true; break; }

    if (!isKnownSin)
    {
        outMessage = "\"" + sinName + "\" isn't a SQL-generated sin.";
        return false;
    }

    std::string openError;
    if (!EffectDb_EnsureOpenForBrowsing(denoiserAddonDir, openError))
    {
        outMessage = "Couldn't open effect db: " + openError;
        return false;
    }

    auto installed = ScanInstalledSinFiles(denoiserAddonDir);
    for (const auto& f : installed)
    {
        if (f.sinName == sinName)
        {
            outMessage = sinName + " is already installed -- use the update action instead.";
            return false;
        }
    }

    ESinGeneratorVariant variant = VariantForSin(sinName);
    json generated = SinGenerator_Generate(variant, /*major*/ 1, /*minor*/ 10);
    int  guidCount = SharedMasterGuidCount();

    fs::path dir = fs::path(denoiserAddonDir);
    std::string newFileName = "VfxD_" + sinName + "-v" + std::to_string(guidCount) + ".json";
    fs::path newPath = dir / newFileName;

    std::string writeError = WriteJsonAtomic(newPath, generated);
    if (!writeError.empty())
    {
        outMessage = "Failed: " + sinName + " (" + writeError + ")";
        LogCritical(outMessage);
        return false;
    }

    outMessage = "Installed (from SQL): " + sinName;
    return true;
}