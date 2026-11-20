// NyxPlayUpdater.exe --wait-pid <pid> --install-dir <dir> --stage-dir <dir> --result-file <file> --version <v>
//
// Started by the client once the update has been downloaded, verified and extracted. Waits for the
// client to exit, swaps the files (rolling back on failure), reports the outcome in the result file
// and starts the client again. The result file holds one status line ("ok <version>" or
// "failed <code> <detail>") followed by "log " lines.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

#include "applyupdate.h"

namespace fs = std::filesystem;

namespace {

std::string toUtf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    }
    return s;
}

// A single line of plain text; keeps the result file parseable whatever a path contains.
std::string oneLine(std::string s)
{
    for (auto& c : s) {
        if (c == '\r' || c == '\n' || (unsigned char)c < 0x20) {
            c = ' ';
        }
    }
    return s.substr(0, 300);
}

void writeResult(const fs::path& file, const std::string& status, const std::vector<std::string>& log)
{
    if (file.empty()) {
        return;
    }
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << oneLine(status) << "\n";
        for (const auto& l : log) {
            out << "log " << oneLine(l) << "\n";
        }
    }
    MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool startClient(const fs::path& installDir)
{
    const fs::path exe = installDir / L"NyxPlay.exe";
    std::wstring cmd = L"\"" + exe.native() + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, installDir.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::map<std::wstring, std::wstring> args;
    for (int i = 1; argv && i + 1 < argc; i += 2) {
        args[argv[i]] = argv[i + 1];
    }
    if (argv) {
        LocalFree(argv);
    }

    const fs::path resultFile = args[L"--result-file"];
    const std::wstring version = args[L"--version"];

    // The updater has no privileges of its own, but it still refuses anything that is not exactly what the client passes
    std::error_code ec;
    const fs::path install = fs::absolute(args[L"--install-dir"], ec).lexically_normal();
    const fs::path stage = fs::absolute(args[L"--stage-dir"], ec).lexically_normal();
    bool valid = !install.empty() && fs::is_regular_file(install / L"NyxPlay.exe", ec) &&
                 stage == (install / L".nyxupdate" / L"stage").lexically_normal() &&
                 resultFile.is_absolute() && resultFile.extension() == L".txt" &&
                 !args[L"--wait-pid"].empty() && args[L"--wait-pid"].find_first_not_of(L"0123456789") == std::wstring::npos &&
                 !version.empty() && version.size() <= 32 && version.find_first_not_of(L"0123456789.") == std::wstring::npos;
    if (!valid) {
        writeResult(resultFile.is_absolute() ? resultFile : fs::path(), "failed invalid-arguments", {});
        return 2;
    }

    std::vector<std::string> log;
    if (!NyxUpdater::waitForProcessExit(wcstoul(args[L"--wait-pid"].c_str(), nullptr, 10), 60000)) {
        writeResult(resultFile, "failed wait-timeout", { "the client did not exit" });
        return 3;
    }

    NyxUpdater::Options options;
    options.installDir = install;
    options.stageDir = stage;
    NyxUpdater::Outcome outcome = NyxUpdater::apply(options);
    log = outcome.log;

    fs::remove_all(stage, ec);

    if (outcome.ok) {
        writeResult(resultFile, "ok " + toUtf8(version), log);
    }
    else {
        writeResult(resultFile, "failed " + outcome.code + (outcome.detail.empty() ? "" : " " + outcome.detail), log);
    }

    // The client comes back in both cases: the new version, or the restored old one
    if (!startClient(install)) {
        return 4;
    }
    return outcome.ok ? 0 : 1;
}
