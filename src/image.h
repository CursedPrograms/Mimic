// Mimic - pictures and the face-alignment maths: RGB images, 2 x 3 affine matrices, the similarity fit
// that lines five face points up with a template (OpenCV's estimateAffinePartial2D, as FaceFusion uses), warping
// with bilinear sampling, pasting a crop back through a mask, and a Gaussian blur for masks.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

// rows y0..y1 shared out over the CPU's cores
template <class F> inline void parallelRows(int y0, int y1, F f) {
    const int n = y1 - y0;
    if (n <= 0) return;
    const int threads = std::max(1, std::min((int)std::thread::hardware_concurrency(), n / 16));
    if (threads == 1) { for (int y = y0; y < y1; ++y) f(y); return; }
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([=] { for (int y = y0 + t; y < y1; y += threads) f(y); });
    for (auto& th : pool) th.join();
}

struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> px;     // RGB, w*h*3
    Img() = default;
    Img(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 3, 0) {}
    bool empty() const { return w <= 0 || h <= 0; }
};

// a float crop: RGB 0..255, w*h*3
struct Crop {
    int w = 0, h = 0;
    std::vector<float> px;
    Crop() = default;
    Crop(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 3, 0.0f) {}
};

// x' = m[0] x + m[1] y + m[2],  y' = m[3] x + m[4] y + m[5]
struct Affine {
    double m[6] = {1, 0, 0, 0, 1, 0};
    void apply(double x, double y, double& ox, double& oy) const { ox = m[0] * x + m[1] * y + m[2]; oy = m[3] * x + m[4] * y + m[5]; }
    Affine inverse() const {
        const double det = m[0] * m[4] - m[1] * m[3];
        Affine r;
        if (std::fabs(det) < 1e-12) return r;
        r.m[0] = m[4] / det; r.m[1] = -m[1] / det; r.m[3] = -m[3] / det; r.m[4] = m[0] / det;
        r.m[2] = -(r.m[0] * m[2] + r.m[1] * m[5]);
        r.m[5] = -(r.m[3] * m[2] + r.m[4] * m[5]);
        return r;
    }
    double scale() const { return std::sqrt(std::fabs(m[0] * m[4] - m[1] * m[3])); }
};

// the least-squares similarity (scale, rotation, shift) taking src points onto dst points
inline Affine fitSimilarity(const float* src, const double* dst, int n) {
    double sx = 0, sy = 0, dx = 0, dy = 0;
    for (int i = 0; i < n; ++i) { sx += src[i * 2]; sy += src[i * 2 + 1]; dx += dst[i * 2]; dy += dst[i * 2 + 1]; }
    sx /= n; sy /= n; dx /= n; dy /= n;
    double num1 = 0, num2 = 0, den = 0;
    for (int i = 0; i < n; ++i) {
        const double x = src[i * 2] - sx, y = src[i * 2 + 1] - sy, u = dst[i * 2] - dx, v = dst[i * 2 + 1] - dy;
        num1 += x * u + y * v; num2 += x * v - y * u; den += x * x + y * y;
    }
    const double a = den > 0 ? num1 / den : 1, b = den > 0 ? num2 / den : 0;
    Affine r;
    r.m[0] = a; r.m[1] = -b; r.m[2] = dx - (a * sx - b * sy);
    r.m[3] = b; r.m[4] = a;  r.m[5] = dy - (b * sx + a * sy);
    return r;
}

// FaceFusion's templates (five points: eyes, nose, mouth corners), in 0..1 of the crop
enum Template { T_ARCFACE_112_V2, T_ARCFACE_128, T_FFHQ_512 };
inline const double* templatePoints(Template t) {
    static const double arc112v2[10] = {0.34191607, 0.46157411, 0.65653393, 0.45983393, 0.50022500, 0.64050536, 0.37097589, 0.82469196, 0.63151696, 0.82325089};
    static const double arc128[10] = {0.36167656, 0.40387734, 0.63696719, 0.40235469, 0.50019687, 0.56044219, 0.38710391, 0.72160547, 0.61507734, 0.72034453};
    static const double ffhq512[10] = {0.37691676, 0.46864664, 0.62285697, 0.46912813, 0.50123859, 0.61331904, 0.39308822, 0.72541100, 0.61150205, 0.72490465};
    return t == T_ARCFACE_112_V2 ? arc112v2 : t == T_ARCFACE_128 ? arc128 : ffhq512;
}
// the matrix taking the picture onto a size x size crop of the face
inline Affine alignFace(const float* kps5, Template t, int size) {
    double dst[10];
    const double* p = templatePoints(t);
    for (int i = 0; i < 10; ++i) dst[i] = p[i] * size;
    return fitSimilarity(kps5, dst, 5);
}

// bilinear sample, edges repeated
inline void sample(const Img& im, double x, double y, float* out) {
    x = std::clamp(x, 0.0, im.w - 1.0); y = std::clamp(y, 0.0, im.h - 1.0);
    const int x0 = (int)x, y0 = (int)y, x1 = std::min(im.w - 1, x0 + 1), y1 = std::min(im.h - 1, y0 + 1);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    const uint8_t *a = &im.px[((size_t)y0 * im.w + x0) * 3], *b = &im.px[((size_t)y0 * im.w + x1) * 3];
    const uint8_t *c = &im.px[((size_t)y1 * im.w + x0) * 3], *d = &im.px[((size_t)y1 * im.w + x1) * 3];
    for (int k = 0; k < 3; ++k) out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

// the crop the matrix M (picture -> crop) gives; shrinking more than 2x averages several samples (like INTER_AREA)
inline Crop warpCrop(const Img& im, const Affine& M, int w, int h) {
    Crop c(w, h);
    const Affine inv = M.inverse();
    const double s = 1.0 / std::max(1e-9, M.scale());              // picture pixels per crop pixel
    const int n = std::clamp((int)std::ceil(s - 0.01), 1, 4);      // samples per side
    parallelRows(0, h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float acc[3] = {0, 0, 0};
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    double px, py, ox = x + (i + 0.5) / n - 0.5, oy = y + (j + 0.5) / n - 0.5;
                    inv.apply(ox, oy, px, py);
                    float v[3]; sample(im, px, py, v);
                    acc[0] += v[0]; acc[1] += v[1]; acc[2] += v[2];
                }
            float* o = &c.px[((size_t)y * w + x) * 3];
            for (int k = 0; k < 3; ++k) o[k] = acc[k] / (n * n);
        }
    });
    return c;
}

inline void sampleCrop(const Crop& c, double x, double y, float* out) {
    x = std::clamp(x, 0.0, c.w - 1.0); y = std::clamp(y, 0.0, c.h - 1.0);
    const int x0 = (int)x, y0 = (int)y, x1 = std::min(c.w - 1, x0 + 1), y1 = std::min(c.h - 1, y0 + 1);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    for (int k = 0; k < 3; ++k) {
        auto P = [&](int xx, int yy) { return c.px[((size_t)yy * c.w + xx) * 3 + k]; };
        out[k] = (P(x0, y0) * (1 - fx) + P(x1, y0) * fx) * (1 - fy) + (P(x0, y1) * (1 - fx) + P(x1, y1) * fx) * fy;
    }
}
inline float sampleMask(const std::vector<float>& m, int w, int h, double x, double y) {
    if (x < -0.5 || y < -0.5 || x > w - 0.5 || y > h - 0.5) return 0.0f;   // outside the crop: nothing
    x = std::clamp(x, 0.0, w - 1.0); y = std::clamp(y, 0.0, h - 1.0);
    const int x0 = (int)x, y0 = (int)y, x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    return (m[(size_t)y0 * w + x0] * (1 - fx) + m[(size_t)y0 * w + x1] * fx) * (1 - fy) + (m[(size_t)y1 * w + x0] * (1 - fx) + m[(size_t)y1 * w + x1] * fx) * fy;
}

// the crop put back where M took it from, through its mask (0..1); blend scales the whole thing (the enhancers' blend)
inline void pasteBack(Img& im, const Crop& c, const std::vector<float>& mask, const Affine& M, float blend = 1.0f) {
    const Affine inv = M.inverse();
    double xs[4], ys[4];
    inv.apply(0, 0, xs[0], ys[0]); inv.apply(c.w, 0, xs[1], ys[1]); inv.apply(c.w, c.h, xs[2], ys[2]); inv.apply(0, c.h, xs[3], ys[3]);
    const int x1 = std::max(0, (int)std::floor(*std::min_element(xs, xs + 4))), x2 = std::min(im.w, (int)std::ceil(*std::max_element(xs, xs + 4)));
    const int y1 = std::max(0, (int)std::floor(*std::min_element(ys, ys + 4))), y2 = std::min(im.h, (int)std::ceil(*std::max_element(ys, ys + 4)));
    parallelRows(y1, y2, [&](int y) {
        for (int x = x1; x < x2; ++x) {
            double cx, cy;
            M.apply(x, y, cx, cy);
            const float a = std::clamp(sampleMask(mask, c.w, c.h, cx, cy), 0.0f, 1.0f) * blend;
            if (a <= 0.0f) continue;
            float v[3];
            sampleCrop(c, cx, cy, v);
            uint8_t* p = &im.px[((size_t)y * im.w + x) * 3];
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)(std::clamp(p[k] * (1 - a) + v[k] * a, 0.0f, 255.0f) + 0.5f);
        }
    });
}

inline void gaussianBlur(std::vector<float>& m, int w, int h, float sigma) {
    if (sigma <= 0.05f) return;
    const int r = std::max(1, (int)std::ceil(sigma * 3));
    std::vector<float> k(2 * r + 1), t(m.size());
    float sum = 0;
    for (int i = -r; i <= r; ++i) { k[i + r] = std::exp(-(i * i) / (2 * sigma * sigma)); sum += k[i + r]; }
    for (auto& v : k) v /= sum;
    parallelRows(0, h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = -r; i <= r; ++i) s += k[i + r] * m[(size_t)y * w + std::clamp(x + i, 0, w - 1)];
            t[(size_t)y * w + x] = s;
        }
    });
    parallelRows(0, h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = -r; i <= r; ++i) s += k[i + r] * t[(size_t)std::clamp(y + i, 0, h - 1) * w + x];
            m[(size_t)y * w + x] = s;
        }
    });
}

// FaceFusion's box mask: the crop's edges faded out; padding (top, right, bottom, left) in % of the crop
inline std::vector<float> makeBoxMask(int w, int h, float blur, const int padding[4]) {
    std::vector<float> m((size_t)w * h, 1.0f);
    const int amount = (int)(w * 0.5f * blur), area = std::max(amount / 2, 1);
    const int top = std::max(area, h * padding[0] / 100), right = std::max(area, w * padding[1] / 100);
    const int bottom = std::max(area, h * padding[2] / 100), left = std::max(area, w * padding[3] / 100);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (y < top || y >= h - bottom || x < left || x >= w - right) m[(size_t)y * w + x] = 0.0f;
    if (amount > 0) gaussianBlur(m, w, h, amount * 0.25f);
    return m;
}

// the same, kept: a mask for these settings is only made once
#include <map>
#include <mutex>
#include <tuple>
inline const std::vector<float>& boxMask(int w, int h, float blur, const int padding[4]) {
    static std::map<std::tuple<int, int, int, int, int, int, int>, std::vector<float>> cache;
    static std::mutex m;
    const auto key = std::make_tuple(w, h, (int)std::lround(blur * 1000), padding[0], padding[1], padding[2], padding[3]);
    std::lock_guard<std::mutex> g(m);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, makeBoxMask(w, h, blur, padding)).first;
    return it->second;
}
