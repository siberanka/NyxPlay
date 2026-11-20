#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Core of NyxPlayUpdater.exe: replaces the program files of an installed (portable) client with
// the files of a verified, extracted package, and rolls everything back when a step fails.
// It has no Qt dependency so that the updater stays a small separate executable; the client's
// tests link the same sources.
namespace NyxUpdater {

namespace fs = std::filesystem;

// File that lists every program file of a package (relative paths, '/' separators, UTF-8, one per
// line). It is how an update recognises the files of the previous version that it may remove.
constexpr const char* FileListName = "package-files.txt";

// Suffix of files that were moved out of the way. They are deleted by the client on its next start.
constexpr const wchar_t* BackupSuffix = L".nyxold";

struct Options {
    fs::path installDir;
    fs::path stageDir;
    // A renamed file may be briefly held by a scanner; try this many times before giving up.
    int renameRetries = 20;
    int retryDelayMs = 250;
};

struct Outcome {
    bool ok = false;
    // Short stable code, "ok" on success: stage-invalid, install-invalid, locked, write-failed,
    // rollback-incomplete.
    std::string code = "install-invalid";
    std::string detail;               // UTF-8 relative path or a short note
    std::vector<std::string> log;     // UTF-8 lines, one per step
};

// Relative path as it may appear in a file list: no absolute paths, drive letters, backslashes,
// empty, "." or ".." segments, stream separators or characters that Windows rejects in names.
bool isValidRelativePath(const std::string& rel);

// User data that an update never touches, whatever the package contains: the portable marker,
// the settings folder, caches, box art, logs, crash dumps and the update working folder.
bool isProtectedPath(const std::string& rel);

// Reads and validates a file list. Protected paths in the list are an error.
bool readFileList(const fs::path& file, std::vector<std::string>* paths, std::string* error);

// Checks an extracted package: a valid file list, every listed file present as a regular file,
// no file that is neither listed nor protected, no links, and NyxPlay.exe among the files.
bool validateStage(const fs::path& stageDir, std::vector<std::string>* files, std::string* error);

// Replaces the program files in installDir with those in stageDir (listed in its file list).
// Files of the old version that are in the old file list but not in the new one are moved away.
// On any failure the install folder is restored to its previous state.
Outcome apply(const Options& options);

// Waits until the process with the given id has exited. True when it is gone or never existed.
bool waitForProcessExit(unsigned long pid, unsigned long timeoutMs);

}
