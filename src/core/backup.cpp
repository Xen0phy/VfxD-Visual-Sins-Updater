#include "core/backup.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <system_error>

namespace fs = std::filesystem;

std::vector<BackupInfo> ScanBackups(const std::string& denoiserAddonDir)
{
    std::vector<BackupInfo> out;

    std::error_code ec;
    if (!fs::exists(denoiserAddonDir, ec) || ec)
        return out; // VfxDenoiser isn't installed / folder doesn't exist yet -- not an error, just nothing to find

    // Same three sin names and both separators as ScanInstalledSinFiles'
    // own pattern, just with a trailing ".bak". A bare, unsuffixed backup
    // ("VfxD_Gluttony.json.bak") is matched too -- version isn't tracked
    // here since a backup is only ever restored to its own restorePath,
    // never compared against a "latest" version like the live files are.
    static const std::regex kPattern(R"(^VfxD_(Gluttony|Pride|Sloth)(?:[-_]v\d+)?\.json\.bak$)");

    for (const auto& entry : fs::directory_iterator(denoiserAddonDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        std::string fileName = entry.path().filename().string();
        std::smatch m;
        if (!std::regex_match(fileName, m, kPattern)) continue;

        BackupInfo info;
        info.sinName     = m[1].str();
        info.bakPath     = entry.path().string();
        info.bakFileName = fileName;
        info.restorePath = fs::path(entry.path()).replace_extension().string(); // strips just the ".bak"

        std::error_code sizeEc;
        auto size = fs::file_size(entry.path(), sizeEc);
        info.fileSize = sizeEc ? 0 : size;

        out.push_back(std::move(info));
    }

    return out;
}

bool RestoreBackup(const BackupInfo& backup, const std::string& currentInstalledPath, std::string& outError)
{
    // 1. Read the backup's content into memory before touching anything on
    //    disk -- restorePath and backup.bakPath can be the same underlying
    //    file content-wise in edge cases, and this way nothing downstream
    //    depends on the .bak file still existing/unmodified.
    std::string content;
    {
        std::ifstream in(backup.bakPath, std::ios::binary);
        if (!in)
        {
            outError = "couldn't open " + backup.bakFileName + " for reading";
            return false;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        if (!in.good() && !in.eof())
        {
            outError = "couldn't read " + backup.bakFileName;
            return false;
        }
        content = buf.str();
    }

    const std::string& restorePath = backup.restorePath;

    // 2. If something's currently sitting at restorePath, back it up first
    //    -- onto the very .bak we just read into memory. This is the same
    //    backup-before-write pattern every other write in this addon
    //    follows, and it means rollback is really a swap: doing it again
    //    would bring back what's about to be overwritten. Nothing to back
    //    up if restorePath doesn't currently exist (e.g. rolling back an
    //    applied update whose old-named file was removed).
    std::error_code ec;
    if (fs::exists(restorePath, ec))
    {
        fs::copy_file(restorePath, backup.bakPath, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            outError = "couldn't back up current file before restoring";
            return false;
        }
    }

    // 3. Write to a temp file first, then rename over restorePath -- so a
    //    crash mid-restore can't corrupt anything.
    fs::path tmpPath = fs::path(restorePath).concat(".tmp");
    try
    {
        std::ofstream out(tmpPath, std::ios::binary);
        if (!out)
        {
            outError = "couldn't open temp file for writing";
            return false;
        }

        out << content;
        if (!out)
        {
            outError = "write to temp file failed (disk full?)";
            return false;
        }

        out.close();
        if (!out)
        {
            outError = "temp file didn't flush to disk cleanly (disk full?)";
            return false;
        }
    }
    catch (...)
    {
        outError = "couldn't write temp file";
        return false;
    }

    fs::rename(tmpPath, restorePath, ec);
    if (ec)
    {
        outError = "couldn't rename into place";
        return false;
    }

    // 4. If the sin is currently installed under a different filename than
    //    what was just restored (an applied update bumped the version
    //    stamp), that file is now stale -- remove it, best-effort, same as
    //    github_update.cpp's own cleanup of the old-named file after
    //    applying an update. A failure here doesn't undo the restore above;
    //    it just leaves harmless clutter.
    if (!currentInstalledPath.empty() && fs::path(currentInstalledPath) != fs::path(restorePath))
        fs::remove(currentInstalledPath, ec);

    return true;
}
