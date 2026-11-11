// Mimic - video through ffmpeg (see video.h)
#include "video.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define popenW(c, m) _wpopen(c, m)
#define pcloseW _pclose
#endif

namespace fs = std::filesystem;

namespace Video {

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

std::string ffmpeg() {
    static std::string found;
    static bool looked = false;
    if (looked) return found;
    looked = true;
    std::error_code ec;
#ifdef _WIN32
    const char* name = "ffmpeg.exe";
    std::vector<fs::path> spots = {fs::u8path(exeDir()) / name, fs::u8path(exeDir()) / "ffmpeg" / "bin" / name, "C:/ffmpeg/bin/ffmpeg.exe"};
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr)) spots.insert(spots.begin() + 2, fs::path(buf));
    if (const char* la = std::getenv("LOCALAPPDATA")) {   // winget's Gyan.FFmpeg
        const fs::path pk = fs::u8path(la) / "Microsoft" / "WinGet" / "Packages";
        if (fs::exists(pk, ec))
            for (const auto& d : fs::directory_iterator(pk, ec))
                if (d.path().filename().u8string().rfind("Gyan.FFmpeg", 0) == 0)
                    for (const auto& b : fs::directory_iterator(d.path(), ec)) spots.push_back(b.path() / "bin" / name);
    }
#else
    std::vector<fs::path> spots = {fs::u8path(exeDir()) / "ffmpeg", "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg"};
#endif
    for (const auto& p : spots) if (fs::exists(p, ec)) { found = p.u8string(); break; }
    return found;
}

static std::string ffprobe() {
    const std::string f = ffmpeg();
    if (f.empty()) return "";
    fs::path p = fs::u8path(f).parent_path() / fs::u8path(f).filename().u8string().replace(0, 6, "ffprobe");
    std::error_code ec;
    return fs::exists(p, ec) ? p.u8string() : "";
}

bool isVideo(const std::string& path) {
    std::string e = fs::u8path(path).extension().u8string();
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (const char* v : {".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".gif", ".wmv"}) if (e == v) return true;
    return false;
}

static std::wstring q(const std::string& s) { return L"\"" + fs::u8path(s).wstring() + L"\""; }

#ifdef _WIN32
// _wpopen runs through cmd /c, which strips the outer quotes: the whole line goes in one more pair
static FILE* run(const std::wstring& cmd, const wchar_t* mode) { return popenW((L"\"" + cmd + L"\"").c_str(), mode); }
#endif

bool probe(const std::string& path, Info& info, std::string& err) {
    const std::string fp = ffprobe();
    if (fp.empty()) { err = "ffprobe (with ffmpeg) isn't on this PC"; return false; }
    FILE* p = run(q(fp) + L" -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate,nb_frames:format=duration -of default=nw=1 " + q(path), L"r");
    if (!p) { err = "Can't run ffprobe"; return false; }
    char line[512];
    while (fgets(line, sizeof line, p)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "width") info.w = atoi(v.c_str());
        else if (k == "height") info.h = atoi(v.c_str());
        else if (k == "nb_frames") info.frames = atol(v.c_str());
        else if (k == "duration") info.seconds = atof(v.c_str());
        else if (k == "r_frame_rate") { const size_t sl = v.find('/'); const double a = atof(v.c_str()), b = sl == std::string::npos ? 1 : atof(v.c_str() + sl + 1); info.fps = b > 0 ? a / b : 0; }
    }
    pcloseW(p);
    if (info.w <= 0 || info.h <= 0) { err = "Not a video ffmpeg can read: " + path; return false; }
    if (info.fps <= 0) info.fps = 25;
    if (info.frames <= 0 && info.seconds > 0) info.frames = (long)(info.seconds * info.fps + 0.5);
    return true;
}

bool Reader::open(const std::string& path, std::string& err, double start) {
    close();
    if (!probe(path, info, err)) return false;
    wchar_t ss[64] = L"";
    if (start > 0) swprintf(ss, 64, L" -ss %.3f", start);
    pipe = run(q(ffmpeg()) + L" -v error" + ss + L" -i " + q(path) + L" -f rawvideo -pix_fmt rgb24 -", L"rb");
    if (!pipe) { err = "Can't run ffmpeg"; return false; }
    return true;
}

bool Reader::next(Img& frame) {
    if (!pipe) return false;
    if (frame.w != info.w || frame.h != info.h) frame = Img(info.w, info.h);
    return fread(frame.px.data(), 1, frame.px.size(), pipe) == frame.px.size();
}

void Reader::close() { if (pipe) { pcloseW(pipe); pipe = nullptr; } }

bool Writer::open(const std::string& out, int w_, int h_, double fps, const std::string& audioFrom, int crf, std::string& err) {
    close();
    w = w_; h = h_;
    wchar_t head[256];
    swprintf(head, 256, L" -v error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r %.6f -i -", w, h, fps);
    wchar_t enc[160];
    // H.264 needs even sizes: the odd pixel is cropped off
    swprintf(enc, 160, L" -map 0:v:0 -map 1:a? -vf crop=trunc(iw/2)*2:trunc(ih/2)*2 -c:v libx264 -preset medium -crf %d -pix_fmt yuv420p -c:a copy -shortest ", crf);
    pipe = run(q(ffmpeg()) + head + L" -i " + q(audioFrom) + enc + q(out), L"wb");
    if (!pipe) { err = "Can't run ffmpeg to write " + out; return false; }
    return true;
}

bool Writer::write(const Img& frame) { return pipe && fwrite(frame.px.data(), 1, frame.px.size(), pipe) == frame.px.size(); }

bool Writer::close() {
    if (!pipe) return true;
    const int rc = pcloseW(pipe);
    pipe = nullptr;
    return rc == 0;
}

bool frameAt(const std::string& path, double seconds, Img& frame, std::string& err) {
    Reader r;
    if (!r.open(path, err, seconds)) return false;
    const bool got = r.next(frame);
    r.close();
    if (!got) err = "No frame there";
    return got;
}

}  // namespace Video
