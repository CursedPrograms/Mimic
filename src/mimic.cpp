// Mimic - the swap (see mimic.h)
#include "mimic.h"
#include "emap.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace Mimic {

const char* swapperName(int s) { return s == SW_HYPERSWAP ? "HyperSwap 1a (256)" : "inswapper (128)"; }
const char* enhancerName(int e) {
    switch (e) { case EN_GFPGAN: return "GFPGAN 1.4"; case EN_CODEFORMER: return "CodeFormer"; case EN_GPEN: return "GPEN 512"; default: return "None"; }
}
const char* whichName(int w) {
    switch (w) { case W_LARGEST: return "The biggest face"; case W_LIKE_REFERENCE: return "Faces like the reference"; default: return "Every face"; }
}

static const char* swapperFile(int s) { return s == SW_HYPERSWAP ? "hyperswap_1a_256.onnx" : "inswapper_128.onnx"; }
static int swapperSize(int s) { return s == SW_HYPERSWAP ? 256 : 128; }
static const char* enhancerFile(int e) {
    switch (e) { case EN_GFPGAN: return "gfpgan_1.4.onnx"; case EN_CODEFORMER: return "codeformer.onnx"; case EN_GPEN: return "gpen_bfr_512.onnx"; default: return nullptr; }
}

bool addSource(Source& src, const Img& picture, std::string& err, Ort::Say say) {
    std::vector<Face> faces;
    if (!Faces::detect(picture, faces, err, 0.5f, say)) return false;
    if (faces.empty()) { err = "No face in that picture"; return false; }
    Face f = faces[0];   // the biggest
    if (!Faces::embed(picture, f, err, say)) return false;
    src.faces.push_back(f);
    finishSource(src);
    return true;
}

void finishSource(Source& src) {
    src.emb.clear(); src.embNorm.clear();
    if (src.faces.empty()) return;
    src.emb.assign(512, 0.0f);
    for (const auto& f : src.faces) for (int i = 0; i < 512; ++i) src.emb[i] += f.emb[i] / src.faces.size();
    double n = 0;
    for (float v : src.emb) n += (double)v * v;
    n = std::sqrt(std::max(n, 1e-12));
    src.embNorm.resize(512);
    for (int i = 0; i < 512; ++i) src.embNorm[i] = (float)(src.emb[i] / n);
}

// MIMIC_PROFILE=1: how long each step takes, per frame
static const bool profile = std::getenv("MIMIC_PROFILE") != nullptr;
static double tick() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static std::vector<float> emapCache;
static std::mutex emapLock;

bool prepare(const Settings& st, std::string& err, Ort::Say say) {
    if (!Ort::open("det_10g.onnx", err, say) || !Ort::open("w600k_r50.onnx", err, say)) return false;
    if (!Ort::open(swapperFile(st.swapper), err, say)) return false;
    if (st.swapper == SW_INSWAPPER) {
        std::lock_guard<std::mutex> g(emapLock);
        if (emapCache.empty() && !readEmap(Ort::findModel("inswapper_128.onnx"), emapCache, err)) return false;
    }
    if (enhancerFile(st.enhancer) && !Ort::open(enhancerFile(st.enhancer), err, say)) return false;
    if (st.occlusion && !Ort::open("xseg_1.onnx", err, say)) return false;
    return true;
}

// what the swapper is given as "source": inswapper wants the identity through its emap, HyperSwap the unit identity;
// both then lean towards the target's own identity by weight (FaceFusion: 0..1 -> +0.35..-0.35)
static std::vector<float> sourceInput(const Source& src, const Face& target, const Settings& st) {
    std::vector<float> s(512);
    if (st.swapper == SW_INSWAPPER) {
        double n = 0;
        for (float v : src.emb) n += (double)v * v;
        n = std::sqrt(std::max(n, 1e-12));
        for (int j = 0; j < 512; ++j) {
            double acc = 0;
            for (int i = 0; i < 512; ++i) acc += (double)src.emb[i] * emapCache[(size_t)i * 512 + j];
            s[j] = (float)(acc / n);
        }
    } else s = src.embNorm;
    const float w = 0.35f - 0.7f * std::clamp(st.weight, 0.0f, 1.0f);
    if (std::fabs(w) > 1e-4f && target.embNorm.size() == 512)
        for (int i = 0; i < 512; ++i) s[i] = s[i] * (1 - w) + target.embNorm[i] * w;
    return s;
}

// XSeg's occlusion mask on a crop: 1 where the face shows, 0 where something is in front of it
static bool occlusionMask(const Crop& crop, std::vector<float>& mask, std::string& err) {
    OrtSession* s = Ort::open("xseg_1.onnx", err);
    if (!s) return false;
    const int N = 256;
    Ort::Tensor in;
    in.name = Ort::inputNames(s)[0];
    in.shape = {1, N, N, 3};
    in.f.resize((size_t)N * N * 3);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float v[3];
            sampleCrop(crop, (x + 0.5) * crop.w / N - 0.5, (y + 0.5) * crop.h / N - 0.5, v);
            for (int k = 0; k < 3; ++k) in.f[((size_t)y * N + x) * 3 + k] = v[2 - k] / 255.0f;   // BGR (DeepFaceLab's)
        }
    std::vector<Ort::Tensor> ins = {in}, outs;
    if (!Ort::run(s, ins, outs, err) || outs.empty()) return false;
    mask.assign((size_t)crop.w * crop.h, 0.0f);
    for (int y = 0; y < crop.h; ++y)
        for (int x = 0; x < crop.w; ++x)
            mask[(size_t)y * crop.w + x] = std::clamp(sampleMask(outs[0].f, N, N, (x + 0.5) * N / crop.w - 0.5, (y + 0.5) * N / crop.h - 0.5), 0.0f, 1.0f);
    gaussianBlur(mask, crop.w, crop.h, 5.0f * crop.w / 256.0f);
    for (auto& m : mask) m = (std::clamp(m, 0.5f, 1.0f) - 0.5f) * 2.0f;
    return true;
}

static bool swapOne(Img& frame, const Face& target, const Source& src, const Settings& st, std::string& err) {
    OrtSession* s = Ort::open(swapperFile(st.swapper), err);
    if (!s) return false;
    const int size = swapperSize(st.swapper), boost = std::clamp(st.boost, 1, 4), big = size * boost;
    const Affine M = alignFace(target.kps, T_ARCFACE_128, big);
    Crop crop = warpCrop(frame, M, big, big);
    std::vector<float> mask = boxMask(big, big, st.maskBlur, st.padding);
    if (st.occlusion) {
        std::vector<float> occ;
        if (!occlusionMask(crop, occ, err)) return false;
        for (size_t i = 0; i < mask.size(); ++i) mask[i] = std::min(mask[i], occ[i]);
    }
    const std::vector<float> source = sourceInput(src, target, st);
    const bool hyper = st.swapper == SW_HYPERSWAP;
    const auto names = Ort::inputNames(s);
    Crop result(big, big);
    // pixel boost: pass (i, j) takes every boost-th pixel from offset (i, j), so each pass is model-sized
    for (int j = 0; j < boost; ++j)
        for (int i = 0; i < boost; ++i) {
            Ort::Tensor tgt, srcT;
            tgt.name = "target"; tgt.shape = {1, 3, size, size}; tgt.f.resize((size_t)3 * size * size);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    const float* p = &crop.px[((size_t)(y * boost + j) * big + (x * boost + i)) * 3];
                    for (int k = 0; k < 3; ++k) {
                        const float v = p[k] / 255.0f;
                        tgt.f[(size_t)k * size * size + (size_t)y * size + x] = hyper ? (v - 0.5f) / 0.5f : v;
                    }
                }
            srcT.name = "source"; srcT.shape = {1, 512}; srcT.f = source;
            std::vector<Ort::Tensor> ins, outs;
            for (const auto& n : names) ins.push_back(n == "source" ? srcT : tgt);
            if (!Ort::run(s, ins, outs, err) || outs.empty()) return false;
            const auto& o = outs[0].f;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    float* p = &result.px[((size_t)(y * boost + j) * big + (x * boost + i)) * 3];
                    for (int k = 0; k < 3; ++k) {
                        float v = o[(size_t)k * size * size + (size_t)y * size + x];
                        if (hyper) v = v * 0.5f + 0.5f;
                        p[k] = std::clamp(v, 0.0f, 1.0f) * 255.0f;
                    }
                }
        }
    // (HyperSwap also gives a face mask, but a weak one, 0..~0.5: like FaceFusion, the box and occlusion masks only)
    const double tp = tick();
    pasteBack(frame, result, mask, M);
    if (profile) std::fprintf(stderr, "  swap paste %.0f ms\n", tick() - tp);
    return true;
}

static bool enhanceOne(Img& frame, const Face& target, const Settings& st, std::string& err) {
    OrtSession* s = Ort::open(enhancerFile(st.enhancer), err);
    if (!s) return false;
    const int N = 512;
    const Affine M = alignFace(target.kps, T_FFHQ_512, N);
    double tw = tick();
    const Crop crop = warpCrop(frame, M, N, N);
    if (profile) std::fprintf(stderr, "  enh warp %.0f ms\n", tick() - tw);
    tw = tick();
    const int pad0[4] = {0, 0, 0, 0};
    std::vector<float> mask = boxMask(N, N, st.maskBlur, pad0);
    if (st.occlusion) {
        std::vector<float> occ;
        if (!occlusionMask(crop, occ, err)) return false;
        for (size_t i = 0; i < mask.size(); ++i) mask[i] = std::min(mask[i], occ[i]);
    }
    std::vector<Ort::Tensor> ins, outs;
    for (const auto& n : Ort::inputNames(s)) {
        Ort::Tensor t;
        t.name = n;
        if (n == "weight") { t.isDouble = true; t.d = std::clamp(st.codeformerFidelity, 0.0f, 1.0f); }
        else {
            t.shape = {1, 3, N, N};
            t.f.resize((size_t)3 * N * N);
            for (int i = 0; i < N * N; ++i)
                for (int k = 0; k < 3; ++k) t.f[(size_t)k * N * N + i] = (crop.px[(size_t)i * 3 + k] / 255.0f - 0.5f) / 0.5f;
        }
        ins.push_back(t);
    }
    if (!Ort::run(s, ins, outs, err) || outs.empty()) return false;
    if (profile) std::fprintf(stderr, "  enh model+mask %.0f ms\n", tick() - tw);
    tw = tick();
    Crop out(N, N);
    const float* o = outs[0].f.data();
    parallelRows(0, N, [&](int y) {
        for (int i = y * N; i < (y + 1) * N; ++i)
            for (int k = 0; k < 3; ++k) out.px[(size_t)i * 3 + k] = (std::clamp(o[(size_t)k * N * N + i], -1.0f, 1.0f) + 1.0f) * 127.5f;
    });
    pasteBack(frame, out, mask, M, std::clamp(st.enhanceBlend, 0.0f, 1.0f));
    if (profile) std::fprintf(stderr, "  enh paste %.0f ms\n", tick() - tw);
    return true;
}

int swapFrame(Img& frame, const Source& src, const Settings& st, const Reference& ref, std::string& err, Ort::Say say) {
    if (!src.ready()) { err = "Pick the face to become first"; return -1; }
    if (!prepare(st, err, say)) return -1;
    double t = tick(), tDet = 0, tEmb = 0, tSwap = 0, tEnh = 0;
    std::vector<Face> faces;
    if (!Faces::detect(frame, faces, err, st.minScore)) return -1;
    tDet = tick() - t; t = tick();
    if (faces.empty()) return 0;
    std::vector<Face> targets;
    if (st.which == W_LARGEST) targets.push_back(faces[0]);
    else targets = faces;
    // identities: needed for the weight blend, and to pick the faces like the reference
    for (auto& f : targets) if (!Faces::embed(frame, f, err)) return -1;
    tEmb = tick() - t; t = tick();
    if (st.which == W_LIKE_REFERENCE) {
        if (!ref.set) { err = "Pick a reference face first"; return -1; }
        targets.erase(std::remove_if(targets.begin(), targets.end(), [&](const Face& f) { return Faces::distance(f, ref.face) > st.referenceDistance; }), targets.end());
    }
    for (const auto& f : targets) if (!swapOne(frame, f, src, st, err)) return -1;
    tSwap = tick() - t; t = tick();
    if (st.enhancer != EN_NONE)
        for (const auto& f : targets) if (!enhanceOne(frame, f, st, err)) return -1;
    tEnh = tick() - t;
    if (profile) std::fprintf(stderr, "detect %.0f  embed %.0f  swap %.0f  enhance %.0f ms\n", tDet, tEmb, tSwap, tEnh);
    return (int)targets.size();
}

}  // namespace Mimic
