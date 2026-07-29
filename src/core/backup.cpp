//################################################################################
// backup.cpp
//--------------------------------------------------------------------------------
// See backup.h for the module contract. This file owns: scanning the
// addon dir for .bak files, and the read-then-backup-then-temp-rename
// sequence that makes a restore crash-safe.
//--------------------------------------------------------------------------------

#include "core/backup.h"

#include "sin_files.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

std::vector<BackupInfo> ScanBackups(const std::string& denoiserAddonDir)
{
    std::vector<BackupInfo> out;

    std::error_code ec;
    if (!fs::exists(denoiserAddonDir, ec) || ec)
        return out;   //. not installed, nothing to find

    //_ Same sin-name/separator pattern as ScanInstalledSinFiles (any
    // <Name>, not a fixed list -- see sin_files.h), plus a trailing
    // ".bak" -- bare unsuffixed backups match too; version isn't tracked
    // here since a backup is only ever restored to its own path.
    static const std::regex kVfxdPattern(R"(^VfxD_([A-Za-z0-9]+)(?:[-_]v\d+)?\.json\.bak$)");

    for (const auto& entry : fs::directory_iterator(denoiserAddonDir, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        std::string fileName = entry.path().filename().string();
        if (fileName.size() < 9 || fileName.compare(fileName.size() - 9, 9, ".json.bak") != 0)
            continue;   //. not a sin file's backup at all

        BackupInfo info;
        std::smatch m;
        if (std::regex_match(fileName, m, kVfxdPattern))
        {
            info.sinName = m[1].str();
        }
        else
        {
            //_ Not VfxD_<Name>-named -- fall back to the same
            // name/version split ScanInstalledSinFiles uses for its own
            // content-matched files, so a backup's sinName always lines
            // up with the live file it belongs to. Version itself is
            // discarded here, same as the VfxD_<Name> branch above.
            std::string outName;
            int         outVersion;
            ExtractNameAndVersion(fileName.substr(0, fileName.size() - 9), outName, outVersion);
            info.sinName = outName;
        }

        info.bakPath     = entry.path().string();
        info.bakFileName = fileName;
        info.restorePath = fs::path(entry.path()).replace_extension().string();   //. strips just the .bak

        std::error_code sizeEc;
        auto size = fs::file_size(entry.path(), sizeEc);
        info.fileSize = sizeEc ? 0 : size;

        out.push_back(std::move(info));
    }

    return out;
}

bool RestoreBackup(const BackupInfo& backup, const std::string& currentInstalledPath, std::string& outError)
{
    //_ Read into memory before touching anything on disk -- bakPath and
    // restorePath can be the same file content-wise in edge cases.
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

    //_ Back up whatever's currently at restorePath onto this same .bak
    // first -- makes a restore a swap; doing it again undoes it.
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

    //_ Temp file then rename over restorePath, so a crash mid-restore
    // can't corrupt anything.
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

    //_ currentInstalledPath, if different from restorePath (e.g. after an
    // applied update), is now stale -- remove it best-effort; failure here
    // doesn't undo the restore above.
    if (!currentInstalledPath.empty() && fs::path(currentInstalledPath) != fs::path(restorePath))
        fs::remove(currentInstalledPath, ec);

    return true;
}