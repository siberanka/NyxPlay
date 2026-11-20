#include "applyupdate.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

namespace NyxUpdater {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

fs::path toPath(const std::string& utf8Rel)
{
    // Relative paths in file lists are UTF-8 with '/' separators.
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8Rel.data(), (int)utf8Rel.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8Rel.data(), (int)utf8Rel.size(), &w[0], n);
    }
    for (auto& c : w) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    return fs::path(w);
}

std::string toUtf8(const fs::path& p)
{
    const std::wstring& w = p.native();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    }
    return s;
}

std::string relUtf8(const fs::path& base, const fs::path& file)
{
    std::string rel = toUtf8(file.lexically_relative(base));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

enum class OpKind { RenamedAway, MovedIn, CreatedDir };

struct Op {
    OpKind kind;
    fs::path a;   // RenamedAway: original path, MovedIn: target path, CreatedDir: directory
    fs::path b;   // RenamedAway: backup path, MovedIn: staged source
};

bool moveWithRetry(const fs::path& from, const fs::path& to, const Options& o, DWORD* error)
{
    for (int attempt = 0;; attempt++) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        *error = GetLastError();
        const bool transient = *error == ERROR_SHARING_VIOLATION || *error == ERROR_ACCESS_DENIED ||
                               *error == ERROR_LOCK_VIOLATION;
        if (!transient || attempt >= o.renameRetries) {
            return false;
        }
        Sleep((DWORD)o.retryDelayMs);
    }
}

bool removeWithRetry(const fs::path& file, const Options& o)
{
    for (int attempt = 0;; attempt++) {
        if (DeleteFileW(file.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (attempt >= o.renameRetries) {
            return false;
        }
        Sleep((DWORD)o.retryDelayMs);
    }
}

bool fileExists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}

// Every directory that has to be created for the file, outermost first.
std::vector<fs::path> missingParents(const fs::path& installDir, const fs::path& target)
{
    std::vector<fs::path> missing;
    for (fs::path p = target.parent_path(); p != installDir && p.has_relative_path(); p = p.parent_path()) {
        if (fileExists(p)) {
            break;
        }
        missing.push_back(p);
    }
    std::reverse(missing.begin(), missing.end());
    return missing;
}

void rollback(std::vector<Op>& ops, const Options& o, Outcome* out)
{
    bool incomplete = false;
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        DWORD err = 0;
        switch (it->kind) {
        case OpKind::MovedIn:
            // Put the staged file back; deleting it is as good, the stage is thrown away anyway
            if (!moveWithRetry(it->a, it->b, o, &err) && !removeWithRetry(it->a, o)) {
                incomplete = true;
                out->log.push_back("rollback: cannot remove " + toUtf8(it->a.filename()));
            }
            break;
        case OpKind::RenamedAway:
            if (!moveWithRetry(it->b, it->a, o, &err)) {
                incomplete = true;
                out->log.push_back("rollback: cannot restore " + toUtf8(it->a.filename()));
            }
            break;
        case OpKind::CreatedDir:
            RemoveDirectoryW(it->a.c_str());
            break;
        }
    }
    out->log.push_back(incomplete ? "rollback incomplete" : "rolled back");
    if (incomplete) {
        out->code = "rollback-incomplete";
    }
}

}

bool isValidRelativePath(const std::string& rel)
{
    if (rel.empty() || rel.size() > 240 || rel.front() == '/' || rel.back() == '/') {
        return false;
    }
    size_t start = 0;
    while (start <= rel.size()) {
        size_t end = rel.find('/', start);
        if (end == std::string::npos) {
            end = rel.size();
        }
        const std::string seg = rel.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ') {
            return false;
        }
        start = end + 1;
    }
    for (unsigned char c : rel) {
        if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

bool isProtectedPath(const std::string& relIn)
{
    const std::string rel = lower(relIn);
    if (rel == "portable.dat") {
        return true;
    }
    for (const char* dir : { "nyxplay/", "cache/", "boxart/", "qmlcache/", ".nyxupdate/" }) {
        if (startsWith(rel, dir)) {
            return true;
        }
    }
    if (rel.find('/') == std::string::npos && startsWith(rel, "nyxplay-") &&
            (endsWith(rel, ".log") || endsWith(rel, ".dmp"))) {
        return true;
    }
    return false;
}

bool readFileList(const fs::path& file, std::vector<std::string>* paths, std::string* error)
{
    paths->clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        *error = "cannot read " + toUtf8(file.filename());
        return false;
    }
    std::set<std::string> seen;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (!isValidRelativePath(line)) {
            *error = "invalid path in file list: " + line;
            return false;
        }
        if (isProtectedPath(line)) {
            *error = "protected path in file list: " + line;
            return false;
        }
        if (!seen.insert(lower(line)).second) {
            *error = "duplicate path in file list: " + line;
            return false;
        }
        paths->push_back(line);
        if (paths->size() > 20000) {
            *error = "file list too long";
            return false;
        }
    }
    return true;
}

bool validateStage(const fs::path& stageIn, std::vector<std::string>* fresh, std::string* error)
{
    std::error_code ec;
    const fs::path stage = fs::absolute(stageIn, ec).lexically_normal();
    if (!fs::is_directory(stage, ec)) {
        *error = "no stage folder";
        return false;
    }
    if (!readFileList(stage / FileListName, fresh, error)) {
        return false;
    }
    std::set<std::string> keys;
    for (const auto& rel : *fresh) {
        keys.insert(lower(rel));
        std::error_code e;
        const fs::path source = stage / toPath(rel);
        if (fs::is_symlink(fs::symlink_status(source, e)) || !fs::is_regular_file(source, e)) {
            *error = "missing " + rel;
            return false;
        }
    }
    for (fs::recursive_directory_iterator it(stage, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        const fs::file_status st = it->symlink_status(e);
        if (fs::is_symlink(st)) {
            *error = "link " + relUtf8(stage, it->path());
            return false;
        }
        if (!fs::is_regular_file(st)) {
            continue;
        }
        const std::string rel = relUtf8(stage, it->path());
        if (lower(rel) != lower(FileListName) && !isProtectedPath(rel) && !keys.count(lower(rel))) {
            *error = "unlisted " + rel;
            return false;
        }
    }
    if (ec || !keys.count("nyxplay.exe")) {
        *error = "NyxPlay.exe";
        return false;
    }
    return true;
}

Outcome apply(const Options& o)
{
    Outcome out;
    std::error_code ec;
    const fs::path install = fs::absolute(o.installDir, ec).lexically_normal();
    const fs::path stage = fs::absolute(o.stageDir, ec).lexically_normal();

    if (!fs::is_regular_file(install / L"NyxPlay.exe", ec)) {
        out.detail = "NyxPlay.exe";
        return out;
    }
    if (!fs::is_directory(stage, ec) || stage == install) {
        out.code = "stage-invalid";
        return out;
    }

    // Plan: nothing is touched until the new file list and the stage agree
    std::vector<std::string> fresh;
    std::string error;
    if (!validateStage(stage, &fresh, &error)) {
        out.code = "stage-invalid";
        out.detail = error;
        return out;
    }
    std::set<std::string> freshKeys;
    for (const auto& rel : fresh) {
        freshKeys.insert(lower(rel));
    }

    std::vector<std::string> old;
    const fs::path oldList = install / FileListName;
    if (fileExists(oldList) && !readFileList(oldList, &old, &error)) {
        // An unreadable old list only means that nothing gets removed
        out.log.push_back("old file list ignored: " + error);
        old.clear();
    }

    std::vector<Op> ops;
    auto fail = [&](const char* code, const std::string& detail) {
        out.code = code;
        out.detail = detail;
        out.log.push_back(std::string("failed: ") + code + " " + detail);
        rollback(ops, o, &out);
        return out;
    };

    // The file list goes last so that an interrupted run never leaves a list that does not match the files
    std::vector<std::string> order = fresh;
    std::sort(order.begin(), order.end());
    order.push_back(FileListName);

    for (const auto& rel : order) {
        const fs::path target = install / toPath(rel);
        const fs::path source = stage / toPath(rel);
        DWORD err = 0;

        for (const fs::path& dir : missingParents(install, target)) {
            if (!CreateDirectoryW(dir.c_str(), nullptr)) {
                return fail("write-failed", rel);
            }
            ops.push_back({ OpKind::CreatedDir, dir, {} });
        }

        if (fileExists(target)) {
            const fs::path backup = target.native() + BackupSuffix;
            if (!removeWithRetry(backup, o)) {
                return fail("locked", rel);
            }
            if (!moveWithRetry(target, backup, o, &err)) {
                return fail("locked", rel);
            }
            ops.push_back({ OpKind::RenamedAway, target, backup });
        }
        if (!moveWithRetry(source, target, o, &err)) {
            return fail("write-failed", rel);
        }
        ops.push_back({ OpKind::MovedIn, target, source });
    }

    for (const auto& rel : old) {
        if (freshKeys.count(lower(rel)) || lower(rel) == lower(FileListName)) {
            continue;
        }
        const fs::path target = install / toPath(rel);
        std::error_code e;
        if (!fs::is_regular_file(target, e)) {
            continue;
        }
        const fs::path backup = target.native() + BackupSuffix;
        DWORD err = 0;
        if (!removeWithRetry(backup, o) || !moveWithRetry(target, backup, o, &err)) {
            return fail("locked", rel);
        }
        ops.push_back({ OpKind::RenamedAway, target, backup });
        out.log.push_back("removed " + rel);
    }

    out.ok = true;
    out.code = "ok";
    out.detail.clear();
    out.log.push_back("replaced " + std::to_string(order.size()) + " files");
    return out;
}

bool waitForProcessExit(unsigned long pid, unsigned long timeoutMs)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) {
        // Gone already, or not ours to wait for: access denied is treated as "still there"
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const DWORD r = WaitForSingleObject(h, timeoutMs);
    CloseHandle(h);
    return r == WAIT_OBJECT_0;
}

}
