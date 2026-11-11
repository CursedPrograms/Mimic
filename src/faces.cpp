// Mimic - faces (see faces.h)
#include "faces.h"
#include <algorithm>
#include <cmath>

namespace Faces {

bool detect(const Img& im, std::vector<Face>& out, std::string& err, float minScore, Ort::Say say) {
    out.clear();
    OrtSession* s = Ort::open("det_10g.onnx", err, say);
    if (!s) return false;
    // the picture into 640 x 640, top-left, shrunk to fit (never enlarged), RGB, (v - 127.5) / 128
    const int S = 640;
    const double scale = std::min(1.0, std::min((double)S / im.w, (double)S / im.h));
    const int sw = std::max(1, (int)std::lround(im.w * scale)), sh = std::max(1, (int)std::lround(im.h * scale));
    Affine M; M.m[0] = scale; M.m[4] = scale;
    const Crop small = warpCrop(im, M, sw, sh);
    Ort::Tensor in;
    in.name = Ort::inputNames(s)[0];
    in.shape = {1, 3, S, S};
    in.f.assign((size_t)3 * S * S, -127.5f / 128.0f);   // the padding is black
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
            for (int c = 0; c < 3; ++c) in.f[(size_t)c * S * S + (size_t)y * S + x] = (small.px[((size_t)y * sw + x) * 3 + c] - 127.5f) / 128.0f;
    std::vector<Ort::Tensor> ins = {in}, outs;
    if (!Ort::run(s, ins, outs, err)) return false;
    if (outs.size() < 9) { err = "The face detector gave too few outputs"; return false; }
    // outputs: scores for strides 8/16/32, then boxes, then the five points; two anchors per cell
    std::vector<Face> found;
    const int strides[3] = {8, 16, 32};
    for (int k = 0; k < 3; ++k) {
        const int st = strides[k], cols = S / st;
        const auto &sc = outs[k].f, &bb = outs[k + 3].f, &kp = outs[k + 6].f;
        for (size_t i = 0; i < sc.size(); ++i) {
            if (sc[i] < minScore) continue;
            const int cell = (int)i / 2;
            const float cx = (float)(cell % cols) * st, cy = (float)(cell / cols) * st;
            Face f;
            f.score = sc[i];
            f.box[0] = (cx - bb[i * 4] * st) / (float)scale; f.box[1] = (cy - bb[i * 4 + 1] * st) / (float)scale;
            f.box[2] = (cx + bb[i * 4 + 2] * st) / (float)scale; f.box[3] = (cy + bb[i * 4 + 3] * st) / (float)scale;
            for (int p = 0; p < 5; ++p) { f.kps[p * 2] = (cx + kp[i * 10 + p * 2] * st) / (float)scale; f.kps[p * 2 + 1] = (cy + kp[i * 10 + p * 2 + 1] * st) / (float)scale; }
            found.push_back(f);
        }
    }
    // keep the best of overlapping boxes (IoU 0.4, as FaceFusion)
    std::sort(found.begin(), found.end(), [](const Face& a, const Face& b) { return a.score > b.score; });
    for (const auto& f : found) {
        bool clash = false;
        for (const auto& k : out) {
            const float ix = std::max(0.0f, std::min(f.box[2], k.box[2]) - std::max(f.box[0], k.box[0]));
            const float iy = std::max(0.0f, std::min(f.box[3], k.box[3]) - std::max(f.box[1], k.box[1]));
            const float inter = ix * iy;
            if (inter / (f.area() + k.area() - inter + 1e-6f) > 0.4f) { clash = true; break; }
        }
        if (!clash) out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const Face& a, const Face& b) { return a.area() > b.area(); });
    return true;
}

bool embed(const Img& im, Face& f, std::string& err, Ort::Say say) {
    OrtSession* s = Ort::open("w600k_r50.onnx", err, say);
    if (!s) return false;
    const Affine M = alignFace(f.kps, T_ARCFACE_112_V2, 112);
    const Crop c = warpCrop(im, M, 112, 112);
    Ort::Tensor in;
    in.name = Ort::inputNames(s)[0];
    in.shape = {1, 3, 112, 112};
    in.f.resize((size_t)3 * 112 * 112);
    for (int i = 0; i < 112 * 112; ++i)
        for (int k = 0; k < 3; ++k) in.f[(size_t)k * 112 * 112 + i] = c.px[(size_t)i * 3 + k] / 127.5f - 1.0f;
    std::vector<Ort::Tensor> ins = {in}, outs;
    if (!Ort::run(s, ins, outs, err) || outs.empty() || outs[0].f.size() < 512) { if (err.empty()) err = "ArcFace gave nothing"; return false; }
    f.emb.assign(outs[0].f.begin(), outs[0].f.begin() + 512);
    double n = 0;
    for (float v : f.emb) n += (double)v * v;
    n = std::sqrt(std::max(n, 1e-12));
    f.embNorm.resize(512);
    for (int i = 0; i < 512; ++i) f.embNorm[i] = (float)(f.emb[i] / n);
    return true;
}

float distance(const Face& a, const Face& b) {
    if (a.embNorm.size() != 512 || b.embNorm.size() != 512) return 1.0f;
    double d = 0;
    for (int i = 0; i < 512; ++i) d += (double)a.embNorm[i] * b.embNorm[i];
    return (float)(1.0 - d);
}

}  // namespace Faces
