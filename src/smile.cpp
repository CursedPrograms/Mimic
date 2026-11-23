// Mimic - the StyleGAN smile: a smile the way FaceApp first made them. The face is aligned on FFHQ's template at
// 1024, found in StyleGAN2's space by e4e (Tov et al., MIT), moved along the "happy" direction on the middle layers
// (4-7: the mouth and cheeks, not the face's shape or colour), and drawn twice by the generator: as it was found, and
// smiling. Only the difference between the two goes onto the real face, round the mouth and cheeks, so the
// person's own skin, eyes and likeness stay; the teeth, the lifted cheeks and the creases come from StyleGAN.
//
// Models (on this PC in F:\AIModels\stylegan2): e4e_encoder.onnx and stylegan2_ffhq_generator.onnx, exported from
// e4e's FFHQ checkpoint (the StyleGAN2 FFHQ weights are NVIDIA's: non-commercial use), and the direction
// directions\emotion_happy.npy (a312863063/generators-with-stylegan2).
#include "mimic.h"
#include "ort.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace Mimic {

static const int SG_SIZE = 1024, SG_ENC = 256, SG_LAYERS = 18, SG_DIM = 512;
static const int EDIT_FROM = 4, EDIT_TO = 7;     // W+ layers the smile moves
static const float SG_REACH = 3.0f;              // the dial's +1 is this far along the direction

// a .npy of doubles (or floats), C order: the direction (18 x 512)
static bool loadNpy(const std::string& path, std::vector<float>& out, std::string& err) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    char magic[6] = {};
    if (!f.read(magic, 6) || memcmp(magic, "\x93NUMPY", 6) != 0) { err = path + " isn't a .npy"; return false; }
    unsigned char ver[2] = {};
    f.read((char*)ver, 2);
    uint32_t hlen = 0;
    if (ver[0] == 1) { uint16_t h = 0; f.read((char*)&h, 2); hlen = h; } else f.read((char*)&hlen, 4);
    std::string header(hlen, ' ');
    f.read(&header[0], hlen);
    const bool f8 = header.find("<f8") != std::string::npos, f4 = header.find("<f4") != std::string::npos;
    if ((!f8 && !f4) || header.find("True") != std::string::npos) { err = path + ": not a C-ordered float array"; return false; }
    out.resize((size_t)SG_LAYERS * SG_DIM);
    for (size_t i = 0; i < out.size(); ++i) {
        if (f8) { double d = 0; f.read((char*)&d, 8); out[i] = (float)d; }
        else { float v = 0; f.read((char*)&v, 4); out[i] = v; }
    }
    if (!f) { err = path + " is shorter than 18 x 512"; return false; }
    return true;
}

static const std::vector<float>* happyDirection(std::string& err) {
    static std::vector<float> dir;
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    if (!dir.empty()) return &dir;
    const std::string p = Ort::findModel("emotion_happy.npy");
    if (p.empty()) { err = "emotion_happy.npy isn't on this PC (F:\\AIModels\\stylegan2\\directions)"; return nullptr; }
    if (!loadNpy(p, dir, err)) { dir.clear(); return nullptr; }
    return &dir;
}

// the soft lower face in the 1024 crop: the mouth, the cheeks, the chin (FFHQ's layout)
static const std::vector<float>& smileMask() {
    static std::vector<float> m;
    static std::once_flag once;
    std::call_once(once, [] {
        const int N = SG_SIZE;
        m.assign((size_t)N * N, 0.0f);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const float dx = (x - 512.0f) / 300.0f, dy = (y - 690.0f) / 230.0f;
                m[(size_t)y * N + x] = std::clamp(1.2f - (dx * dx + dy * dy), 0.0f, 1.0f);
            }
        gaussianBlur(m, N, N, 40.0f);
    });
    return m;
}

static Ort::Tensor tensor(const char* name, std::vector<int64_t> shape, std::vector<float> f) {
    Ort::Tensor t; t.name = name; t.shape = std::move(shape); t.f = std::move(f); return t;
}

bool styleganSmile(Img& frame, const Face& target, float dial, float strength, std::string& err) {
    if (std::fabs(dial) < 1e-4f) return true;
    const std::vector<float>* dir = happyDirection(err);
    if (!dir) return false;
    OrtSession* enc = Ort::open("e4e_encoder.onnx", err);
    if (!enc) return false;
    OrtSession* gen = Ort::open("stylegan2_ffhq_generator.onnx", err);
    if (!gen) return false;

    // the face on FFHQ's template: 256 for the encoder, 1024 for the generator's frame
    const Affine M = alignFace(target.kps, T_FFHQ_512, SG_SIZE), Ms = alignFace(target.kps, T_FFHQ_512, SG_ENC);
    const Crop small = warpCrop(frame, Ms, SG_ENC, SG_ENC);
    std::vector<float> in((size_t)3 * SG_ENC * SG_ENC);
    for (int i = 0; i < SG_ENC * SG_ENC; ++i)
        for (int c = 0; c < 3; ++c) in[(size_t)c * SG_ENC * SG_ENC + i] = small.px[(size_t)i * 3 + c] / 127.5f - 1.0f;
    std::vector<Ort::Tensor> ins = {tensor("image", {1, 3, SG_ENC, SG_ENC}, in)}, outs;
    if (!Ort::run(enc, ins, outs, err) || outs.empty() || outs[0].f.size() < (size_t)SG_LAYERS * SG_DIM) {
        if (err.empty()) err = "e4e gave too little";
        return false;
    }
    std::vector<float> w = outs[0].f;
    w.resize((size_t)SG_LAYERS * SG_DIM);
    std::vector<float> we = w;
    const float amount = dial * SG_REACH * std::clamp(strength, 0.0f, 3.0f);
    for (int l = EDIT_FROM; l <= EDIT_TO; ++l)
        for (int k = 0; k < SG_DIM; ++k) we[(size_t)l * SG_DIM + k] += amount * (*dir)[(size_t)l * SG_DIM + k];

    auto draw = [&](const std::vector<float>& lat, std::vector<float>& img) {
        std::vector<Ort::Tensor> gi = {tensor("w_plus", {1, SG_LAYERS, SG_DIM}, lat)}, go;
        if (!Ort::run(gen, gi, go, err) || go.empty() || go[0].f.size() < (size_t)3 * SG_SIZE * SG_SIZE) {
            if (err.empty()) err = "StyleGAN's generator gave too little";
            return false;
        }
        img = std::move(go[0].f);
        return true;
    };
    std::vector<float> recon, smiling;
    if (!draw(w, recon) || !draw(we, smiling)) return false;

    // the real face plus the change, round the mouth and cheeks
    Crop out = warpCrop(frame, M, SG_SIZE, SG_SIZE);
    const size_t plane = (size_t)SG_SIZE * SG_SIZE;
    for (size_t i = 0; i < plane; ++i)
        for (int c = 0; c < 3; ++c) {
            const float d = (smiling[c * plane + i] - recon[c * plane + i]) * 127.5f;
            out.px[i * 3 + c] = std::clamp(out.px[i * 3 + c] + d, 0.0f, 255.0f);
        }
    pasteBack(frame, out, smileMask(), M);
    return true;
}

}  // namespace Mimic
