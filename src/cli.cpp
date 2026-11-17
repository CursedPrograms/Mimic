// mimic-cli - Mimic on the command line: FaceFusion's face processors on a picture or a video. The options are
// FaceFusion's own (--face-swapper-model, --face-mask-types, ...), so its commands carry over.
//
//   mimic-cli -s face.jpg [-s more.jpg] -t target.jpg|video.mp4 -o out.jpg|out.mp4 [options]
#include "batch.h"
#include "mimic.h"
#include "picture.h"
#include "video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

static std::string modelKey(const char* file) { std::string f = file; return f.substr(0, f.size() - 5); }   // "inswapper_128"

static void usage() {
    std::printf(
        "mimic-cli - FaceFusion's face processors in C++: swap, enhance, debug, on pictures and videos. Part of Atelier.\n\n"
        "  mimic-cli -s face.jpg [-s more.jpg] -t target.jpg|video.mp4 [-o out] [options]\n\n"
        "  -s, --source FILE                the face to become (several of one person: their average)\n"
        "  -t, --target FILE...             the picture or video; several, or folders: a batch\n"
        "  -o, --output FILE                where the result goes (default <target>_mimic.<ext>); a batch: a folder\n"
        "  --subfolders                     a batch: folders' subfolders too (kept in the output folder)\n"
        "  --skip-existing                  a batch: leave results already there (carry on a stopped batch)\n"
        "  --suffix TEXT                    a batch: the results' name ending (default _mimic)\n"
        "  --processors P...                face_swapper face_editor face_enhancer face_debugger (default: swapper + enhancer)\n\n"
        "  --face-editor-prompt TEXT        an expression in words: \"a big smile, eyes closed, looking left\"\n"
        "  --face-editor-mouth-smile X      -1..1, and likewise: eyebrow-direction eye-gaze-horizontal eye-gaze-vertical\n"
        "                                   eye-open-ratio lip-open-ratio mouth-grim mouth-pout mouth-purse\n"
        "                                   mouth-position-horizontal mouth-position-vertical head-pitch head-yaw head-roll\n\n"
        "  --face-swapper-model M           ");
    for (const auto& m : Mimic::swapModels()) std::printf("%s ", modelKey(m.file).c_str());
    std::printf("\n  --face-swapper-pixel-boost N    1..4 (x the model's size)\n"
                "  --face-swapper-weight X          0..1: 0 more of the source, 1 more of the target's own (default 0.5)\n"
                "  --face-enhancer-model M          ");
    for (const auto& m : Mimic::enhanceModels()) std::printf("%s ", modelKey(m.file).c_str());
    std::printf("\n  --face-enhancer-blend N          0..100 (default 80)\n"
                "  --face-enhancer-weight X         0..1 (CodeFormer's fidelity, default 0.5)\n"
                "  --face-debugger-items I...       bounding-box face-landmark-5 face-landmark-5/68 face-landmark-68 face-landmark-68/5 face-mask\n\n"
                "  --face-detector-model M          retinaface scrfd yolo_face yunet many\n"
                "  --face-detector-size S           160x160 320x320 480x480 512x512 640x640\n"
                "  --face-detector-angles A...      0 90 180 270\n"
                "  --face-detector-score X          0..1 (default 0.5)\n"
                "  --face-detector-margin T R B L   0..100 each\n"
                "  --face-landmarker-model M        2dfan4 peppa_wutz many\n"
                "  --face-landmarker-score X        0..1 (default 0.5; 0 never uses its points)\n\n"
                "  --face-selector-mode M           many one reference\n"
                "  --face-selector-order O          left-right right-left top-bottom bottom-top small-large large-small best-worst worst-best\n"
                "  --face-selector-gender G         auto female male\n"
                "  --face-selector-race R           auto white black latino asian indian arabic\n"
                "  --face-selector-age-start N      0..100\n"
                "  --face-selector-age-end N        0..100\n"
                "  --reference-face FILE            a picture of the face to replace (mode reference)\n"
                "  --reference-face-distance X      0..1 (default 0.3)\n\n"
                "  --face-mask-types T...           box occlusion area region (default box)\n"
                "  --face-occluder-model M          xseg_1 xseg_2 xseg_3 many\n"
                "  --face-parser-model M            bisenet_resnet_18 bisenet_resnet_34\n"
                "  --face-mask-areas A...           upper-face lower-face mouth\n"
                "  --face-mask-regions R...         skin left-eyebrow right-eyebrow left-eye right-eye glasses nose mouth upper-lip lower-lip\n"
                "  --face-mask-blur X               0..1 (default 0.3)\n"
                "  --face-mask-padding T R B L      0..100 each\n\n"
                "  --output-image-quality N         0..100 (default 95)\n"
                "  --output-video-encoder E         libx264 libx265 libvpx-vp9 h264_nvenc hevc_nvenc h264_amf hevc_amf h264_qsv hevc_qsv\n"
                "  --output-video-preset P          ultrafast .. veryslow (default veryfast)\n"
                "  --output-video-quality N         0..100 (default 80)\n"
                "  --output-video-fps X             (default: the target's)\n"
                "  --output-audio-encoder E         aac libmp3lame libopus libvorbis flac copy\n"
                "  --output-audio-quality N         0..100\n"
                "  --output-audio-volume N          0..100\n"
                "  --trim-frame-start N / --trim-frame-end N\n"
                "  --execution-providers cpu        don't use the GPU (default: DirectML)\n\n"
                "Only use faces you have the right to use.\n");
}

static int run(const std::vector<std::string>& args) {
    std::vector<std::string> sources;
    std::string target, out, reference, suffix = "_mimic";
    std::vector<std::string> targets;
    bool subfolders = false, skipExisting = false;
    Mimic::Settings st;
    Video::Output vo;
    bool cpu = false;
    int imageQuality = 95;
    // an option's values: up to the next "--"
    auto values = [&](size_t& i) { std::vector<std::string> v; while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0 && args[i + 1] != "-s" && args[i + 1] != "-t" && args[i + 1] != "-o") v.push_back(args[++i]); return v; };
    auto one = [&](size_t& i) { auto v = values(i); return v.empty() ? std::string() : v[0]; };
    auto num = [&](size_t& i) { return (float)std::atof(one(i).c_str()); };
    auto index = [](const std::string& v, std::initializer_list<const char*> names) { int k = 0; for (const char* n : names) { if (v == n) return k; ++k; } return -1; };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-s" || a == "--source" || a == "--source-paths") for (auto& v : values(i)) sources.push_back(v);
        else if (a == "-t" || a == "--target" || a == "--target-path") for (auto& v : values(i)) targets.push_back(v);
        else if (a == "--subfolders" || a == "--recursive") subfolders = true;
        else if (a == "--skip-existing") skipExisting = true;
        else if (a == "--suffix") suffix = one(i);
        else if (a == "-o" || a == "--output" || a == "--output-path") out = one(i);
        else if (a == "--processors") { st.swap = st.enhance = st.debug = false; for (auto& v : values(i)) { if (v == "face_swapper") st.swap = true; else if (v == "face_enhancer") st.enhance = true; else if (v == "face_debugger") st.debug = true; else if (v == "face_editor") st.edit = true; } }
        else if (a == "--face-swapper-model") { const std::string v = one(i); for (size_t k = 0; k < Mimic::swapModels().size(); ++k) if (modelKey(Mimic::swapModels()[k].file) == v) st.swapModel = (int)k; }
        else if (a == "--face-swapper-pixel-boost") { const std::string v = one(i); const int px = std::atoi(v.c_str()); const int size = Mimic::swapModels()[st.swapModel].size; st.boost = std::clamp(px > 8 ? px / size : px, 1, 4); }
        else if (a == "--face-swapper-weight") st.weight = std::clamp(num(i), 0.0f, 1.0f);
        // the swap's style (Mimic's own): classic or deep-live-cam, and the Deep-Live-Cam style's settings
        else if (a == "--face-swapper-style") st.style = one(i) == "deep-live-cam" ? 1 : 0;
        else if (a == "--dlc-colour" || a == "--dlc-color") st.dlcColour = one(i) != "off";
        else if (a == "--dlc-poisson") st.dlcPoisson = one(i) != "off";
        else if (a == "--dlc-mouth") { st.dlcMouth = true; st.dlcMouthSize = std::clamp(num(i) / 100.0f, 0.0f, 1.0f); }
        else if (a == "--dlc-opacity") st.dlcOpacity = std::clamp(num(i) / 100.0f, 0.0f, 1.0f);
        else if (a == "--dlc-sharpness") st.dlcSharpness = std::clamp(num(i) / 100.0f, 0.0f, 1.0f);
        else if (a == "--dlc-smoothing") st.dlcSmooth = std::clamp(num(i) / 100.0f, 0.0f, 1.0f);
        else if (a == "--face-enhancer-model") { const std::string v = one(i); for (size_t k = 0; k < Mimic::enhanceModels().size(); ++k) if (modelKey(Mimic::enhanceModels()[k].file) == v) st.enhanceModel = (int)k; }
        else if (a == "--face-enhancer-blend") st.enhanceBlend = std::clamp(num(i) / 100.0f, 0.0f, 1.0f);
        else if (a == "--face-enhancer-weight") st.enhanceWeight = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--face-debugger-items") { for (auto& b : st.debugItems) b = false; for (auto& v : values(i)) { const int k = index(v, {"bounding-box", "face-landmark-5", "face-landmark-5/68", "face-landmark-68", "face-landmark-68/5", "face-mask"}); if (k >= 0) st.debugItems[k] = true; } }
        else if (a == "--face-editor-prompt") {
            std::string p; for (auto& v : values(i)) p += v + " ";
            const std::string got = Mimic::promptToEdits(p, st.edits);
            std::printf("Expression: %s\n", got.empty() ? "(no words it knows)" : got.c_str());
            st.edit = true;
        }
        else if (a.rfind("--face-editor-", 0) == 0) {
            static const char* keys[Mimic::ED_COUNT] = {"eyebrow-direction", "eye-gaze-horizontal", "eye-gaze-vertical", "eye-open-ratio",
                "lip-open-ratio", "mouth-grim", "mouth-pout", "mouth-purse", "mouth-smile", "mouth-position-horizontal",
                "mouth-position-vertical", "head-pitch", "head-yaw", "head-roll"};
            const std::string k = a.substr(14);
            bool known = false;
            for (int e = 0; e < Mimic::ED_COUNT; ++e) if (k == keys[e]) { st.edits[e] = std::clamp(num(i), -1.0f, 1.0f); st.edit = known = true; }
            if (!known) { std::printf("What's %s? (--help lists the options)\n", a.c_str()); return 1; }
        }
        else if (a == "--face-detector-model") { const int k = index(one(i), {"retinaface", "scrfd", "yolo_face", "yunet", "many"}); if (k >= 0) st.detect.model = k; }
        else if (a == "--face-detector-size") st.detect.size = std::atoi(one(i).c_str());
        else if (a == "--face-detector-angles") { for (auto& b : st.detect.angles) b = false; for (auto& v : values(i)) { const int k = std::atoi(v.c_str()) / 90; if (k >= 0 && k < 4) st.detect.angles[k] = true; } }
        else if (a == "--face-detector-score") st.detect.score = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--face-detector-margin") { auto v = values(i); for (int k = 0; k < 4; ++k) st.detect.margin[k] = std::clamp(std::atoi(v.size() > (size_t)k ? v[k].c_str() : (v.empty() ? "0" : v.back().c_str())), 0, 100); }
        else if (a == "--face-landmarker-model") { const int k = index(one(i), {"2dfan4", "peppa_wutz", "many"}); if (k >= 0) st.detect.landmarker = k; }
        else if (a == "--face-landmarker-score") st.detect.landmarkerScore = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--face-selector-mode") { const int k = index(one(i), {"many", "one", "reference"}); if (k >= 0) st.mode = k; }
        else if (a == "--face-selector-order") { const int k = index(one(i), {"left-right", "right-left", "top-bottom", "bottom-top", "small-large", "large-small", "best-worst", "worst-best"}); if (k >= 0) st.order = k; }
        else if (a == "--face-selector-gender") { const std::string v = one(i); st.gender = v == "auto" ? -1 : v == "female" ? G_FEMALE : v == "male" ? G_MALE : -2; }
        else if (a == "--face-selector-race") { const std::string v = one(i); const int k = index(v, {"white", "black", "latino", "asian", "indian", "arabic"}); st.race = v == "auto" ? -1 : k; if (st.race < -1) st.race = -2; }
        else if (a == "--face-selector-age-start") st.ageStart = std::clamp(std::atoi(one(i).c_str()), 0, 100);
        else if (a == "--face-selector-age-end") st.ageEnd = std::clamp(std::atoi(one(i).c_str()), 0, 100);
        else if (a == "--reference-face" || a == "--reference") { reference = one(i); st.mode = Mimic::SEL_REFERENCE; }
        else if (a == "--reference-face-distance") st.referenceDistance = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--face-mask-types") { st.maskBox = st.maskOcclusion = st.maskArea = st.maskRegion = false; for (auto& v : values(i)) { if (v == "box") st.maskBox = true; else if (v == "occlusion") st.maskOcclusion = true; else if (v == "area") st.maskArea = true; else if (v == "region") st.maskRegion = true; } }
        else if (a == "--face-occluder-model") { const int k = index(one(i), {"xseg_1", "xseg_2", "xseg_3", "many"}); if (k >= 0) st.occluder = k; }
        else if (a == "--face-parser-model") st.parser = one(i) == "bisenet_resnet_18" ? 0 : 1;
        else if (a == "--face-mask-areas") { for (auto& b : st.areas) b = false; for (auto& v : values(i)) { const int k = index(v, {"upper-face", "lower-face", "mouth"}); if (k >= 0) st.areas[k] = true; } }
        else if (a == "--face-mask-regions") { for (auto& b : st.regions) b = false; for (auto& v : values(i)) { const int k = index(v, {"skin", "left-eyebrow", "right-eyebrow", "left-eye", "right-eye", "glasses", "nose", "mouth", "upper-lip", "lower-lip"}); if (k >= 0) st.regions[k] = true; } }
        else if (a == "--face-mask-blur") st.maskBlur = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--face-mask-padding") { auto v = values(i); for (int k = 0; k < 4; ++k) st.padding[k] = std::clamp(std::atoi(v.size() > (size_t)k ? v[k].c_str() : (v.empty() ? "0" : v.back().c_str())), 0, 100); }
        else if (a == "--output-image-quality") imageQuality = std::clamp(std::atoi(one(i).c_str()), 1, 100);
        else if (a == "--output-video-encoder") vo.videoEncoder = one(i);
        else if (a == "--output-video-preset") vo.preset = one(i);
        else if (a == "--output-video-quality") vo.videoQuality = std::clamp(std::atoi(one(i).c_str()), 0, 100);
        else if (a == "--output-video-fps") vo.fps = num(i);
        else if (a == "--output-audio-encoder") vo.audioEncoder = one(i);
        else if (a == "--output-audio-quality") vo.audioQuality = std::clamp(std::atoi(one(i).c_str()), 0, 100);
        else if (a == "--output-audio-volume") vo.audioVolume = std::clamp(std::atoi(one(i).c_str()), 0, 100);
        else if (a == "--trim-frame-start") vo.trimStart = std::max(0L, atol(one(i).c_str()));
        else if (a == "--trim-frame-end") vo.trimEnd = std::max(0L, atol(one(i).c_str()));
        else if (a == "--execution-providers") { for (auto& v : values(i)) if (v == "cpu") cpu = true; }
        else if (a == "--cpu") cpu = true;
        else if (a == "-h" || a == "--help" || a == "/?") { usage(); return 0; }
        else { std::printf("What's %s? (--help lists the options)\n", a.c_str()); return 1; }
    }
    if (st.swap && sources.empty() && st.edit) st.swap = false;   // only editing the expression: no face needed
    if (targets.empty() || (st.swap && sources.empty())) { usage(); return 1; }
    bool batch = targets.size() > 1;
    { std::error_code ec; for (const auto& t : targets) if (fs::is_directory(fs::u8path(t), ec)) batch = true; }
    target = targets[0];
    const bool video = Video::isVideo(target);
    if (out.empty() && !batch) {
        const fs::path p = fs::u8path(target);
        out = (p.parent_path() / (p.stem().u8string() + "_mimic" + (video ? ".mp4" : p.extension().u8string()))).u8string();
    }
    auto say = [](const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); };
    std::string err;
    if (!Ort::init(!cpu, err)) { std::printf("%s\n", err.c_str()); return 1; }

    Mimic::Source src;
    for (const auto& s : sources) {
        Img im;
        if (!loadPicture(s, im, err) || !Mimic::addSource(src, im, err, say)) { std::printf("%s: %s\n", s.c_str(), err.c_str()); return 1; }
    }
    Mimic::Reference ref;
    if (!reference.empty()) {
        Img im;
        std::vector<Face> faces;
        if (!loadPicture(reference, im, err) || !Faces::analyse(im, st.detect, faces, err, false) || faces.empty()) { std::printf("Reference: %s\n", err.empty() ? "no face in it" : err.c_str()); return 1; }
        ref.face = faces[0];
        ref.set = true;
    }
    if (!Mimic::prepare(st, err, say)) { std::printf("%s\n", err.c_str()); return 1; }
    say("On the " + Ort::device());
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    if (batch) {   // many targets, one after another; the output folder keeps folders' subfolders
        std::vector<std::pair<std::string, std::string>> jobs;   // (target, the folder it came from)
        for (const auto& t : targets) { std::error_code ec; const bool dir = fs::is_directory(fs::u8path(t), ec); for (const auto& f : Batch::collect({t}, subfolders)) jobs.push_back({f, dir ? t : ""}); }
        if (jobs.empty()) { std::printf("No pictures or videos in those.\n"); return 1; }
        Batch::Options opt; opt.outDir = out; opt.suffix = suffix; opt.skipDone = skipExisting; opt.jpegQuality = imageQuality; opt.video = vo;
        std::atomic<bool> no{false};
        int done = 0, skipped = 0, failed = 0;
        for (size_t k = 0; k < jobs.size(); ++k) {
            const std::string& in = jobs[k].first;
            const std::string res = Batch::outputFor(in, out, suffix, jobs[k].second);
            std::error_code ec;
            if (skipExisting && fs::exists(fs::u8path(res), ec)) { std::printf("[%zu/%zu] %s: already there\n", k + 1, jobs.size(), in.c_str()); ++skipped; continue; }
            int faces = 0; std::string e;
            std::printf("[%zu/%zu] %s\n", k + 1, jobs.size(), in.c_str()); std::fflush(stdout);
            const bool ok = Batch::processOne(in, res, src, st, ref, opt, no, no,
                [](float, const std::string& what) { std::printf("\r  %s          ", what.c_str()); std::fflush(stdout); }, faces, e);
            if (ok) { std::printf("\r  -> %s (%d face%s)          \n", res.c_str(), faces, faces == 1 ? "" : "s"); ++done; }
            else { std::printf("\r  failed: %s          \n", e.c_str()); ++failed; }
        }
        std::printf("Batch: %d done, %d skipped, %d failed (%.1f s)\n", done, skipped, failed, secs());
        return failed ? 2 : 0;
    }
    if (!video) {
        Img im;
        if (!loadPicture(target, im, err)) { std::printf("%s\n", err.c_str()); return 1; }
        const int n = Mimic::processFrame(im, src, st, ref, err, say);
        if (n < 0) { std::printf("%s\n", err.c_str()); return 1; }
        if (!savePicture(out, im, err, imageQuality)) { std::printf("%s\n", err.c_str()); return 1; }
        std::printf("%s -> %s (%d face%s, %.1f s)\n", target.c_str(), out.c_str(), n, n == 1 ? "" : "s", secs());
        return 0;
    }
    if (Video::ffmpeg().empty()) { std::printf("Videos need ffmpeg: put ffmpeg.exe next to Mimic or on the PATH.\n"); return 1; }
    Video::Reader rd;
    if (!rd.open(target, err)) { std::printf("%s\n", err.c_str()); return 1; }
    if (vo.trimStart > 0) { rd.close(); if (!rd.open(target, err, vo.trimStart / rd.info.fps)) { std::printf("%s\n", err.c_str()); return 1; } }
    const long last = vo.trimEnd > vo.trimStart ? vo.trimEnd - vo.trimStart : rd.info.frames - vo.trimStart;
    Video::Writer wr;
    if (!wr.open(out, rd.info.w, rd.info.h, rd.info.fps, target, vo, err)) { std::printf("%s\n", err.c_str()); return 1; }
    Img frame;
    long done = 0;
    st.sequence = Mimic::beginSequence();   // one video: its frames may be smoothed
    while (done < last && rd.next(frame)) {
        if (Mimic::processFrame(frame, src, st, ref, err) < 0) { std::printf("Frame %ld: %s\n", done, err.c_str()); return 1; }
        if (!wr.write(frame)) { std::printf("ffmpeg stopped taking frames\n"); return 1; }
        ++done;
        if (done % 10 == 0 || done == last) { std::printf("\r%ld / %ld frames (%.1f fps)   ", done, last, done / std::max(0.001, secs())); std::fflush(stdout); }
    }
    rd.close();
    if (!wr.close()) { std::printf("\nffmpeg couldn't finish %s\n", out.c_str()); return 1; }
    std::printf("\n%s -> %s (%ld frames, %.1f s)\n", target.c_str(), out.c_str(), done, secs());
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(fs::path(argv[i]).u8string());
    return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
