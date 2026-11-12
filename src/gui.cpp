// Mimic - the window: one person's face onto a picture or a video. Part of Atelier.
// Drop pictures of the face to become on the left, a picture or a video on the preview; tune it on the right; save
// the picture or render the video. Click a face in the preview to swap only that person.
//
//   Mimic [target]
//   Mimic --capture out.bmp [--source face.jpg] [target]     a screenshot from a hidden window, then quit
#include "mimic.h"
#include "picture.h"
#include "splash.h"
#include "video.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace fs = std::filesystem;

// ---------- the Atelier look: greys, the Cursed pink; solid, no gradients, no shadows ----------
static const ImU32 C_BG = IM_COL32(0x1a, 0x1a, 0x1a, 255), C_PANEL = IM_COL32(0x24, 0x24, 0x24, 255),
                   C_LINE = IM_COL32(0x3a, 0x3a, 0x3a, 255), C_TEXT = IM_COL32(0xe6, 0xe6, 0xe6, 255),
                   C_DIM = IM_COL32(0x9a, 0x9a, 0x9a, 255), C_PINK = IM_COL32(0xe0, 0x40, 0x8a, 255),
                   C_DEEP = IM_COL32(0xc2, 0x18, 0x6b, 255), C_WELL = IM_COL32(0x14, 0x14, 0x14, 255);
static ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.FrameRounding = 2; s.GrabRounding = 2; s.PopupRounding = 2; s.ScrollbarRounding = 2;
    s.WindowBorderSize = 0; s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.WindowPadding = ImVec2(12, 10); s.FramePadding = ImVec2(7, 4); s.ItemSpacing = ImVec2(6, 7);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = col4(C_TEXT); c[ImGuiCol_TextDisabled] = col4(C_DIM);
    c[ImGuiCol_WindowBg] = col4(C_PANEL); c[ImGuiCol_ChildBg] = col4(C_PANEL); c[ImGuiCol_PopupBg] = col4(IM_COL32(0x21, 0x21, 0x21, 255));
    c[ImGuiCol_Border] = col4(C_LINE); c[ImGuiCol_Separator] = col4(C_LINE);
    c[ImGuiCol_FrameBg] = col4(C_WELL); c[ImGuiCol_FrameBgHovered] = col4(IM_COL32(0x2e, 0x2e, 0x2e, 255)); c[ImGuiCol_FrameBgActive] = col4(IM_COL32(0x33, 0x33, 0x33, 255));
    c[ImGuiCol_MenuBarBg] = col4(C_BG);
    c[ImGuiCol_CheckMark] = col4(C_PINK); c[ImGuiCol_SliderGrab] = col4(C_PINK); c[ImGuiCol_SliderGrabActive] = col4(C_DEEP);
    c[ImGuiCol_Button] = col4(IM_COL32(0x33, 0x33, 0x33, 255)); c[ImGuiCol_ButtonHovered] = col4(C_DEEP); c[ImGuiCol_ButtonActive] = col4(C_PINK);
    c[ImGuiCol_Header] = col4(IM_COL32(0x33, 0x33, 0x33, 255)); c[ImGuiCol_HeaderHovered] = col4(C_DEEP); c[ImGuiCol_HeaderActive] = col4(C_PINK);
    c[ImGuiCol_PlotHistogram] = col4(C_PINK);
}

static bool fileExists(const char* f) { if (FILE* t = fopen(f, "rb")) { fclose(t); return true; } return false; }

static void loadFont() {
    ImGuiIO& io = ImGui::GetIO();
    const char* fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char* f : fonts)
        if (fileExists(f)) {
            static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2190, 0x21FF, 0x2600, 0x26FF, 0};
            // the interface font, then the splash's: a light one for the name, a small one for the credits; Segoe UI has
            // no stars (the credit's), so Windows' symbol font fills them in
            auto add = [&](const char* file, float size) {
                ImFont* font = io.Fonts->AddFontFromFileTTF(file, size, nullptr, ranges);
                if (fileExists("C:/Windows/Fonts/seguisym.ttf")) {
                    ImFontConfig merge; merge.MergeMode = true;
                    static const ImWchar sym[] = {0x2190, 0x21FF, 0x2600, 0x26FF, 0};
                    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisym.ttf", size, &merge, sym);
                }
                return font;
            };
            add(f, 16.0f);
            const char* light = fileExists("C:/Windows/Fonts/segoeuil.ttf") ? "C:/Windows/Fonts/segoeuil.ttf" : f;
            ImFont* title = add(light, 50.0f);
            ImFont* small = add(f, 13.0f);
            Splash::setFonts(title, small);
            return;
        }
    io.Fonts->AddFontDefault();
}

static std::string fileDialog(bool save, const char* filter, const char* defExt, const std::string& start, bool multi = false, std::vector<std::string>* many = nullptr) {
#ifdef _WIN32
    std::wstring f;
    for (const char* p = filter; *p; ++p) f += (*p == '|') ? L'\0' : (wchar_t)*p;
    f += L'\0'; f += L'\0';
    std::vector<wchar_t> buf(32768, 0);
    if (!start.empty()) { std::wstring s = fs::u8path(start).wstring(); wcsncpy(buf.data(), s.c_str(), buf.size() - 1); }
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.lpstrFilter = f.c_str(); o.lpstrFile = buf.data(); o.nMaxFile = (DWORD)buf.size();
    std::wstring ext = defExt ? std::wstring(defExt, defExt + strlen(defExt)) : L"";
    o.lpstrDefExt = defExt ? ext.c_str() : nullptr;
    o.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR) : (OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | (multi ? OFN_ALLOWMULTISELECT | OFN_EXPLORER : 0));
    if (!(save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o))) return "";
    if (multi && many) {   // a folder, then the names, each 0-ended; or one full path
        std::wstring dir = buf.data();
        const wchar_t* p = buf.data() + dir.size() + 1;
        if (!*p) many->push_back(fs::path(dir).u8string());
        for (; *p; p += wcslen(p) + 1) many->push_back((fs::path(dir) / p).u8string());
        return many->empty() ? "" : many->front();
    }
    return fs::path(buf.data()).u8string();
#else
    (void)multi; (void)many;
    std::string cmd = save ? "zenity --file-selection --save --confirm-overwrite 2>/dev/null" : "zenity --file-selection 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    char line[4096] = {0};
    std::string r = fgets(line, sizeof line, p) ? line : "";
    pclose(p);
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
    if (save && !r.empty() && defExt && r.find('.') == std::string::npos) r += std::string(".") + defExt;
    return r;
#endif
}

static const char* PICTURES = "Pictures|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*|";
static const char* TARGETS = "Pictures and videos|*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.gif|All files|*.*|";

static void upload(GLuint& tex, const Img& im) {
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, im.w, im.h, 0, GL_RGB, GL_UNSIGNED_BYTE, im.px.data());
}

// a face's square, for the thumbnails
static Img faceThumb(const Img& im, const Face& f, int size) {
    const float cx = (f.box[0] + f.box[2]) / 2, cy = (f.box[1] + f.box[3]) / 2, side = std::max(f.box[2] - f.box[0], f.box[3] - f.box[1]) * 1.3f;
    Affine M; M.m[0] = M.m[4] = size / side; M.m[2] = -(cx - side / 2) * M.m[0]; M.m[5] = -(cy - side / 2) * M.m[4];
    const Crop c = warpCrop(im, M, size, size);
    Img t(size, size);
    for (size_t i = 0; i < t.px.size(); ++i) t.px[i] = (uint8_t)std::clamp(c.px[i] + 0.5f, 0.0f, 255.0f);
    return t;
}

// ---------- state ----------
static SDL_Window* window = nullptr;
static bool quit = false;

struct SourcePic { std::string path; Img thumb; GLuint tex = 0; bool ready = false; };
static std::vector<SourcePic> sourcePics;
static Mimic::Source source;
static Mimic::Settings settings;
static Mimic::Reference reference;
static Img referenceThumb; static GLuint referenceTex = 0; static bool referenceTexStale = false;

static std::string targetPath;
static bool targetIsVideo = false;
static Video::Info targetInfo;
static float frameTime = 0;            // the video's preview frame, in seconds
static Img original, result;           // the target (or its preview frame) and the swapped one
static std::vector<Face> previewFaces; // where the faces are in it (click one: the reference)
static GLuint originalTex = 0, resultTex = 0;
static bool originalStale = false, resultStale = false, haveResult = false;
static float wipe = 0.5f;
static int view = 0;                   // 0 wipe, 1 result, 2 original
static int jpegQuality = 95;
static Video::Output output;          // FaceFusion's output settings
static bool showAbout = false;

// the worker: model work, one job at a time
static std::mutex mtx;
static std::deque<std::function<void()>> jobs;
static std::thread worker;
static std::atomic<bool> busy{false}, stopRender{false}, workerQuit{false};
static std::atomic<float> progress{-1.0f};
static std::string status = "Drop pictures of the face to become on the left, then a picture or a video in the middle";
static std::vector<std::function<void()>> onMain;   // results handed back to the main thread (textures live there)
static bool previewWanted = false;

static void say(const std::string& s) { std::lock_guard<std::mutex> g(mtx); status = s; }
static void post(std::function<void()> f) { std::lock_guard<std::mutex> g(mtx); onMain.push_back(std::move(f)); }
static void queue(std::function<void()> f) { std::lock_guard<std::mutex> g(mtx); jobs.push_back(std::move(f)); }

static void workerLoop() {
    while (!workerQuit) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> g(mtx);
            if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); }
        }
        if (!job) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
        busy = true;
        job();
        busy = false;
    }
}

static std::string base(const std::string& p) { return fs::u8path(p).filename().u8string(); }

static void addSources(const std::vector<std::string>& files) {
    for (const auto& f : files) {
        queue([f] {
            Img im; std::string err;
            if (!loadPicture(f, im, err)) { say(err); return; }
            std::vector<Face> faces;
            if (!Faces::detect(im, faces, err, 0.5f, say) || faces.empty()) { say(base(f) + ": " + (err.empty() ? "no face in it" : err)); return; }
            Face face = faces[0];
            if (!Faces::embed(im, face, err, say)) { say(err); return; }
            Img thumb = faceThumb(im, face, 96);
            post([f, face, thumb] {
                SourcePic sp; sp.path = f; sp.thumb = thumb; sp.ready = true;
                sourcePics.push_back(sp);
                source.faces.push_back(face);
                Mimic::finishSource(source);
                previewWanted = true;
            });
            say("The face to become: " + std::to_string(source.faces.size() + 1) + " picture(s)");
        });
    }
}

static void loadTarget(const std::string& path) {
    targetPath = path;
    targetIsVideo = Video::isVideo(path);
    frameTime = 0;
    haveResult = false;
    reference.set = false;
    queue([path] {
        Img im; std::string err;
        Video::Info info;
        if (Video::isVideo(path)) {
            if (Video::ffmpeg().empty()) { say("Videos need ffmpeg: put ffmpeg.exe next to Mimic or on the PATH"); return; }
            if (!Video::probe(path, info, err) || !Video::frameAt(path, 0, im, err)) { say(err); return; }
        } else if (!loadPicture(path, im, err)) { say(err); return; }
        std::vector<Face> faces;
        Faces::analyse(im, settings.detect, faces, err, false, say);
        post([im, info, faces] { original = im; targetInfo = info; previewFaces = faces; originalStale = true; previewWanted = true; });
        say(base(path) + (Video::isVideo(path) ? " (" + std::to_string(info.w) + " x " + std::to_string(info.h) + ", " + std::to_string(info.frames) + " frames)" : " (" + std::to_string(im.w) + " x " + std::to_string(im.h) + ")") + ", " + std::to_string(faces.size()) + " face(s)");
    });
}

static void seekFrame(float t) {
    const std::string path = targetPath;
    queue([path, t] {
        Img im; std::string err;
        if (!Video::frameAt(path, t, im, err)) { say(err); return; }
        std::vector<Face> faces;
        Faces::analyse(im, settings.detect, faces, err, false);
        post([im, faces] { original = im; previewFaces = faces; originalStale = true; previewWanted = true; });
    });
}

static void runPreview() {
    if (original.empty() || (settings.swap && !source.ready())) return;
    const Img im = original; const auto src = source; const auto st = settings; const auto ref = reference;
    queue([im, src, st, ref] {
        Img out = im; std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const int n = Mimic::swapFrame(out, src, st, ref, err, say);
        if (n < 0) { say(err); return; }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        post([out] { result = out; resultStale = true; haveResult = true; });
        char msg[160];
        snprintf(msg, sizeof msg, "%d face%s done in %.2f s, on the %s", n, n == 1 ? "" : "s", s, Ort::device().c_str());
        say(msg);
    });
}

static std::string defaultOut() {
    const fs::path p = fs::u8path(targetPath);
    return (p.parent_path() / (p.stem().u8string() + "_mimic" + (targetIsVideo ? ".mp4" : p.extension().u8string()))).u8string();
}

static void savePictureAs() {
    if (!haveResult || targetIsVideo) return;
    const std::string p = fileDialog(true, "JPEG|*.jpg|PNG|*.png|", "jpg", defaultOut());
    if (p.empty()) return;
    std::string err;
    if (savePicture(p, result, err, jpegQuality)) say("Saved " + p); else say(err);
}

static void renderVideo() {
    if (!targetIsVideo || (settings.swap && !source.ready())) return;
    const std::string out = fileDialog(true, "MP4 video|*.mp4|", "mp4", defaultOut());
    if (out.empty()) return;
    const std::string in = targetPath; const auto src = source; const auto st = settings; const auto ref = reference; const auto vo = output;
    stopRender = false; progress = 0.0f;
    queue([in, out, src, st, ref, vo] {
        std::string err;
        Video::Reader rd;
        if (!rd.open(in, err)) { say(err); progress = -1; return; }
        if (vo.trimStart > 0) { rd.close(); if (!rd.open(in, err, vo.trimStart / rd.info.fps)) { say(err); progress = -1; return; } }
        const long total = vo.trimEnd > vo.trimStart ? vo.trimEnd - vo.trimStart : rd.info.frames - vo.trimStart;
        rd.info.frames = total;
        Video::Writer wr;
        if (!wr.open(out, rd.info.w, rd.info.h, rd.info.fps, in, vo, err)) { say(err); progress = -1; return; }
        Img frame;
        long done = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (!stopRender && done < total && rd.next(frame)) {
            if (Mimic::swapFrame(frame, src, st, ref, err) < 0) { say(err); break; }
            if (!wr.write(frame)) { say("ffmpeg stopped taking frames"); break; }
            ++done;
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            progress = rd.info.frames > 0 ? (float)done / rd.info.frames : 0.0f;
            if (done % 5 == 0) {
                char msg[200];
                const double fps = done / std::max(0.001, s);
                snprintf(msg, sizeof msg, "Rendering: %ld / %ld frames, %.1f fps, about %.0f s to go", done, rd.info.frames, fps, rd.info.frames > done ? (rd.info.frames - done) / std::max(0.01, fps) : 0.0);
                say(msg);
            }
        }
        rd.close();
        const bool fine = wr.close();
        say(stopRender ? "Stopped: what was done is in " + out : fine ? "Rendered " + out + " (" + std::to_string(done) + " frames)" : "ffmpeg couldn't finish " + out);
        progress = -1.0f;
    });
}

static void dropFiles(const std::vector<std::string>& files, bool onSourcePanel) {
    std::vector<std::string> pics, rest;
    for (const auto& f : files) (Video::isVideo(f) ? rest : pics).push_back(f);
    // on the left panel: the face to become. Elsewhere: the target (the last one), unless there's no face yet and
    // several pictures came: those are the face to become
    if (onSourcePanel) { addSources(pics); if (!rest.empty()) loadTarget(rest.back()); return; }
    if (!rest.empty()) { loadTarget(rest.back()); addSources(pics); return; }
    if (pics.size() > 1 && sourcePics.empty()) { addSources(pics); return; }
    if (!pics.empty()) loadTarget(pics.back());
}

// ---------- the frame ----------
static void menuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Add the Face to Become...")) { std::vector<std::string> many; fileDialog(false, PICTURES, nullptr, "", true, &many); addSources(many); }
        if (ImGui::MenuItem("Open Target...", "Ctrl+O")) { const std::string p = fileDialog(false, TARGETS, nullptr, ""); if (!p.empty()) loadTarget(p); }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Picture As...", "Ctrl+S", false, haveResult && !targetIsVideo)) savePictureAs();
        if (ImGui::MenuItem("Render Video...", "Ctrl+R", false, targetIsVideo && source.ready() && progress < 0)) renderVideo();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Wipe: before | after", "1", view == 0)) view = 0;
        if (ImGui::MenuItem("After", "2", view == 1)) view = 1;
        if (ImGui::MenuItem("Before", "3", view == 2)) view = 2;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) { if (ImGui::MenuItem("About Mimic")) showAbout = true; ImGui::EndMenu(); }
    ImGui::EndMainMenuBar();
}

static void keys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) { const std::string p = fileDialog(false, TARGETS, nullptr, ""); if (!p.empty()) loadTarget(p); }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) savePictureAs();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R) && progress < 0) renderVideo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q)) quit = true;
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_1)) view = 0;
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_2)) view = 1;
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_3)) view = 2;
}

static bool combo(const char* id, int& v, int n, const char* (*name)(int)) {
    bool changed = false;
    if (ImGui::BeginCombo(id, name(v))) {
        for (int i = 0; i < n; ++i) if (ImGui::Selectable(name(i), v == i)) { v = i; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

static void heading(const char* t) { ImGui::PushStyleColor(ImGuiCol_Text, col4(C_PINK)); ImGui::TextUnformatted(t); ImGui::PopStyleColor(); }

static ImVec2 sourcePanelMin, sourcePanelMax;

static void leftPanel(ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(size);
    ImGui::Begin("##left", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    sourcePanelMin = pos; sourcePanelMax = ImVec2(pos.x + size.x, pos.y + size.y);
    const float w = ImGui::GetContentRegionAvail().x;
    heading("THE FACE TO BECOME");
    if (sourcePics.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + 120);
        for (float x = a.x; x < b.x; x += 12) { dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + 6, b.x), a.y), C_LINE, 1.5f); dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + 6, b.x), b.y), C_LINE, 1.5f); }
        for (float y = a.y; y < b.y; y += 12) { dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + 6, b.y)), C_LINE, 1.5f); dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + 6, b.y)), C_LINE, 1.5f); }
        const char* t1 = "Drop pictures of them here";
        const char* t2 = "Several of the same person: better";
        dl->AddText(ImVec2(a.x + (w - ImGui::CalcTextSize(t1).x) / 2, a.y + 42), C_TEXT, t1);
        dl->AddText(ImVec2(a.x + (w - ImGui::CalcTextSize(t2).x) / 2, a.y + 64), C_DIM, t2);
        ImGui::Dummy(ImVec2(w, 126));
    } else {
        const float t = 78;
        int perRow = std::max(1, (int)((w + 6) / (t + 6)));
        for (size_t i = 0; i < sourcePics.size(); ++i) {
            if (i % perRow) ImGui::SameLine();
            ImGui::PushID((int)i);
            ImGui::Image((ImTextureID)(intptr_t)sourcePics[i].tex, ImVec2(t, t));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nRight-click to remove", base(sourcePics[i].path).c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !busy) {
                sourcePics.erase(sourcePics.begin() + i);
                source.faces.erase(source.faces.begin() + i);
                Mimic::finishSource(source);
                previewWanted = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    if (ImGui::Button("Add pictures...", ImVec2(w, 0))) { std::vector<std::string> many; fileDialog(false, PICTURES, nullptr, "", true, &many); addSources(many); }

    ImGui::Spacing();
    heading("TARGET");
    if (targetPath.empty()) ImGui::TextDisabled("Drop a picture or a video on the preview");
    else { ImGui::TextWrapped("%s", base(targetPath).c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", targetPath.c_str()); }
    if (ImGui::Button("Open...", ImVec2(w, 0))) { const std::string p = fileDialog(false, TARGETS, nullptr, ""); if (!p.empty()) loadTarget(p); }
    if (targetIsVideo && targetInfo.seconds > 0) {
        ImGui::TextDisabled("%d x %d, %.2f fps, %ld frames", targetInfo.w, targetInfo.h, targetInfo.fps, targetInfo.frames);
        ImGui::PushItemWidth(w);
        ImGui::SliderFloat("##t", &frameTime, 0.0f, (float)std::max(0.0, targetInfo.seconds - 0.05), "preview at %.1f s");
        ImGui::PopItemWidth();
        if (ImGui::IsItemDeactivatedAfterEdit()) seekFrame(frameTime);
    }

    ImGui::Spacing();
    heading("FACE SELECTOR");
    ImGui::PushItemWidth(w);
    if (combo("##mode", settings.mode, Mimic::SEL_COUNT, Mimic::modeName)) previewWanted = true;
    if (settings.mode == Mimic::SEL_REFERENCE) {
        if (reference.set && referenceTex) {
            ImGui::Image((ImTextureID)(intptr_t)referenceTex, ImVec2(64, 64));
            ImGui::SameLine(); ImGui::TextWrapped("Only faces like this one. Click another face in the preview to change it.");
            ImGui::SliderFloat("##dist", &settings.referenceDistance, 0.0f, 1.0f, "face distance: %.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) previewWanted = true;
        } else ImGui::TextWrapped("Click a face in the preview: only faces like it get swapped.");
    } else {
        ImGui::TextDisabled("Order");
        if (combo("##order", settings.order, Mimic::ORD_COUNT, Mimic::orderName)) previewWanted = true;
        static const char* genders[] = {"Any gender", "Like the source", "Male", "Female"};
        int g = settings.gender + 2;
        if (ImGui::Combo("##gender", &g, genders, 4)) { settings.gender = g - 2; previewWanted = true; }
        static std::vector<std::string> races = {"Any race", "Like the source", "White", "Black", "Latino", "Asian", "Indian", "Arabic"};
        int r = settings.race + 2;
        if (ImGui::BeginCombo("##race", races[r].c_str())) {
            for (int i = 0; i < (int)races.size(); ++i) if (ImGui::Selectable(races[i].c_str(), r == i)) { settings.race = i - 2; previewWanted = true; }
            ImGui::EndCombo();
        }
        int age[2] = {settings.ageStart, settings.ageEnd};
        if (ImGui::SliderInt2("##age", age, 0, 100, "age %d")) { settings.ageStart = std::min(age[0], age[1]); settings.ageEnd = std::max(age[0], age[1]); }
        if (ImGui::IsItemDeactivatedAfterEdit()) previewWanted = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only faces in this age span (FairFace guesses each face's age, gender and race).");
    }
    ImGui::PopItemWidth();
    ImGui::End();
}

static void canvas(ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(C_BG));
    ImGui::Begin("##canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (original.empty()) {
        const ImVec2 c(pos.x + size.x / 2, pos.y + size.y / 2);
        const float bw = std::min(540.0f, size.x - 80), bh = 240;
        const ImVec2 a(c.x - bw / 2, c.y - bh / 2), b(c.x + bw / 2, c.y + bh / 2);
        for (float x = a.x; x < b.x; x += 12) { dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + 6, b.x), a.y), C_LINE, 1.5f); dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + 6, b.x), b.y), C_LINE, 1.5f); }
        for (float y = a.y; y < b.y; y += 12) { dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + 6, b.y)), C_LINE, 1.5f); dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + 6, b.y)), C_LINE, 1.5f); }
        auto centred = [&](const char* t, float y, ImU32 col) { const ImVec2 ts = ImGui::CalcTextSize(t); dl->AddText(ImVec2(c.x - ts.x / 2, y), col, t); };
        centred("Drop a picture or a video here", c.y - 34, C_TEXT);
        centred("Mimic puts the face on the left into it.", c.y - 6, C_DIM);
        centred("Only use faces you have the right to use.", c.y + 22, C_DIM);
        ImGui::End(); ImGui::PopStyleColor();
        return;
    }
    const float pad = 20;
    const float s = std::min((size.x - pad * 2) / original.w, (size.y - pad * 2) / original.h);
    const float dw = original.w * s, dh = original.h * s;
    const ImVec2 a(pos.x + (size.x - dw) / 2, pos.y + (size.y - dh) / 2), b(a.x + dw, a.y + dh);
    const bool after = haveResult && resultTex;
    if (view == 2 || !after) dl->AddImage((ImTextureID)(intptr_t)originalTex, a, b);
    else if (view == 1) dl->AddImage((ImTextureID)(intptr_t)resultTex, a, b);
    else {
        const float x = a.x + dw * wipe;
        dl->AddImage((ImTextureID)(intptr_t)originalTex, a, ImVec2(x, b.y), ImVec2(0, 0), ImVec2(wipe, 1));
        dl->AddImage((ImTextureID)(intptr_t)resultTex, ImVec2(x, a.y), b, ImVec2(wipe, 0), ImVec2(1, 1));
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), C_PINK, 2.0f);
        dl->AddCircleFilled(ImVec2(x, a.y + dh / 2), 9, C_PINK);
        auto label = [&](const char* t, float x) {   // on a dark tab, so it reads on bright pictures
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddRectFilled(ImVec2(x - 6, a.y + 6), ImVec2(x + ts.x + 6, a.y + 10 + ts.y), IM_COL32(0x1a, 0x1a, 0x1a, 210), 2);
            dl->AddText(ImVec2(x, a.y + 8), C_TEXT, t);
        };
        label("Before", a.x + 14);
        label("After", b.x - 14 - ImGui::CalcTextSize("After").x);
    }
    // the faces: hover one to see it, click it to make it the reference; drag elsewhere moves the wipe
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("view", ImVec2(dw, dh));
    const ImVec2 m = ImGui::GetIO().MousePos;
    int hovered = -1;
    for (size_t i = 0; i < previewFaces.size(); ++i) {
        const Face& f = previewFaces[i];
        const ImVec2 fa(a.x + f.box[0] * s, a.y + f.box[1] * s), fb(a.x + f.box[2] * s, a.y + f.box[3] * s);
        const bool isRef = reference.set && settings.mode == Mimic::SEL_REFERENCE && Faces::distance(f, reference.face) < 0.03f;
        if (ImGui::IsItemHovered() && m.x >= fa.x && m.x <= fb.x && m.y >= fa.y && m.y <= fb.y) hovered = (int)i;
        if (hovered == (int)i || isRef) dl->AddRect(fa, fb, isRef ? C_PINK : C_TEXT, 2, 0, 1.5f);
    }
    if (hovered >= 0) {
        ImGui::SetTooltip("Click: swap only this person");
        if (ImGui::IsItemClicked()) {
            const Face f = previewFaces[hovered]; const Img im = original;
            queue([f, im] {
                Face face = f; std::string err;
                if (!Faces::embed(im, face, err)) { say(err); return; }
                Img thumb = faceThumb(im, face, 96);
                post([face, thumb] { reference.face = face; reference.set = true; referenceThumb = thumb; referenceTexStale = true; settings.mode = Mimic::SEL_REFERENCE; previewWanted = true; });
            });
        }
    } else if (ImGui::IsItemActive() && view == 0) wipe = std::clamp((m.x - a.x) / dw, 0.0f, 1.0f);
    if (busy) {
        const float t = (float)ImGui::GetTime() * 6;
        const ImVec2 c(b.x - 22, b.y - 22);
        dl->AddCircleFilled(c, 16, IM_COL32(0x1a, 0x1a, 0x1a, 200));
        dl->PathArcTo(c, 10, t, t + 4.2f, 24);
        dl->PathStroke(C_PINK, 0, 2.5f);
    }
    ImGui::End(); ImGui::PopStyleColor();
}

static void rightPanel(ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(size);
    ImGui::Begin("##right", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    const float w = ImGui::GetContentRegionAvail().x;
    bool again = false;   // re-run the preview
    auto after = [&] { if (ImGui::IsItemDeactivatedAfterEdit()) again = true; };
    const float footH = 150;
    ImGui::BeginChild("##settings", ImVec2(w, size.y - footH - 20), false);
    const float cw = ImGui::GetContentRegionAvail().x;
    ImGui::PushItemWidth(cw);
    // a model choice: models not on this PC yet say so (they're fetched once when used)
    auto modelCombo = [&](const char* id, int& v, int n, std::function<const char*(int)> name, std::function<const char*(int)> file, std::function<const char*(int)> about) {
        bool changed = false;
        auto label = [&](int i) { std::string l = name(i); if (!Ort::have(file(i))) l += "  (download)"; return l; };
        if (ImGui::BeginCombo(id, label(v).c_str())) {
            for (int i = 0; i < n; ++i) {
                if (ImGui::Selectable(label(i).c_str(), v == i)) { v = i; changed = true; }
                if (ImGui::IsItemHovered() && about) ImGui::SetTooltip("%s", about(i));
            }
            ImGui::EndCombo();
        }
        return changed;
    };
    const auto& sms = Mimic::swapModels();
    const auto& ems = Mimic::enhanceModels();

    heading("PROCESSORS");
    if (ImGui::Checkbox("Face swapper", &settings.swap)) again = true;
    if (ImGui::Checkbox("Face enhancer", &settings.enhance)) again = true;
    if (ImGui::Checkbox("Face editor", &settings.edit)) again = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Change the expression: a smile, the eyes, the brows, the mouth, the head's pose (LivePortrait).");
    if (ImGui::Checkbox("Face debugger", &settings.debug)) again = true;

    if (settings.swap && ImGui::CollapsingHeader("FACE SWAPPER", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (modelCombo("##swapper", settings.swapModel, (int)sms.size(), [&](int i) { return sms[i].name; }, [&](int i) { return sms[i].file; }, [&](int i) { return sms[i].about; })) again = true;
        const int base = sms[settings.swapModel].size;
        char fmt[64]; snprintf(fmt, sizeof fmt, "pixel boost: %d px", base * settings.boost);
        ImGui::SliderInt("##boost", &settings.boost, 1, 4, fmt); after();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A bigger crop, swapped in several passes: sharper on big faces, slower.");
        ImGui::SliderFloat("##weight", &settings.weight, 0.0f, 1.0f, "weight: %.2f"); after();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0: more of the face to become. 1: more of the target's own look. 0.5: neither.");
    }
    if (settings.enhance && ImGui::CollapsingHeader("FACE ENHANCER", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (modelCombo("##enh", settings.enhanceModel, (int)ems.size(), [&](int i) { return ems[i].name; }, [&](int i) { return ems[i].file; }, [&](int i) { return ems[i].about; })) again = true;
        ImGui::SliderFloat("##blend", &settings.enhanceBlend, 0.0f, 1.0f, "blend: %.2f"); after();
        if (std::string(ems[settings.enhanceModel].file) == "codeformer.onnx") { ImGui::SliderFloat("##fid", &settings.enhanceWeight, 0.0f, 1.0f, "weight: %.2f"); after(); }
    }
    if (settings.edit && ImGui::CollapsingHeader("FACE EDITOR", ImGuiTreeNodeFlags_DefaultOpen)) {
        // a prompt sets the dials; the dials stay free to change after
        static char prompt[256] = "";
        static std::string understood;
        ImGui::TextDisabled("Describe the expression");
        const bool enter = ImGui::InputTextWithHint("##prompt", "a big smile, eyes closed", prompt, sizeof prompt, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Words like: smile, laughing, sad, surprised, angry, worried, smug, eyes closed, wide eyes,\n"
                                                      "looking left/up, raised eyebrows, mouth open, pout, kiss, turn left, chin up, tilt.\n"
                                                      "\"slight\" / \"a little\" softens, \"very\" / \"big\" strengthens, \"not\" turns it round.");
        if (ImGui::Button("Apply", ImVec2(cw * 0.5f - 3, 0)) || enter) {
            float e[Mimic::ED_COUNT] = {0};
            understood = Mimic::promptToEdits(prompt, e);
            if (!understood.empty()) { memcpy(settings.edits, e, sizeof e); again = true; }
            else understood = "(no words it knows: try smile, surprised, eyes closed...)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(-1, 0))) { for (auto& v : settings.edits) v = 0; understood.clear(); again = true; }
        if (!understood.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", understood.c_str()); ImGui::PopTextWrapPos(); }
        for (int i = 0; i < Mimic::ED_COUNT; ++i) {
            ImGui::PushID(1000 + i);
            char fmt[64]; snprintf(fmt, sizeof fmt, "%s: %%.2f", Mimic::editName(i));
            ImGui::SliderFloat("##ed", &settings.edits[i], -1.0f, 1.0f, fmt); after();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s. Double-click to type; right-click for 0.", Mimic::editHint(i));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { settings.edits[i] = 0; again = true; }
            ImGui::PopID();
        }
    }
    if (settings.debug && ImGui::CollapsingHeader("FACE DEBUGGER", ImGuiTreeNodeFlags_DefaultOpen))
        for (int i = 0; i < Mimic::DB_COUNT; ++i) if (ImGui::Checkbox(Mimic::debugName(i), &settings.debugItems[i])) again = true;

    if (ImGui::CollapsingHeader("FACE DETECTOR")) {
        if (combo("##det", settings.detect.model, DET_COUNT, detectorName)) again = true;
        static const int sizes[] = {160, 320, 480, 512, 640};
        const bool fixed = settings.detect.model == DET_YOLOFACE || settings.detect.model == DET_YUNET;
        ImGui::BeginDisabled(fixed);
        char cur[32]; snprintf(cur, sizeof cur, "%d x %d", fixed ? 640 : settings.detect.size, fixed ? 640 : settings.detect.size);
        if (ImGui::BeginCombo("##dsize", cur)) {
            for (int sz : sizes) { char l[32]; snprintf(l, sizeof l, "%d x %d", sz, sz); if (ImGui::Selectable(l, settings.detect.size == sz)) { settings.detect.size = sz; again = true; } }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Angles");
        const char* ang[4] = {"0", "90", "180", "270"};
        for (int i = 0; i < 4; ++i) { if (i) ImGui::SameLine(); if (ImGui::Checkbox(ang[i], &settings.detect.angles[i])) again = true; }
        ImGui::SliderFloat("##dscore", &settings.detect.score, 0.05f, 1.0f, "score: %.2f"); after();
        int mg = settings.detect.margin[0];
        if (ImGui::SliderInt("##margin", &mg, 0, 100, "margin: %d")) for (auto& m : settings.detect.margin) m = mg;
        after();
    }
    if (ImGui::CollapsingHeader("FACE LANDMARKER")) {
        if (combo("##lm", settings.detect.landmarker, LM_COUNT, landmarkerName)) again = true;
        ImGui::SliderFloat("##lscore", &settings.detect.landmarkerScore, 0.0f, 1.0f, "score: %.2f"); after();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its 68 points are used when it's at least this sure. 0: never (the detector's five only).");
    }
    if (ImGui::CollapsingHeader("FACE MASKER", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Types");
        if (ImGui::Checkbox("Box", &settings.maskBox)) again = true;
        ImGui::SameLine(); if (ImGui::Checkbox("Occlusion", &settings.maskOcclusion)) again = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hands, hair and glasses in front of the face stay in front (XSeg).");
        if (ImGui::Checkbox("Area", &settings.maskArea)) again = true;
        ImGui::SameLine(); if (ImGui::Checkbox("Region", &settings.maskRegion)) again = true;
        if (settings.maskOcclusion && combo("##occ", settings.occluder, Mimic::OCC_COUNT, Mimic::occluderName)) again = true;
        if (settings.maskArea) for (int i = 0; i < Mimic::AR_COUNT; ++i) { if (i) ImGui::SameLine(); if (ImGui::Checkbox(Mimic::areaName(i), &settings.areas[i])) again = true; }
        if (settings.maskRegion) {
            static const char* parsers[] = {"BiSeNet ResNet-18", "BiSeNet ResNet-34"};
            if (ImGui::Combo("##parser", &settings.parser, parsers, 2)) again = true;
            for (int i = 0; i < Mimic::RG_COUNT; ++i) { if (i % 2) ImGui::SameLine(cw / 2); if (ImGui::Checkbox(Mimic::regionName(i), &settings.regions[i])) again = true; }
        }
        ImGui::SliderFloat("##blur", &settings.maskBlur, 0.0f, 1.0f, "blur: %.2f"); after();
        const char* names[4] = {"padding top: %d%%", "padding right: %d%%", "padding bottom: %d%%", "padding left: %d%%"};
        for (int i = 0; i < 4; ++i) { ImGui::PushID(i); ImGui::SliderInt("##p", &settings.padding[i], 0, 100, names[i]); after(); ImGui::PopID(); }
    }
    if (ImGui::CollapsingHeader("OUTPUT")) {
        ImGui::SliderInt("##q", &jpegQuality, 1, 100, "image quality: %d");
        auto strCombo = [&](const char* id, std::string& v, const std::vector<std::string>& opts) {
            if (ImGui::BeginCombo(id, v.c_str())) { for (const auto& o : opts) if (ImGui::Selectable(o.c_str(), v == o)) v = o; ImGui::EndCombo(); }
        };
        ImGui::TextDisabled("Video");
        strCombo("##venc", output.videoEncoder, Video::videoEncoders());
        strCombo("##preset", output.preset, Video::presets());
        ImGui::SliderInt("##vq", &output.videoQuality, 0, 100, "video quality: %d");
        float fps = (float)output.fps;
        if (ImGui::SliderFloat("##fps", &fps, 0.0f, 60.0f, fps <= 0 ? "fps: as the target" : "fps: %.2f")) output.fps = fps;
        int trim[2] = {(int)output.trimStart, (int)(output.trimEnd > 0 ? output.trimEnd : std::max<long>(1, targetInfo.frames))};
        if (targetIsVideo && targetInfo.frames > 0 && ImGui::SliderInt2("##trim", trim, 0, (int)targetInfo.frames, "frame %d")) {
            output.trimStart = std::min(trim[0], trim[1]); output.trimEnd = std::max(trim[0], trim[1]);
            if (output.trimEnd >= targetInfo.frames) output.trimEnd = 0;
        }
        ImGui::TextDisabled("Audio");
        strCombo("##aenc", output.audioEncoder, Video::audioEncoders());
        ImGui::SliderInt("##aq", &output.audioQuality, 0, 100, "audio quality: %d");
        ImGui::SliderInt("##av", &output.audioVolume, 0, 100, "volume: %d%%");
    }
    if (ImGui::CollapsingHeader("EXECUTION")) ImGui::TextWrapped("Running on the %s. (mimic-cli --execution-providers cpu runs on the CPU.)", Ort::device().c_str());
    ImGui::PopItemWidth();
    ImGui::EndChild();
    if (again) previewWanted = true;

    // the foot: save / render, progress, status, credit
    ImGui::SetCursorPosY(size.y - footH);
    ImGui::Separator();
    if (targetIsVideo) {
        if (progress >= 0) {
            ImGui::ProgressBar(progress, ImVec2(w, 8), "");
            if (ImGui::Button("Stop", ImVec2(w, 0))) stopRender = true;
        } else {
            ImGui::BeginDisabled(settings.swap && !source.ready());
            if (ImGui::Button("Render video...", ImVec2(w, 0))) renderVideo();
            ImGui::EndDisabled();
        }
    } else {
        ImGui::BeginDisabled(!haveResult);
        if (ImGui::Button("Save picture...", ImVec2(w, 0))) savePictureAs();
        ImGui::EndDisabled();
    }
    { std::lock_guard<std::mutex> g(mtx); ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", status.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::SetCursorPosY(size.y - 26);
    ImGui::TextDisabled("Mimic " MIMIC_VERSION " - part of Atelier");
    ImGui::End();
}

static void about() {
    if (showAbout) { ImGui::OpenPopup("About Mimic"); showAbout = false; }
    ImGui::SetNextWindowSize(ImVec2(Splash::W + 16, 0));
    if (ImGui::BeginPopupModal("About Mimic", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        Splash::draw(ImGui::GetWindowDrawList(), at.x, at.y, nullptr, -1);
        ImGui::Dummy(ImVec2(Splash::W, Splash::H));
        ImGui::Spacing();
        ImGui::TextWrapped("FaceFusion's face processors in C++: swap, enhance, debug, on pictures and videos. Part of Atelier, "
                           "Cursed Entertainment's creative suite.");
        ImGui::Spacing();
        ImGui::TextWrapped("Faces: SCRFD and ArcFace (InsightFace). Swap: inswapper (InsightFace, non-commercial) or HyperSwap "
                           "(FaceFusion, ResearchRAIL). Restore: GFPGAN, CodeFormer (S-Lab, non-commercial), GPEN. Occlusion: XSeg. "
                           "Run by ONNX Runtime with DirectML; video by ffmpeg.");
        ImGui::Spacing();
        ImGui::TextWrapped("Only use faces you have the right to use, and say when a picture or video has been changed.");
        ImGui::TextDisabled("Running on the %s", Ort::device().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("\xC2\xA9 Cursed Entertainment");
        if (ImGui::Button("Close", ImVec2(-1, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void frame() {
    std::vector<std::function<void()>> todo;
    { std::lock_guard<std::mutex> g(mtx); todo.swap(onMain); }
    for (auto& f : todo) f();
    for (auto& sp : sourcePics) if (sp.ready && !sp.tex) upload(sp.tex, sp.thumb);
    if (originalStale) { upload(originalTex, original); originalStale = false; }
    if (resultStale) { upload(resultTex, result); resultStale = false; }
    if (referenceTexStale) { upload(referenceTex, referenceThumb); referenceTexStale = false; }
    if (previewWanted && !busy && progress < 0) {
        bool empty; { std::lock_guard<std::mutex> g(mtx); empty = jobs.empty(); }
        if (empty) { previewWanted = false; runPreview(); }
    }
    keys();
    menuBar();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float side = 290, top = ImGui::GetFrameHeight();
    leftPanel(ImVec2(vp->Pos.x, vp->Pos.y + top), ImVec2(side, vp->Size.y - top));
    canvas(ImVec2(vp->Pos.x + side, vp->Pos.y + top), ImVec2(vp->Size.x - side * 2, vp->Size.y - top));
    rightPanel(ImVec2(vp->Pos.x + vp->Size.x - side, vp->Pos.y + top), ImVec2(side, vp->Size.y - top));
    about();
}

int main(int argc, char** argv) {
    std::string file, capture;
    std::vector<std::string> sources;
    std::vector<std::string> args;
#ifdef _WIN32
    { int n; LPWSTR* wa = CommandLineToArgvW(GetCommandLineW(), &n); if (wa) { for (int i = 1; i < n; ++i) args.push_back(fs::path(wa[i]).u8string()); LocalFree(wa); } }
#else
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
    (void)argc; (void)argv;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--capture" && i + 1 < args.size()) capture = args[++i];
        else if (args[i] == "--source" && i + 1 < args.size()) sources.push_back(args[++i]);
        else if (args[i] == "--edit" && i + 1 < args.size()) { settings.edit = true; Mimic::promptToEdits(args[++i], settings.edits); }
        else file = args[i];
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;   // shown after the splash
    window = SDL_CreateWindow("Mimic", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1500, 900, flags);
    if (!window) return 1;
    SDL_SetWindowMinimumSize(window, 1000, 600);
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
    char* bp = SDL_GetBasePath();
    SDL_Surface* icon = bp ? SDL_LoadBMP((std::string(bp) + "assets/icon.bmp").c_str()) : nullptr;
    if (bp) SDL_free(bp);
    if (icon) SDL_SetWindowIcon(window, icon);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    loadFont();
    applyStyle();

    // the splash: a small borderless window sharing the main window's OpenGL context, up while the face models load
    SDL_Window* splashWin = nullptr;
    if (capture.empty()) {
        splashWin = SDL_CreateWindow("Mimic", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)Splash::W, (int)Splash::H, SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS);
        if (splashWin && SDL_GL_MakeCurrent(splashWin, gl) != 0) { SDL_DestroyWindow(splashWin); splashWin = nullptr; SDL_GL_MakeCurrent(window, gl); }
        if (splashWin && icon) SDL_SetWindowIcon(splashWin, icon);
    }
    if (icon) SDL_FreeSurface(icon);
    ImGui_ImplSDL2_InitForOpenGL(splashWin ? splashWin : window, gl);
    ImGui_ImplOpenGL3_Init("#version 330");
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    static std::atomic<int> loaded{0};
    static std::atomic<bool> ready{false};
    const int toLoad = std::max(1, (int)Mimic::modelsFor(settings).size());
    worker = std::thread(workerLoop);
    queue([] {
        std::string e;
        if (!Ort::init(true, e)) { say(e); ready = true; return; }
        auto count = [](const std::string& s) { say(s); if (s.rfind("Loading ", 0) == 0) ++loaded; };
        if (!Mimic::prepare(settings, e, count)) say(e); else say("Ready, on the " + Ort::device());
        ready = true;
    });
    if (splashWin) {
        const Uint32 start = SDL_GetTicks();
        bool skip = false;
        for (;;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL2_ProcessEvent(&e);
                if (e.type == SDL_QUIT) quit = true;
                if (e.type == SDL_MOUSEBUTTONDOWN || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) skip = true;
            }
            const Uint32 t = SDL_GetTicks() - start;
            if (skip || quit || (ready && t >= 1500) || t >= 30000) break;
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            std::string line;
            { std::lock_guard<std::mutex> g(mtx); line = status; }
            const float p = ready ? 1.0f : 0.05f + 0.9f * std::min(1.0f, (float)loaded / toLoad);
            Splash::draw(ImGui::GetBackgroundDrawList(), 0, 0, ready ? "Ready" : line.c_str(), p);
            ImGui::Render();
            int sw, sh;
            SDL_GL_GetDrawableSize(splashWin, &sw, &sh);
            glViewport(0, 0, sw, sh);
            glClearColor(0.1f, 0.1f, 0.1f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(splashWin);
        }
        ImGui_ImplSDL2_Shutdown();
        SDL_DestroyWindow(splashWin);
        SDL_GL_MakeCurrent(window, gl);
        ImGui_ImplSDL2_InitForOpenGL(window, gl);
    }
    if (capture.empty()) { SDL_ShowWindow(window); SDL_MaximizeWindow(window); SDL_RaiseWindow(window); }
    if (settings.edit && sources.empty()) settings.swap = false;   // only the expression: no face to become needed
    if (!sources.empty()) addSources(sources);
    if (!file.empty()) loadTarget(file);

    int frames = 0, idle = 0;
    std::vector<std::string> drops;
    while (!quit) {
        SDL_Event e;
        drops.clear();
        bool dropLeft = false;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_DROPFILE) {
                drops.push_back(e.drop.file); SDL_free(e.drop.file);
                int mx, my; SDL_GetMouseState(&mx, &my);
                dropLeft = mx >= sourcePanelMin.x && mx < sourcePanelMax.x && my >= sourcePanelMin.y;
            }
        }
        if (!drops.empty()) dropFiles(drops, dropLeft);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frames;
        if (!capture.empty()) {
            bool quiet; { std::lock_guard<std::mutex> g(mtx); quiet = jobs.empty() && onMain.empty(); }
            idle = (quiet && !busy && !previewWanted) ? idle + 1 : 0;
            if (frames >= 8 && idle >= 6) {
                std::vector<unsigned char> px((size_t)w * h * 4);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                std::vector<unsigned char> flip(px.size());
                for (int y = 0; y < h; ++y) memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
                SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(flip.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
                SDL_SaveBMP(s, capture.c_str());
                SDL_FreeSurface(s);
                break;
            }
        }
        SDL_GL_SwapWindow(window);
    }
    stopRender = true;
    workerQuit = true;
    if (worker.joinable()) worker.join();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
