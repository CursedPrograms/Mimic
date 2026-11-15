// Mimic - batch: see batch.h
#include "batch.h"
#include "picture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace Batch {

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
bool isPicture(const std::string& path) {
    const std::string e = lower(fs::u8path(path).extension().u8string());
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga";
}
bool isTarget(const std::string& path) { return isPicture(path) || Video::isVideo(path); }
static bool isResult(const fs::path& p) { const std::string s = lower(p.stem().u8string()); return s.size() > 6 && s.compare(s.size() - 6, 6, "_mimic") == 0; }

std::vector<std::string> collect(const std::vector<std::string>& paths, bool subfolders) {
    std::vector<std::string> out; std::set<std::string> seen;
    auto add = [&](const fs::path& p) {
        const std::string s = p.u8string();
        if (!isTarget(s) || isResult(p)) return;
        std::error_code ec; const std::string key = lower(fs::weakly_canonical(p, ec).u8string());
        if (seen.insert(key).second) out.push_back(s);
    };
    for (const auto& path : paths) {
        const fs::path p = fs::u8path(path);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> files;
            if (subfolders) { for (const auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) if (e.is_regular_file(ec)) files.push_back(e.path()); }
            else for (const auto& e : fs::directory_iterator(p, ec)) if (e.is_regular_file(ec)) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto& f : files) add(f);
        } else add(p);
    }
    return out;
}

std::string outputFor(const std::string& target, const std::string& outDir, const std::string& suffix, const std::string& root) {
    const fs::path t = fs::u8path(target);
    const std::string ext = Video::isVideo(target) ? ".mp4" : t.extension().u8string();
    fs::path dir = outDir.empty() ? t.parent_path() : fs::u8path(outDir);
    if (!outDir.empty() && !root.empty()) {   // the same subfolder as under the folder it came from
        std::error_code ec; const fs::path rel = fs::relative(t.parent_path(), fs::u8path(root), ec);
        if (!ec && !rel.empty() && rel.u8string() != "." && rel.u8string().rfind("..", 0) != 0) dir /= rel;
    }
    return (dir / fs::u8path(t.stem().u8string() + suffix + ext)).u8string();
}

const char* stateName(int s) {
    static const char* n[] = {"waiting", "working", "done", "skipped", "failed", "stopped"};
    return s >= 0 && s <= STOPPED ? n[s] : "";
}

bool processOne(const std::string& in, const std::string& out, const Mimic::Source& src, const Mimic::Settings& st,
                const Mimic::Reference& ref, const Options& opt, const std::atomic<bool>& stop, const std::atomic<bool>& skip,
                const std::function<void(float, const std::string&)>& progress, int& faces, std::string& err) {
    faces = 0;
    std::error_code ec; fs::create_directories(fs::u8path(out).parent_path(), ec);
    if (!Video::isVideo(in)) {
        Img im;
        if (!loadPicture(in, im, err)) return false;
        progress(0.3f, "swapping");
        faces = Mimic::processFrame(im, src, st, ref, err);
        if (faces < 0) return false;
        if (!savePicture(out, im, err, opt.jpegQuality)) return false;
        progress(1, "saved");
        return true;
    }
    if (Video::ffmpeg().empty()) { err = "videos need ffmpeg (next to Mimic or on the PATH)"; return false; }
    const Video::Output& vo = opt.video;
    Video::Reader rd;
    if (!rd.open(in, err)) return false;
    if (vo.trimStart > 0) { rd.close(); if (!rd.open(in, err, vo.trimStart / rd.info.fps)) return false; }
    const long total = vo.trimEnd > vo.trimStart ? vo.trimEnd - vo.trimStart : rd.info.frames - vo.trimStart;
    Video::Writer wr;
    if (!wr.open(out, rd.info.w, rd.info.h, rd.info.fps, in, vo, err)) return false;
    Img frame; long done = 0;
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = true;
    while (!stop && !skip && done < total && rd.next(frame)) {
        const int n = Mimic::processFrame(frame, src, st, ref, err);
        if (n < 0) { ok = false; break; }
        if (!faces && n > 0) faces = n;
        if (!wr.write(frame)) { err = "ffmpeg stopped taking frames"; ok = false; break; }
        ++done;
        if (done % 5 == 0 || done == total) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fps = done / std::max(0.001, s);
            char msg[160]; snprintf(msg, sizeof msg, "frame %ld / %ld, %.1f fps, about %.0f s to go", done, total, fps, total > done ? (total - done) / std::max(0.01, fps) : 0.0);
            progress(total > 0 ? (float)done / total : 0.0f, msg);
        }
    }
    rd.close();
    const bool closed = wr.close();
    if (ok && !closed && !stop && !skip) { err = "ffmpeg couldn't finish " + fs::u8path(out).filename().u8string(); return false; }
    if (skip && !stop) { std::error_code rm; fs::remove(fs::u8path(out), rm); }   // skipped: no half video left behind
    return ok;
}

}
