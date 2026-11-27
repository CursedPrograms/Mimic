// Mimic - Inoculant through its command line (see inoculant.h)
#include "inoculant.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace Inoculant {

static std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().u8string();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path().u8string();
#endif
}

std::string cli() {
    static std::string found;
    static bool looked = false;
    if (looked) return found;
    looked = true;
    std::error_code ec;
#ifdef _WIN32
    const char* name = "inoculant-cli.exe";
#else
    const char* name = "inoculant-cli";
#endif
    std::vector<fs::path> spots;
    if (const char* env = std::getenv("INOCULANT_CLI")) spots.push_back(fs::u8path(env));
    spots.push_back(fs::u8path(exeDir()) / name);
    spots.push_back(fs::u8path(exeDir()) / ".." / "Inoculant" / name);
#ifdef _WIN32
    if (const char* la = std::getenv("LOCALAPPDATA")) spots.push_back(fs::u8path(la) / "Programs" / "Inoculant" / name);
    spots.push_back("D:/cc/Inoculant/dist/Inoculant/inoculant-cli.exe");
#else
    spots.push_back("/usr/local/bin/inoculant-cli");
    if (const char* home = std::getenv("HOME")) spots.push_back(fs::u8path(home) / ".local" / "bin" / name);
#endif
    for (const auto& p : spots)
        if (fs::is_regular_file(p, ec)) { found = fs::absolute(p, ec).u8string(); break; }
    return found;
}

std::string tempPath(const char* ext) {
    static std::atomic<int> n{0};
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "mimic-inoculant";
    fs::create_directories(dir, ec);
    return (dir / ("pic" + std::to_string(
#ifdef _WIN32
        GetCurrentProcessId()
#else
        0
#endif
        ) + "_" + std::to_string(++n) + ext)).u8string();
}

static std::string num(float v) { char b[32]; std::snprintf(b, sizeof b, "%g", v); return b; }

// Run the command line hidden; stdout + stderr come back in `out`. Returns the exit code (-1: couldn't start).
static int run(const std::vector<std::string>& args, std::string& out) {
    out.clear();
#ifdef _WIN32
    std::wstring line;
    for (const auto& a : args) {
        const std::wstring w = fs::u8path(a).wstring();
        line += L"\"" + w + L"\" ";
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = fs::u8path(args[0]).parent_path().wstring();
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); return -1;
    }
    CloseHandle(wr);
    char buf[4096]; DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
#else
    std::string line;
    for (const auto& a : args) line += "'" + a + "' ";
    line += "2>&1";
    FILE* f = popen(line.c_str(), "r");
    if (!f) return -1;
    char buf[4096]; size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
    return pclose(f);
#endif
}

static std::string lastLine(const std::string& s) {
    size_t e = s.find_last_not_of("\r\n ");
    if (e == std::string::npos) return "Inoculant failed";
    size_t b = s.find_last_of('\n', e);
    return s.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1) + 1);
}

static bool missing(std::string& err) {
    if (!cli().empty()) return false;
    err = "Inoculant isn't installed: get it from github.com/CursedPrograms/Inoculant (or set INOCULANT_CLI)";
    return true;
}

bool protect(const std::string& in, const std::string& out, const Poison& p, std::string& report, std::string& err) {
    if (missing(err)) return false;
    std::string text;
    const int code = run({cli(), "protect", in, out, "--strength", num(p.strength), "--steps", std::to_string(p.steps),
                          "--texture", num(p.texture), "--colour", num(p.colour), "--robustness", num(p.robustness)}, text);
    if (code != 0) { err = "Inoculant: " + lastLine(text); return false; }
    // the summary lines, without the progress lines before them
    const size_t s = text.find(" step(s)");
    const size_t b = s == std::string::npos ? 0 : text.rfind('\n', s);
    report = text.substr(b == std::string::npos ? 0 : b + 1);
    while (!report.empty() && (report.back() == '\n' || report.back() == '\r')) report.pop_back();
    return true;
}

bool cure(const std::string& in, const std::string& out, const Cure& c, std::string& verdict, std::string& err) {
    if (missing(err)) return false;
    std::string text;
    if (run({cli(), "inspect", in}, text) == 0) {
        verdict = lastLine(text);
    } else {
        verdict = "(not inspected)";
    }
    std::vector<std::string> args = {cli(), "purify", in, out, "--jpeg", std::to_string(c.jpeg)};
    if (c.blur > 0) { args.push_back("--blur"); args.push_back(num(c.blur)); }
    if (c.shrink < 100) { args.push_back("--resize"); args.push_back(std::to_string(c.shrink)); }
    if (run(args, text) != 0) { err = "Inoculant: " + lastLine(text); return false; }
    return true;
}

}  // namespace Inoculant
