// mimic-cli - Mimic on the command line: one person's face onto a picture or a video.
//
//   mimic-cli -s face.jpg [-s another-of-them.jpg ...] -t target.jpg|video.mp4 -o out.jpg|out.mp4 [options]
#include "mimic.h"
#include "picture.h"
#include "video.h"

#include <algorithm>
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

static void usage() {
    std::printf(
        "mimic-cli - Mimic: one person's face onto a picture or a video. Part of Atelier.\n\n"
        "  mimic-cli -s face.jpg [-s more-of-them.jpg] -t target.jpg|video.mp4 -o out.jpg|out.mp4 [options]\n\n"
        "  -s FILE           the face to become (several pictures of the same person: their average)\n"
        "  -t FILE           the picture or video to put it in\n"
        "  -o FILE           where the result goes (default: <target>_mimic.<ext>)\n"
        "  --swapper NAME    inswapper (default) or hyperswap\n"
        "  --boost N         pixel boost 1..4: a bigger, sharper swap (slower)\n"
        "  --weight X        0..1: 0 more of the source, 1 more of the target's own look (default 0.5)\n"
        "  --enhancer NAME   gfpgan (default), codeformer, gpen or none\n"
        "  --blend X         how much of the restored face, 0..1 (default 0.8)\n"
        "  --fidelity X      CodeFormer: 0 more restoration, 1 truer to the face (default 0.5)\n"
        "  --mask-blur X     the mask's feathering, 0..1 (default 0.3)\n"
        "  --padding T R B L the mask's padding in %% (default 0 0 0 0)\n"
        "  --occlusion       keep hands, hair and glasses that are in front of the face\n"
        "  --faces WHICH     all (default), largest, or reference (with --reference)\n"
        "  --reference FILE  a picture of the face to replace (the others are left alone)\n"
        "  --distance X      how close to the reference counts, 0..1.5 (default 0.6)\n"
        "  --crf N           video quality, 0..51 (default 18)\n"
        "  --cpu             don't use the GPU\n\n"
        "Only use faces you have the right to use.\n");
}

static int run(const std::vector<std::string>& args) {
    std::vector<std::string> sources;
    std::string target, out, reference;
    Mimic::Settings st;
    bool cpu = false;
    int crf = 18;
    auto num = [&](size_t& i) { return (float)std::atof(args[++i].c_str()); };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool more = i + 1 < args.size();
        if ((a == "-s" || a == "--source") && more) sources.push_back(args[++i]);
        else if ((a == "-t" || a == "--target") && more) target = args[++i];
        else if ((a == "-o" || a == "--out") && more) out = args[++i];
        else if (a == "--swapper" && more) st.swapper = args[++i] == "hyperswap" ? Mimic::SW_HYPERSWAP : Mimic::SW_INSWAPPER;
        else if (a == "--boost" && more) st.boost = std::clamp(std::atoi(args[++i].c_str()), 1, 4);
        else if (a == "--weight" && more) st.weight = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--enhancer" && more) { const std::string e = args[++i]; st.enhancer = e == "codeformer" ? Mimic::EN_CODEFORMER : e == "gpen" ? Mimic::EN_GPEN : e == "none" ? Mimic::EN_NONE : Mimic::EN_GFPGAN; }
        else if (a == "--blend" && more) st.enhanceBlend = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--fidelity" && more) st.codeformerFidelity = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--mask-blur" && more) st.maskBlur = std::clamp(num(i), 0.0f, 1.0f);
        else if (a == "--padding" && i + 4 < args.size()) for (int k = 0; k < 4; ++k) st.padding[k] = std::clamp(std::atoi(args[++i].c_str()), 0, 100);
        else if (a == "--occlusion") st.occlusion = true;
        else if (a == "--faces" && more) { const std::string w = args[++i]; st.which = w == "largest" ? Mimic::W_LARGEST : w == "reference" ? Mimic::W_LIKE_REFERENCE : Mimic::W_ALL; }
        else if (a == "--reference" && more) { reference = args[++i]; st.which = Mimic::W_LIKE_REFERENCE; }
        else if (a == "--distance" && more) st.referenceDistance = std::clamp(num(i), 0.0f, 1.5f);
        else if (a == "--crf" && more) crf = std::clamp(std::atoi(args[++i].c_str()), 0, 51);
        else if (a == "--cpu") cpu = true;
        else if (a == "-h" || a == "--help" || a == "/?") { usage(); return 0; }
        else { std::printf("What's %s? (--help lists the options)\n", a.c_str()); return 1; }
    }
    if (sources.empty() || target.empty()) { usage(); return 1; }
    const bool video = Video::isVideo(target);
    if (out.empty()) {
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
        if (!loadPicture(reference, im, err) || !Faces::detect(im, faces, err) || faces.empty()) { std::printf("Reference: %s\n", err.empty() ? "no face in it" : err.c_str()); return 1; }
        ref.face = faces[0];
        if (!Faces::embed(im, ref.face, err)) { std::printf("%s\n", err.c_str()); return 1; }
        ref.set = true;
    }
    if (!Mimic::prepare(st, err, say)) { std::printf("%s\n", err.c_str()); return 1; }
    say("On the " + Ort::device());
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    if (!video) {
        Img im;
        if (!loadPicture(target, im, err)) { std::printf("%s\n", err.c_str()); return 1; }
        const int n = Mimic::swapFrame(im, src, st, ref, err, say);
        if (n < 0) { std::printf("%s\n", err.c_str()); return 1; }
        if (!savePicture(out, im, err)) { std::printf("%s\n", err.c_str()); return 1; }
        std::printf("%s -> %s (%d face%s, %.1f s)\n", target.c_str(), out.c_str(), n, n == 1 ? "" : "s", secs());
        return 0;
    }
    if (Video::ffmpeg().empty()) { std::printf("Videos need ffmpeg: put ffmpeg.exe next to Mimic or on the PATH.\n"); return 1; }
    Video::Reader rd;
    if (!rd.open(target, err)) { std::printf("%s\n", err.c_str()); return 1; }
    Video::Writer wr;
    if (!wr.open(out, rd.info.w, rd.info.h, rd.info.fps, target, crf, err)) { std::printf("%s\n", err.c_str()); return 1; }
    Img frame;
    long done = 0;
    while (rd.next(frame)) {
        if (Mimic::swapFrame(frame, src, st, ref, err) < 0) { std::printf("Frame %ld: %s\n", done, err.c_str()); return 1; }
        if (!wr.write(frame)) { std::printf("ffmpeg stopped taking frames\n"); return 1; }
        ++done;
        if (done % 10 == 0 || done == rd.info.frames) {
            std::printf("\r%ld / %ld frames (%.1f fps)   ", done, rd.info.frames, done / std::max(0.001, secs()));
            std::fflush(stdout);
        }
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
