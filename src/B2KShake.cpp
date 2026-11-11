// B2K Shake - an original camera-shake OFX plugin (clean-room, inspired by the
// public feature set of shake effects like S_Shake). Written against the raw
// OpenFX C API, CPU only, RGBA 8/16/32f.
//
// Parameters (per axis X / Y / Z(zoom) / Tilt):
//   Rand Amp/Freq  - smooth random motion
//   Wave Amp/Freq  - sine motion
//   Phase          - offset in shake cycles
// Global: Style, Amplitude, Frequency (cycles/sec), Phase, Z Dist, Motion Blur,
// Mo Blur Length (frames), Seed (continuous, animatable), Wrap X/Y,
// per-channel amplitude/phase + RGB randomness, Crop Input.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ofxImageEffect.h"
#include "ofxGPURender.h"
#include "cl_min.h"
#include <GL/gl.h>
#include "ofxDrawSuite.h"
#pragma comment(lib, "opengl32.lib")

#if defined(_WIN32)
#define SFX_EXPORT extern "C" __declspec(dllexport)
#else
#define SFX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEffect = nullptr;
static const OfxParameterSuiteV1* gParam = nullptr;

static const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- noise ----
static inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
static inline double rnd(int64_t i, uint32_t stream, uint32_t seed) {
    return hash3((uint32_t)i, stream, seed) * (2.0 / 4294967295.0) - 1.0;
}

enum Style { kStyleNormal = 0, kStyleSmooth = 1, kStyleJerky = 2 };

// One octave of 1D value noise, roughly in [-1, 1].
static double octave(double x, uint32_t stream, uint32_t seed, int style) {
    double fl = std::floor(x);
    int64_t i = (int64_t)fl;
    double t = x - fl;
    if (style == kStyleJerky) {
        double a = rnd(i, stream, seed), b = rnd(i + 1, stream, seed);
        return a + (b - a) * t;
    }
    double p0 = rnd(i - 1, stream, seed), p1 = rnd(i, stream, seed);
    double p2 = rnd(i + 1, stream, seed), p3 = rnd(i + 2, stream, seed);
    // Catmull-Rom: C1-continuous -> smooth velocity, no visible kinks
    return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}

static double noiseSeeded(double x, uint32_t stream, uint32_t seed, int style) {
    if (style == kStyleNormal) {
        // a little fractal detail gives a more organic, hand-held feel
        return (octave(x, stream, seed, style) + 0.4 * octave(2.03 * x + 17.31, stream + 101, seed, style) +
                0.15 * octave(4.11 * x + 41.7, stream + 211, seed, style)) / 1.2;
    }
    return octave(x, stream, seed, style);
}

// Continuous seed: integer steps give new patterns, fractions blend smoothly.
static double noise(double x, uint32_t stream, double seed, int style) {
    double s = seed * 1000.0;
    double fl = std::floor(s);
    double f = s - fl;
    uint32_t si = (uint32_t)(int64_t)fl;
    double a = noiseSeeded(x, stream, si, style);
    if (f < 1e-9) return a;
    double b = noiseSeeded(x, stream, si + 1, style);
    f = f * f * (3.0 - 2.0 * f);
    return a + (b - a) * f;
}

// --------------------------------------------------------------- params ----
struct Axis { double randAmp, randFreq, waveAmp, waveFreq, phase; };

struct Params {
    int style;
    double amp, freq, phase, zdist;
    bool motionBlur;
    double mbLen, seed;
    int wrapX, wrapY;
    Axis ax[4];  // x, y, z, tilt
    double chAmp[3], chPhase[3], rgbRand, rgbFreq;
    double crop[4];  // left, right, top, bottom
};

static const char* kAxisPrefix[4] = {"x", "y", "z", "tilt"};
static const char* kAxisLabel[4] = {"X", "Y", "Z", "Tilt"};

static double getD(OfxParamSetHandle ps, const char* name, OfxTime t) {
    OfxParamHandle h; double v = 0;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) == kOfxStatOK) gParam->paramGetValueAtTime(h, t, &v);
    return v;
}
static int getI(OfxParamSetHandle ps, const char* name, OfxTime t) {
    OfxParamHandle h; int v = 0;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) == kOfxStatOK) gParam->paramGetValueAtTime(h, t, &v);
    return v;
}

static void readParams(OfxImageEffectHandle effect, OfxTime t, Params& p) {
    OfxParamSetHandle ps; gEffect->getParamSet(effect, &ps);
    p.style = getI(ps, "style", t);
    p.amp = getD(ps, "amplitude", t);
    p.freq = getD(ps, "frequency", t);
    p.phase = getD(ps, "phase", t);
    p.zdist = getD(ps, "zDist", t);
    p.motionBlur = getI(ps, "motionBlur", t) != 0;
    p.mbLen = getD(ps, "moBlurLength", t);
    p.seed = getD(ps, "seed", t);
    p.wrapX = getI(ps, "wrapX", t);
    p.wrapY = getI(ps, "wrapY", t);
    char n[64];
    for (int k = 0; k < 4; ++k) {
        snprintf(n, sizeof n, "%sRandAmp", kAxisPrefix[k]);  p.ax[k].randAmp = getD(ps, n, t);
        snprintf(n, sizeof n, "%sRandFreq", kAxisPrefix[k]); p.ax[k].randFreq = getD(ps, n, t);
        snprintf(n, sizeof n, "%sWaveAmp", kAxisPrefix[k]);  p.ax[k].waveAmp = getD(ps, n, t);
        snprintf(n, sizeof n, "%sWaveFreq", kAxisPrefix[k]); p.ax[k].waveFreq = getD(ps, n, t);
        snprintf(n, sizeof n, "%sPhase", kAxisPrefix[k]);    p.ax[k].phase = getD(ps, n, t);
    }
    p.chAmp[0] = getD(ps, "redAmp", t);   p.chAmp[1] = getD(ps, "greenAmp", t);   p.chAmp[2] = getD(ps, "blueAmp", t);
    p.chPhase[0] = getD(ps, "redPhase", t); p.chPhase[1] = getD(ps, "greenPhase", t); p.chPhase[2] = getD(ps, "bluePhase", t);
    p.rgbRand = getD(ps, "rgbRandomness", t);
    p.rgbFreq = getD(ps, "rgbFrequency", t);
    p.crop[0] = getD(ps, "cropLeft", t);  p.crop[1] = getD(ps, "cropRight", t);
    p.crop[2] = getD(ps, "cropTop", t);   p.crop[3] = getD(ps, "cropBottom", t);
}

// shake values at time (seconds) for channel ch (-1 = base / all channels)
static void shakeAt(const Params& p, double tsec, int ch, double out[4]) {
    double cyc = tsec * p.freq;
    for (int k = 0; k < 4; ++k) {
        const Axis& a = p.ax[k];
        double ph = p.phase + a.phase + (ch >= 0 ? p.chPhase[ch] : 0.0);
        double v = 0;
        if (a.randAmp != 0) v += a.randAmp * noise(cyc * a.randFreq + ph, (uint32_t)k, p.seed, p.style);
        if (a.waveAmp != 0) v += a.waveAmp * std::sin(2.0 * kPi * (cyc * a.waveFreq + ph));
        if (ch >= 0 && p.rgbRand != 0)
            v += p.rgbRand * (std::fabs(a.randAmp) + std::fabs(a.waveAmp)) *
                 noise(cyc * p.rgbFreq + ph, 16u + (uint32_t)(k * 4 + ch), p.seed, p.style);
        out[k] = p.amp * (ch >= 0 ? p.chAmp[ch] : 1.0) * v;
    }
}

// Affine map output pixel -> source pixel: S = (a X + b Y + c, d X + e Y + f)
struct Affine { double a, b, c, d, e, f; };

static Affine makeAffine(const double s4[4], double cx, double cy, double refW, double par, double zdist) {
    // Z Dist = camera distance to the image: a static zoom (1 = none, <1 zoom in,
    // >1 zoom out). Z shake moves the image towards the camera: scale 1 / (zd - z).
    // X/Y shake stays in screen space, independent of Z Dist.
    double zd = std::max(zdist, 0.01);
    double dx = s4[0] * refW, dy = s4[1] * refW, z = s4[2];
    double th = s4[3] * kPi / 180.0;
    double s = 1.0 / std::max(zd - z, 0.02);
    double cs = std::cos(th), sn = std::sin(th);
    Affine m;
    m.a = cs / s;
    m.b = sn / (s * par);
    m.c = cx + (cs * (-cx * par - dx) + sn * (-cy - dy)) / (s * par);
    m.d = -sn * par / s;
    m.e = cs / s;
    m.f = cy + (-sn * (-cx * par - dx) + cs * (-cy - dy)) / s;
    return m;
}

// ------------------------------------------------------------- sampling ----
struct SrcImg {
    const char* data; int x1, y1, x2, y2; int rowBytes;
    int cx1, cy1, cw, ch;  // crop rect (pixels)
    int wrapX, wrapY;
};

static inline bool wrapIdx(int& i, int lo, int n, int mode) {
    if (i >= lo && i < lo + n) return true;
    switch (mode) {
        case 0: return false;  // Black
        case 1: { int m = (i - lo) % n; if (m < 0) m += n; i = lo + m; return true; }  // Repeat
        case 2: { int pd = 2 * n; int m = (i - lo) % pd; if (m < 0) m += pd; if (m >= n) m = pd - 1 - m; i = lo + m; return true; }  // Reflect
        default: i = i < lo ? lo : lo + n - 1; return true;  // Extend
    }
}

template <class T> struct PixTraits;
template <> struct PixTraits<unsigned char> { static constexpr float scale = 255.f; };
template <> struct PixTraits<unsigned short> { static constexpr float scale = 65535.f; };
template <> struct PixTraits<float> { static constexpr float scale = 1.f; };

template <class T>
static inline void sample(const SrcImg& s, double fx, double fy, float out[4]) {
    double x = fx - 0.5, y = fy - 0.5;
    double flx = std::floor(x), fly = std::floor(y);
    int ix = (int)flx, iy = (int)fly;
    float tx = (float)(x - flx), ty = (float)(y - fly);
    int xs[2] = {ix, ix + 1}, ys[2] = {iy, iy + 1};
    bool okx[2], oky[2];
    for (int k = 0; k < 2; ++k) { okx[k] = wrapIdx(xs[k], s.cx1, s.cw, s.wrapX); oky[k] = wrapIdx(ys[k], s.cy1, s.ch, s.wrapY); }
    float wx[2] = {1.f - tx, tx}, wy[2] = {1.f - ty, ty};
    out[0] = out[1] = out[2] = out[3] = 0.f;
    const float inv = 1.f / PixTraits<T>::scale;
    for (int j = 0; j < 2; ++j) {
        if (!oky[j] || wy[j] == 0.f) continue;
        const T* row = (const T*)(s.data + (ptrdiff_t)(ys[j] - s.y1) * s.rowBytes);
        for (int i = 0; i < 2; ++i) {
            if (!okx[i] || wx[i] == 0.f) continue;
            const T* p = row + (ptrdiff_t)(xs[i] - s.x1) * 4;
            float w = wx[i] * wy[j] * inv;
            out[0] += p[0] * w; out[1] += p[1] * w; out[2] += p[2] * w; out[3] += p[3] * w;
        }
    }
}

template <class T> static inline T toPix(float v);
template <> inline unsigned char toPix<unsigned char>(float v) { v = v * 255.f + 0.5f; return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
template <> inline unsigned short toPix<unsigned short>(float v) { v = v * 65535.f + 0.5f; return (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
template <> inline float toPix<float>(float v) { return v; }

// Samples source A, B or a cross-fade of both (w = 0 -> A, w = 1 -> B).
template <class T>
static inline void sampleMix(const SrcImg& a, const SrcImg& b, float w, double fx, double fy, float out[4]) {
    const SrcImg& one = w >= 1.f ? b : a;
    if (w <= 0.f || w >= 1.f) {
        if (one.cw <= 0 || one.ch <= 0) { out[0] = out[1] = out[2] = out[3] = 0.f; return; }
        sample<T>(one, fx, fy, out);
        return;
    }
    float va[4] = {0, 0, 0, 0}, vb[4] = {0, 0, 0, 0};
    if (a.cw > 0 && a.ch > 0) sample<T>(a, fx, fy, va);
    if (b.cw > 0 && b.ch > 0) sample<T>(b, fx, fy, vb);
    for (int k = 0; k < 4; ++k) out[k] = va[k] + (vb[k] - va[k]) * w;
}

template <class T>
static void renderT(const SrcImg& src, const SrcImg& srcB, float mixW, char* dstData, int dx1, int dy1,
                    int dstRowBytes, const int rw[4], const std::vector<Affine>& mats, int nSamples, bool perChannel) {
    const int rows = rw[3] - rw[1];
    if (rows <= 0 || rw[2] <= rw[0]) return;
    const bool empty = false;
    const int nch = perChannel ? 3 : 1;
    const float invN = 1.f / (float)nSamples;

    auto work = [&](int tid, int nThreads) {
        for (int y = rw[1] + tid; y < rw[3]; y += nThreads) {
            T* d = (T*)(dstData + (ptrdiff_t)(y - dy1) * dstRowBytes) + (ptrdiff_t)(rw[0] - dx1) * 4;
            const double Y = y + 0.5;
            for (int x = rw[0]; x < rw[2]; ++x, d += 4) {
                float acc[4] = {0, 0, 0, 0};
                if (!empty) {
                    const double X = x + 0.5;
                    for (int s = 0; s < nSamples; ++s) {
                        if (!perChannel) {
                            const Affine& m = mats[s];
                            float v[4];
                            sampleMix<T>(src, srcB, mixW, m.a * X + m.b * Y + m.c, m.d * X + m.e * Y + m.f, v);
                            acc[0] += v[0]; acc[1] += v[1]; acc[2] += v[2]; acc[3] += v[3];
                        } else {
                            float a = 0;
                            for (int c = 0; c < 3; ++c) {
                                const Affine& m = mats[s * nch + c];
                                float v[4];
                                sampleMix<T>(src, srcB, mixW, m.a * X + m.b * Y + m.c, m.d * X + m.e * Y + m.f, v);
                                acc[c] += v[c];
                                a = std::max(a, v[3]);
                            }
                            acc[3] += a;
                        }
                    }
                }
                d[0] = toPix<T>(acc[0] * invN); d[1] = toPix<T>(acc[1] * invN);
                d[2] = toPix<T>(acc[2] * invN); d[3] = toPix<T>(acc[3] * invN);
            }
        }
    };

    int nThreads = (int)std::max(1u, std::thread::hardware_concurrency());
    nThreads = std::min(nThreads, rows);
    if (nThreads <= 1) { work(0, 1); return; }
    std::vector<std::thread> pool;
    pool.reserve(nThreads - 1);
    for (int t = 1; t < nThreads; ++t) pool.emplace_back(work, t, nThreads);
    work(0, nThreads);
    for (auto& th : pool) th.join();
}

// --------------------------------------------------------------- OpenCL ----
// Same algorithm as the CPU path; host passes images as OpenCL Images
// (kOfxImageEffectPropOpenCLSupported, used by VEGAS).
static const char* kClSource = R"CLC(
__constant sampler_t smp = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

inline int wrapIdx(int i, int lo, int n, int mode, int* ok) {
    *ok = 1;
    if (i >= lo && i < lo + n) return i;
    if (mode == 0) { *ok = 0; return lo; }
    if (mode == 1) { int m = (i - lo) % n; if (m < 0) m += n; return lo + m; }
    if (mode == 2) { int p = 2 * n; int m = (i - lo) % p; if (m < 0) m += p; if (m >= n) m = p - 1 - m; return lo + m; }
    return i < lo ? lo : lo + n - 1;
}

inline float4 bilin(read_only image2d_t src, float fx, float fy, int4 crop, int2 wrap, int2 org) {
    float x = fx - 0.5f, y = fy - 0.5f;
    float flx = floor(x), fly = floor(y);
    int ix = (int)flx, iy = (int)fly;
    float tx = x - flx, ty = y - fly;
    int okx0, okx1, oky0, oky1;
    int x0 = wrapIdx(ix, crop.x, crop.z, wrap.x, &okx0), x1 = wrapIdx(ix + 1, crop.x, crop.z, wrap.x, &okx1);
    int y0 = wrapIdx(iy, crop.y, crop.w, wrap.y, &oky0), y1 = wrapIdx(iy + 1, crop.y, crop.w, wrap.y, &oky1);
    float4 r = (float4)(0.0f);
    if (oky0) {
        float w = 1.0f - ty;
        if (okx0) r += read_imagef(src, smp, (int2)(x0 - org.x, y0 - org.y)) * ((1.0f - tx) * w);
        if (okx1) r += read_imagef(src, smp, (int2)(x1 - org.x, y0 - org.y)) * (tx * w);
    }
    if (oky1) {
        if (okx0) r += read_imagef(src, smp, (int2)(x0 - org.x, y1 - org.y)) * ((1.0f - tx) * ty);
        if (okx1) r += read_imagef(src, smp, (int2)(x1 - org.x, y1 - org.y)) * (tx * ty);
    }
    return r;
}

inline float4 bilinOne(read_only image2d_t src, float fx, float fy, int4 crop, int2 wrap, int2 org) {
    if (crop.z <= 0 || crop.w <= 0) return (float4)(0.0f);
    return bilin(src, fx, fy, crop, wrap, org);
}

// w = 0 -> A, w = 1 -> B, in between: cross-fade
inline float4 bilinMix(read_only image2d_t a, read_only image2d_t b, float w, float fx, float fy,
                       int4 cropA, int4 cropB, int2 wrap, int2 orgA, int2 orgB) {
    if (w <= 0.0f) return bilinOne(a, fx, fy, cropA, wrap, orgA);
    if (w >= 1.0f) return bilinOne(b, fx, fy, cropB, wrap, orgB);
    return mix(bilinOne(a, fx, fy, cropA, wrap, orgA), bilinOne(b, fx, fy, cropB, wrap, orgB), w);
}

__kernel void shake(read_only image2d_t srcA, read_only image2d_t srcB, write_only image2d_t dst,
                    __global const float* mats, int nSamples, int perChannel, int4 cropA, int4 cropB, int2 wrap,
                    int2 orgA, int2 orgB, int2 dstOrg, int2 rwOrg, int2 rwSize, float w) {
    int gx = get_global_id(0), gy = get_global_id(1);
    if (gx >= rwSize.x || gy >= rwSize.y) return;
    int x = rwOrg.x + gx, y = rwOrg.y + gy;
    float X = x + 0.5f, Y = y + 0.5f;
    float4 acc = (float4)(0.0f);
    for (int s = 0; s < nSamples; ++s) {
        if (!perChannel) {
            __global const float* m = mats + s * 6;
            acc += bilinMix(srcA, srcB, w, m[0] * X + m[1] * Y + m[2], m[3] * X + m[4] * Y + m[5], cropA, cropB, wrap, orgA, orgB);
        } else {
            __global const float* m = mats + s * 18;
            float4 r = bilinMix(srcA, srcB, w, m[0] * X + m[1] * Y + m[2], m[3] * X + m[4] * Y + m[5], cropA, cropB, wrap, orgA, orgB);
            float4 g = bilinMix(srcA, srcB, w, m[6] * X + m[7] * Y + m[8], m[9] * X + m[10] * Y + m[11], cropA, cropB, wrap, orgA, orgB);
            float4 b = bilinMix(srcA, srcB, w, m[12] * X + m[13] * Y + m[14], m[15] * X + m[16] * Y + m[17], cropA, cropB, wrap, orgA, orgB);
            acc += (float4)(r.x, g.y, b.z, fmax(r.w, fmax(g.w, b.w)));
        }
    }
    acc /= (float)nSamples;
    write_imagef(dst, (int2)(x - dstOrg.x, y - dstOrg.y), acc);
}
)CLC";

static ClApi gCl;
static std::mutex gClMutex;
struct ClProgram { cl_context ctx; cl_program prog; cl_kernel kern; };
static std::vector<ClProgram> gClPrograms;

static void appendLog(const char* msg) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) return;
    FILE* f = std::fopen((std::string(tmp) + "\\B2KShake_host.txt").c_str(), "a");
    if (!f) return;
    std::fprintf(f, "%s\n", msg);
    std::fclose(f);
}

// Call with gClMutex held.
static cl_kernel clKernelFor(cl_command_queue q) {
    cl_context ctx = nullptr;
    cl_device_id dev = nullptr;
    if (gCl.GetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof ctx, &ctx, nullptr) != CL_SUCCESS) return nullptr;
    for (auto& p : gClPrograms) if (p.ctx == ctx) return p.kern;
    gCl.GetCommandQueueInfo(q, CL_QUEUE_DEVICE, sizeof dev, &dev, nullptr);
    cl_int err = 0;
    cl_program prog = gCl.CreateProgramWithSource(ctx, 1, &kClSource, nullptr, &err);
    if (err != CL_SUCCESS) return nullptr;
    if (gCl.BuildProgram(prog, 1, &dev, "-cl-fast-relaxed-math", nullptr, nullptr) != CL_SUCCESS) {
        char log[4096] = {0};
        gCl.GetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof log - 1, log, nullptr);
        appendLog("OpenCL build failed:");
        appendLog(log);
        return nullptr;
    }
    cl_kernel k = gCl.CreateKernel(prog, "shake", &err);
    if (err != CL_SUCCESS) return nullptr;
    gClPrograms.push_back({ctx, prog, k});
    return k;
}

static OfxStatus renderCL(cl_command_queue q, cl_mem imgA, cl_mem imgB, cl_mem dstImg, const int ba[4],
                          const int bb[4], const int db[4], const int rw[4], const int cropA[4], const int cropB[4],
                          int wrapX, int wrapY, float mixW, const std::vector<Affine>& mats, int nSamples,
                          bool perChannel) {
    if (rw[2] <= rw[0] || rw[3] <= rw[1]) return kOfxStatOK;
    if (!gCl.load()) { appendLog("OpenCL.dll not available"); return kOfxStatFailed; }
    std::vector<float> m;
    m.reserve(mats.size() * 6);
    for (const Affine& a : mats) { m.push_back((float)a.a); m.push_back((float)a.b); m.push_back((float)a.c);
                                   m.push_back((float)a.d); m.push_back((float)a.e); m.push_back((float)a.f); }
    std::lock_guard<std::mutex> lock(gClMutex);
    cl_kernel k = clKernelFor(q);
    if (!k) return kOfxStatFailed;
    cl_context ctx = nullptr;
    gCl.GetCommandQueueInfo(q, CL_QUEUE_CONTEXT, sizeof ctx, &ctx, nullptr);
    cl_int err = 0;
    cl_mem buf = gCl.CreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, m.size() * sizeof(float), m.data(), &err);
    if (err != CL_SUCCESS) return kOfxStatFailed;
    cl_int pc = perChannel ? 1 : 0, ns = nSamples;
    cl_int ca[4] = {cropA[0], cropA[1], cropA[2], cropA[3]}, cb[4] = {cropB[0], cropB[1], cropB[2], cropB[3]};
    cl_int wrap[2] = {wrapX, wrapY}, oa[2] = {ba[0], ba[1]}, ob[2] = {bb[0], bb[1]}, dorg[2] = {db[0], db[1]};
    cl_int rwo[2] = {rw[0], rw[1]}, rws[2] = {rw[2] - rw[0], rw[3] - rw[1]};
    cl_uint i = 0;
    gCl.SetKernelArg(k, i++, sizeof(cl_mem), &imgA);
    gCl.SetKernelArg(k, i++, sizeof(cl_mem), &imgB);
    gCl.SetKernelArg(k, i++, sizeof(cl_mem), &dstImg);
    gCl.SetKernelArg(k, i++, sizeof(cl_mem), &buf);
    gCl.SetKernelArg(k, i++, sizeof ns, &ns);
    gCl.SetKernelArg(k, i++, sizeof pc, &pc);
    gCl.SetKernelArg(k, i++, sizeof ca, ca);
    gCl.SetKernelArg(k, i++, sizeof cb, cb);
    gCl.SetKernelArg(k, i++, sizeof wrap, wrap);
    gCl.SetKernelArg(k, i++, sizeof oa, oa);
    gCl.SetKernelArg(k, i++, sizeof ob, ob);
    gCl.SetKernelArg(k, i++, sizeof dorg, dorg);
    gCl.SetKernelArg(k, i++, sizeof rwo, rwo);
    gCl.SetKernelArg(k, i++, sizeof rws, rws);
    gCl.SetKernelArg(k, i++, sizeof mixW, &mixW);
    size_t local[2] = {16, 16};
    size_t global[2] = {(size_t)(rws[0] + 15) / 16 * 16, (size_t)(rws[1] + 15) / 16 * 16};
    err = gCl.EnqueueNDRangeKernel(q, k, 2, nullptr, global, local, 0, nullptr, nullptr);
    gCl.ReleaseMemObject(buf);  // freed by the runtime once the kernel is done
    return err == CL_SUCCESS ? kOfxStatOK : kOfxStatFailed;
}

// --------------------------------------------------------------- render ----
struct ImgGuard {
    OfxPropertySetHandle h = nullptr;
    ~ImgGuard() { if (h) gEffect->clipReleaseImage(h); }
};

static double frameRate(OfxImageEffectHandle effect, OfxImageClipHandle src) {
    double fps = 0;
    OfxPropertySetHandle props;
    if (gEffect->getPropertySet(effect, &props) == kOfxStatOK) gProp->propGetDouble(props, kOfxImageEffectPropFrameRate, 0, &fps);
    if (fps <= 0 && gEffect->clipGetPropertySet(src, &props) == kOfxStatOK) gProp->propGetDouble(props, kOfxImageEffectPropFrameRate, 0, &fps);
    return fps > 0 ? fps : 30.0;
}

struct ImgInfo {
    void* ptr = nullptr;
    int b[4] = {0, 0, 0, 0};
    int row = 0;
    char* depth = nullptr;
    char* comp = nullptr;
    double par = 1.0;
};

static bool readImg(OfxPropertySetHandle h, bool cl, ImgInfo& o) {
    gProp->propGetPointer(h, cl ? kOfxImageEffectPropOpenCLImage : kOfxImagePropData, 0, &o.ptr);
    gProp->propGetIntN(h, kOfxImagePropBounds, 4, o.b);
    gProp->propGetDouble(h, kOfxImagePropPixelAspectRatio, 0, &o.par);
    if (o.par <= 0) o.par = 1.0;
    if (cl) return o.ptr != nullptr;
    gProp->propGetInt(h, kOfxImagePropRowBytes, 0, &o.row);
    gProp->propGetString(h, kOfxImageEffectPropPixelDepth, 0, &o.depth);
    gProp->propGetString(h, kOfxImageEffectPropComponents, 0, &o.comp);
    return o.ptr && o.depth;
}

static SrcImg makeSrc(const ImgInfo& im, const Params& p) {
    SrcImg s;
    const int* sb = im.b;
    const int sw = sb[2] - sb[0], sh = sb[3] - sb[1];
    s.data = (const char*)im.ptr;
    s.x1 = sb[0]; s.y1 = sb[1]; s.x2 = sb[2]; s.y2 = sb[3];
    s.rowBytes = im.row;
    int cl = (int)std::lround(std::clamp(p.crop[0], 0.0, 1.0) * sw);
    int cr = (int)std::lround(std::clamp(p.crop[1], 0.0, 1.0) * sw);
    int ct = (int)std::lround(std::clamp(p.crop[2], 0.0, 1.0) * sh);
    int cb = (int)std::lround(std::clamp(p.crop[3], 0.0, 1.0) * sh);
    s.cx1 = sb[0] + cl; s.cw = sw - cl - cr;
    s.cy1 = sb[1] + cb; s.ch = sh - cb - ct;  // OFX y axis points up: bottom = y1
    s.wrapX = p.wrapX; s.wrapY = p.wrapY;
    return s;
}

static inline double smoothstep(double e0, double e1, double x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ------------------------------------------------------ dissolve curves ----
// Shake envelope (yellow): two cubic Bezier segments (0,0) -> peak -> (1,0) with
// a horizontal ease handle at each end and a vertical handle at the peak.
// Dissolve (blue): linear ramp centred on Dissolve Center, 1/Dissolve Speed long.
enum Mode { kModeShake = 0, kModeDissolveTransition = 1, kModeDissolveFilter = 2 };

struct Curves {
    double peakX, peakY, easeIn, easeOut, peakEase, center, speed;
    double bendIn, bendOut, bendMix;  // -1..1: <0 exponential (slow start), >0 logarithmic (fast start)
};

// bend -> exponent of a power warp on a 0..1 value (endpoints stay fixed)
static inline double bendWarp(double n, double bend) {
    if (bend == 0 || n <= 0 || n >= 1) return n;
    return std::pow(n, std::pow(4.0, -bend));
}

static void readCurves(OfxParamSetHandle ps, OfxTime t, Curves& c) {
    c.peakX = std::clamp(getD(ps, "shakePeakPos", t), 0.001, 0.999);
    c.peakY = std::max(0.0, getD(ps, "shakePeakAmp", t));
    c.easeIn = std::clamp(getD(ps, "easeIn", t), 0.0, 1.0);
    c.easeOut = std::clamp(getD(ps, "easeOut", t), 0.0, 1.0);
    c.peakEase = std::clamp(getD(ps, "peakEase", t), 0.0, 1.0);
    c.center = getD(ps, "dissolveCenter", t);
    c.speed = std::max(1.0, getD(ps, "dissolveSpeed", t));
    c.bendIn = std::clamp(getD(ps, "bendIn", t), -1.0, 1.0);
    c.bendOut = std::clamp(getD(ps, "bendOut", t), -1.0, 1.0);
    c.bendMix = std::clamp(getD(ps, "dissolveBend", t), -1.0, 1.0);
}

static inline double bez(double a, double b, double c, double d, double s) {
    double u = 1.0 - s;
    return u * u * u * a + 3 * u * u * s * b + 3 * u * s * s * c + s * s * s * d;
}

// y of a cubic Bezier at given x (control x values are monotonic)
static double bezAtX(const double px[4], const double py[4], double x) {
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        double m = 0.5 * (lo + hi);
        if (bez(px[0], px[1], px[2], px[3], m) < x) lo = m; else hi = m;
    }
    return bez(py[0], py[1], py[2], py[3], 0.5 * (lo + hi));
}

// control points of the two envelope segments
static void envSegments(const Curves& c, double lx[4], double ly[4], double rx[4], double ry[4]) {
    const double hy = c.peakY * (1.0 - c.peakEase);
    lx[0] = 0; lx[1] = c.easeIn * c.peakX; lx[2] = c.peakX; lx[3] = c.peakX;
    ly[0] = 0; ly[1] = 0; ly[2] = hy; ly[3] = c.peakY;
    rx[0] = c.peakX; rx[1] = c.peakX; rx[2] = 1.0 - c.easeOut * (1.0 - c.peakX); rx[3] = 1;
    ry[0] = c.peakY; ry[1] = hy; ry[2] = 0; ry[3] = 0;
}

static double shakeEnvelope(const Curves& c, double t) {
    if (t <= 0 || t >= 1) return 0;
    double lx[4], ly[4], rx[4], ry[4];
    envSegments(c, lx, ly, rx, ry);
    const bool left = t <= c.peakX;
    double y = left ? bezAtX(lx, ly, t) : bezAtX(rx, ry, t);
    if (c.peakY <= 0) return y;
    return c.peakY * bendWarp(std::clamp(y / c.peakY, 0.0, 1.0), left ? c.bendIn : c.bendOut);
}

static double dissolveMix(const Curves& c, double t) {
    return bendWarp(std::clamp((t - c.center) * c.speed + 0.5, 0.0, 1.0), c.bendMix);
}

// progress of a dissolve plugin at time t (host transition value or Dissolve Amount)
static double dissolveProgress(OfxParamSetHandle ps, OfxTime t, int mode) {
    return std::clamp(getD(ps, mode == kModeDissolveTransition ? kOfxImageEffectTransitionParamName : "dissolveAmount", t), 0.0, 1.0);
}

static OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs, int mode) {
    const bool transition = mode == kModeDissolveTransition;
    OfxTime time = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    int rw[4];
    gProp->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, rw);

    const char* nameA = transition ? kOfxImageEffectTransitionSourceFromClipName : kOfxImageEffectSimpleSourceClipName;
    OfxImageClipHandle clipA = nullptr, clipB = nullptr, dstClip = nullptr;
    if (gEffect->clipGetHandle(effect, nameA, &clipA, nullptr) != kOfxStatOK) return kOfxStatFailed;
    if (transition && gEffect->clipGetHandle(effect, kOfxImageEffectTransitionSourceToClipName, &clipB, nullptr) != kOfxStatOK) return kOfxStatFailed;
    if (gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &dstClip, nullptr) != kOfxStatOK) return kOfxStatFailed;

    int clEnabled = 0;
    void* clQueue = nullptr;
    gProp->propGetInt(inArgs, kOfxImageEffectPropOpenCLEnabled, 0, &clEnabled);
    if (clEnabled) gProp->propGetPointer(inArgs, kOfxImageEffectPropOpenCLCommandQueue, 0, &clQueue);
    const bool useCL = clEnabled && clQueue;
    {
        static std::atomic<int> logged{-1};
        int prev = logged.exchange(useCL ? 1 : 0);
        if (prev != (useCL ? 1 : 0)) appendLog(useCL ? "render: OpenCL (GPU)" : "render: CPU");
    }

    Params p;
    readParams(effect, time, p);
    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    double mixW = 0.0;
    if (mode != kModeShake) {
        Curves cv;
        readCurves(ps, time, cv);
        const double prog = dissolveProgress(ps, time, mode);
        mixW = dissolveMix(cv, prog);
        p.amp *= shakeEnvelope(cv, prog);
    }
    // video-effect dissolve fades the clip out to transparent (track below shows through)
    const bool fadeOut = mode == kModeDissolveFilter;

    ImgGuard dstG, aG, bG;
    if (gEffect->clipGetImage(dstClip, time, nullptr, &dstG.h) != kOfxStatOK) return kOfxStatFailed;
    // only fetch the clips that are actually visible
    const bool needA = !transition || mixW < 1.0, needB = transition && mixW > 0.0;
    if (needA && gEffect->clipGetImage(clipA, time, nullptr, &aG.h) != kOfxStatOK) return kOfxStatFailed;
    if (needB && gEffect->clipGetImage(clipB, time, nullptr, &bG.h) != kOfxStatOK) return kOfxStatFailed;

    ImgInfo dI, aI, bI;
    if (!readImg(dstG.h, useCL, dI)) { if (useCL) appendLog("OpenCL enabled but no OpenCL image on output"); return kOfxStatFailed; }
    if (needA && !readImg(aG.h, useCL, aI)) return kOfxStatFailed;
    if (needB && !readImg(bG.h, useCL, bI)) return kOfxStatFailed;
    if (!needA) aI = bI;
    if (!needB) bI = aI;
    if (!useCL) {
        for (const ImgInfo* s : {&aI, &bI}) {
            if (std::strcmp(s->depth, dI.depth) != 0) return kOfxStatErrImageFormat;
            if (!s->comp || std::strcmp(s->comp, kOfxImageComponentRGBA) != 0) return kOfxStatErrImageFormat;
        }
    }

    // clamp render window to dst bounds
    const int* db = dI.b;
    rw[0] = std::max(rw[0], db[0]); rw[1] = std::max(rw[1], db[1]);
    rw[2] = std::min(rw[2], db[2]); rw[3] = std::min(rw[3], db[3]);

    double fps = frameRate(effect, clipA);
    SrcImg srcA = makeSrc(aI, p), srcB = makeSrc(bI, p);
    if (fadeOut) { srcB.cw = 0; srcB.ch = 0; }  // "B" = transparent

    // geometry reference: the output frame
    const int* sb = aI.b;
    const double par = aI.par;
    const double cx = 0.5 * (db[0] + db[2]), cy = 0.5 * (db[1] + db[3]);
    const double refW = (sb[2] - sb[0]) * par;  // X/Y amplitudes are fractions of the frame width

    const bool perChannel = p.rgbRand != 0 || p.chAmp[0] != p.chAmp[1] || p.chAmp[1] != p.chAmp[2] ||
                            p.chPhase[0] != p.chPhase[1] || p.chPhase[1] != p.chPhase[2] || p.chAmp[0] != 1.0 ||
                            p.chPhase[0] != 0.0;
    const int nch = perChannel ? 3 : 1;
    auto matAt = [&](double frame, int c) {
        double s4[4];
        shakeAt(p, frame / fps, perChannel ? c : -1, s4);
        return makeAffine(s4, cx, cy, refW, par, p.zdist);
    };

    // motion blur: choose sample count from how far the frame corners travel during the shutter
    int nSamples = 1;
    const double L = p.motionBlur ? std::max(0.0, p.mbLen) : 0.0;
    if (L > 0) {
        const double corners[4][2] = {{(double)db[0], (double)db[1]}, {(double)db[2], (double)db[1]},
                                      {(double)db[0], (double)db[3]}, {(double)db[2], (double)db[3]}};
        double maxLen = 0;
        const int probes = 8;
        for (int c = 0; c < nch; ++c) {
            double len[4] = {0, 0, 0, 0}, px[4], py[4];
            for (int k = 0; k <= probes; ++k) {
                Affine m = matAt(time - 0.5 * L + L * k / probes, c);
                for (int q = 0; q < 4; ++q) {
                    double X = m.a * corners[q][0] + m.b * corners[q][1] + m.c;
                    double Y = m.d * corners[q][0] + m.e * corners[q][1] + m.f;
                    if (k) len[q] += std::hypot(X - px[q], Y - py[q]);
                    px[q] = X; py[q] = Y;
                }
            }
            for (double l : len) maxLen = std::max(maxLen, l);
        }
        // one sample per ~2 px of travel is visually smooth with bilinear filtering
        nSamples = std::clamp((int)std::ceil(maxLen * 0.5), 1, 48);
    }

    std::vector<Affine> mats((size_t)nSamples * nch);
    for (int s = 0; s < nSamples; ++s) {
        double frame = nSamples == 1 ? time : time - 0.5 * L + L * (s + 0.5) / nSamples;
        for (int c = 0; c < nch; ++c) mats[(size_t)s * nch + c] = matAt(frame, c);
    }

    const float w = (float)mixW;
    if (useCL) {
        const int cropA[4] = {srcA.cx1, srcA.cy1, srcA.cw, srcA.ch};
        const int cropB[4] = {srcB.cx1, srcB.cy1, srcB.cw, srcB.ch};
        return renderCL((cl_command_queue)clQueue, (cl_mem)aI.ptr, (cl_mem)bI.ptr, (cl_mem)dI.ptr, aI.b, bI.b, db, rw,
                        cropA, cropB, p.wrapX, p.wrapY, w, mats, nSamples, perChannel);
    }
    char* dstData = (char*)dI.ptr;
    if (std::strcmp(dI.depth, kOfxBitDepthFloat) == 0)
        renderT<float>(srcA, srcB, w, dstData, db[0], db[1], dI.row, rw, mats, nSamples, perChannel);
    else if (std::strcmp(dI.depth, kOfxBitDepthShort) == 0)
        renderT<unsigned short>(srcA, srcB, w, dstData, db[0], db[1], dI.row, rw, mats, nSamples, perChannel);
    else if (std::strcmp(dI.depth, kOfxBitDepthByte) == 0)
        renderT<unsigned char>(srcA, srcB, w, dstData, db[0], db[1], dI.row, rw, mats, nSamples, perChannel);
    else
        return kOfxStatErrImageFormat;
    return kOfxStatOK;
}

// ------------------------------------------------------------- describe ----
static OfxPropertySetHandle defGroup(OfxParamSetHandle ps, const char* name, const char* label, bool open = true) {
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeGroup, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetInt(p, kOfxParamPropGroupOpen, 0, open ? 1 : 0);
    return p;
}

static OfxPropertySetHandle defDouble(OfxParamSetHandle ps, const char* name, const char* label, double def, double mn,
                                      double mx, double dmn, double dmx, int digits, const char* parent, const char* hint) {
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeDouble, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetString(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, def);
    gProp->propSetDouble(p, kOfxParamPropMin, 0, mn);
    gProp->propSetDouble(p, kOfxParamPropMax, 0, mx);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, dmn);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, dmx);
    gProp->propSetDouble(p, kOfxParamPropIncrement, 0, digits >= 3 ? 0.001 : 0.01);
    gProp->propSetInt(p, kOfxParamPropDigits, 0, digits);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 1);
    if (parent) gProp->propSetString(p, kOfxParamPropParent, 0, parent);
    if (hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    return p;
}

static void defChoice(OfxParamSetHandle ps, const char* name, const char* label, const char* const* opts, int n, int def,
                      const char* hint) {
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeChoice, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    for (int i = 0; i < n; ++i) gProp->propSetString(p, kOfxParamPropChoiceOption, i, opts[i]);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    if (hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
}

static void defBool(OfxParamSetHandle ps, const char* name, const char* label, int def, const char* hint) {
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeBoolean, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    if (hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
}

static OfxStatus overlayMain(const char* action, const void* handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs);

static OfxStatus describe(OfxImageEffectHandle effect, int mode) {
    const bool transition = mode == kModeDissolveTransition;
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, mode == kModeShake ? "B2K Shake" : "B2K Dissolve Shake");
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "B2K");
    gProp->propSetString(props, kOfxPropPluginDescription, 0,
                         mode == kModeShake ? "Camera shake with random + wave motion on X, Y, Z (zoom) and Tilt, motion blur, edge wrapping and RGB split."
                         : transition ? "Transition: both clips shake, the shake swells towards the middle and the clips dissolve."
                                      : "Animate Dissolve Amount 0 -> 1: the clip shakes up and dissolves to transparent.");
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0,
                         transition ? kOfxImageEffectContextTransition : kOfxImageEffectContextFilter);
    if (mode != kModeShake) {
        // curve editor drawn over the preview (V2 = draw suite, V1 = OpenGL)
        gProp->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0, (void*)&overlayMain);
        gProp->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV2, 0, (void*)&overlayMain);
    }
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gProp->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
    // GPU: OpenCL Images (what VEGAS offers). Hosts may type this as string or int.
    if (gProp->propSetString(props, kOfxImageEffectPropOpenCLSupported, 0, "true") != kOfxStatOK)
        gProp->propSetInt(props, kOfxImageEffectPropOpenCLSupported, 0, 1);
    return kOfxStatOK;
}

static void defClip(OfxImageEffectHandle effect, const char* name) {
    OfxPropertySetHandle cp;
    gEffect->clipDefine(effect, name, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetInt(cp, kOfxImageEffectPropSupportsTiles, 0, 0);
}

static void defShakeParams(OfxParamSetHandle ps, int mode) {
    const bool dissolve = mode != kModeShake;
    static const char* const styles[] = {"Normal", "Smooth", "Jerky"};
    // the dissolve variant keeps its top level short (like S_DissolveShake); extras go to "More"
    const char* more = "moreGroup";
    defChoice(ps, "style", "Style", styles, 3, 0, "Normal: organic hand-held noise. Smooth: soft floating motion. Jerky: sharp direction changes.");
    defDouble(ps, "amplitude", "Amplitude", dissolve ? 3.0 : 1.0, 0, 100, 0, dissolve ? 6 : 4, 3, nullptr, "Scales all shake amounts.");
    defDouble(ps, "frequency", "Frequency", dissolve ? 10.0 : 8.0, 0, 1000, 0, 30, 2, nullptr, "Shake cycles per second.");
    if (!dissolve) {
        defDouble(ps, "phase", "Phase", 0.0, -1000, 1000, -2, 2, 2, nullptr, "Offsets the shake in time (in cycles).");
        defDouble(ps, "zDist", "Z Dist", 1.0, 0.01, 100, 0.1, 4, 3, nullptr, "Camera distance (overall zoom). 1 = original size, smaller zooms in, larger zooms out.");
    }
    defBool(ps, "motionBlur", "Motion Blur", dissolve ? 1 : 0, "Blur along the shake motion.");
    defDouble(ps, "moBlurLength", "Mo Blur Length", dissolve ? 0.5 : 1.0, 0, 10, 0, 4, 3, nullptr, "Shutter length in frames.");
    defDouble(ps, "seed", "Seed", 0.0, 0, 1000, 0, 1, 3, nullptr, "Random pattern. Each 0.001 step is a new pattern; in-between values blend.");
    static const char* const wraps[] = {"Black", "Repeat", "Reflect", "Extend"};
    defChoice(ps, "wrapX", "Wrap X", wraps, 4, 2, "How to fill pixels that move in from the left/right edge.");
    defChoice(ps, "wrapY", "Wrap Y", wraps, 4, 2, "How to fill pixels that move in from the top/bottom edge.");
    if (dissolve) {
        defGroup(ps, more, "More", false);
        defDouble(ps, "phase", "Phase", 0.0, -1000, 1000, -2, 2, 2, more, "Offsets the shake in time (in cycles).");
        defDouble(ps, "zDist", "Z Dist", 1.0, 0.01, 100, 0.1, 4, 3, more, "Camera distance (overall zoom). 1 = original size, smaller zooms in, larger zooms out.");
    }

    // X: 0.1, Y: 0.05, Z/Tilt: 0 by default
    const double defRand[4] = {0.1, 0.05, 0.0, 0.0};
    char name[64], label[64], grp[64];
    for (int k = 0; k < 4; ++k) {
        const bool tilt = k == 3;
        snprintf(grp, sizeof grp, "%sShakeGroup", kAxisPrefix[k]);
        snprintf(label, sizeof label, "%s Shake", kAxisLabel[k]);
        defGroup(ps, grp, label, false);
        const char* unit = tilt ? "Degrees." : (k == 2 ? "Units of Z Dist (zoom)." : "Fraction of frame width.");
        snprintf(name, sizeof name, "%sRandAmp", kAxisPrefix[k]); snprintf(label, sizeof label, "%s Rand Amp", kAxisLabel[k]);
        defDouble(ps, name, label, defRand[k], -1000, 1000, 0, tilt ? 30 : 1, tilt ? 2 : 3, grp, unit);
        snprintf(name, sizeof name, "%sRandFreq", kAxisPrefix[k]); snprintf(label, sizeof label, "%s Rand Freq", kAxisLabel[k]);
        defDouble(ps, name, label, 1.0, 0, 100, 0, 4, 3, grp, "Multiplies the main Frequency.");
        snprintf(name, sizeof name, "%sWaveAmp", kAxisPrefix[k]); snprintf(label, sizeof label, "%s Wave Amp", kAxisLabel[k]);
        defDouble(ps, name, label, 0.0, -1000, 1000, 0, tilt ? 30 : 1, tilt ? 2 : 3, grp, unit);
        snprintf(name, sizeof name, "%sWaveFreq", kAxisPrefix[k]); snprintf(label, sizeof label, "%s Wave Freq", kAxisLabel[k]);
        defDouble(ps, name, label, 0.5, 0, 100, 0, 4, 3, grp, "Sine frequency, multiplies the main Frequency.");
        snprintf(name, sizeof name, "%sPhase", kAxisPrefix[k]); snprintf(label, sizeof label, "%s Phase", kAxisLabel[k]);
        defDouble(ps, name, label, 0.0, -1000, 1000, -2, 2, 2, grp, "Time offset for this axis (in cycles).");
    }

    defGroup(ps, "channelsGroup", "Channels", false);
    defDouble(ps, "redAmp", "Red Amplitude", 1.0, -100, 100, 0, 4, 3, "channelsGroup", nullptr);
    defDouble(ps, "greenAmp", "Green Amplitude", 1.0, -100, 100, 0, 4, 3, "channelsGroup", nullptr);
    defDouble(ps, "blueAmp", "Blue Amplitude", 1.0, -100, 100, 0, 4, 3, "channelsGroup", nullptr);
    defDouble(ps, "redPhase", "Red Phase", 0.0, -1000, 1000, -2, 2, 3, "channelsGroup", nullptr);
    defDouble(ps, "greenPhase", "Green Phase", 0.0, -1000, 1000, -2, 2, 3, "channelsGroup", nullptr);
    defDouble(ps, "bluePhase", "Blue Phase", 0.0, -1000, 1000, -2, 2, 3, "channelsGroup", nullptr);
    defDouble(ps, "rgbRandomness", "RGB Randomness", 0.0, 0, 100, 0, 1, 3, "channelsGroup", "Extra independent shake per color channel.");
    defDouble(ps, "rgbFrequency", "RGB Frequency", 2.0, 0, 100, 0, 8, 2, "channelsGroup", "Frequency of the RGB randomness, multiplies the main Frequency.");

    defGroup(ps, "cropGroup", "Crop Input", false);
    defDouble(ps, "cropLeft", "Crop Left", 0.0, 0, 1, 0, 0.5, 3, "cropGroup", nullptr);
    defDouble(ps, "cropRight", "Crop Right", 0.0, 0, 1, 0, 0.5, 3, "cropGroup", nullptr);
    defDouble(ps, "cropTop", "Crop Top", 0.0, 0, 1, 0, 0.5, 3, "cropGroup", nullptr);
    defDouble(ps, "cropBottom", "Crop Bottom", 0.0, 0, 1, 0, 0.5, 3, "cropGroup", nullptr);
}

static void defCurveParams(OfxParamSetHandle ps, bool transition) {
    if (transition) {
        // mandatory transition parameter, driven by the host (0 = from clip, 1 = to clip)
        OfxPropertySetHandle p;
        gParam->paramDefine(ps, kOfxParamTypeDouble, kOfxImageEffectTransitionParamName, &p);
        gProp->propSetString(p, kOfxPropLabel, 0, "Transition");
        gProp->propSetDouble(p, kOfxParamPropDefault, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropMin, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropMax, 0, 1.0);
        gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, 1.0);
    } else {
        defDouble(ps, "dissolveAmount", "Dissolve Amount", 0.0, 0, 1, 0, 1, 3, nullptr,
                  "Animate 0 -> 1: the clip shakes up and dissolves to transparent (1 -> 0 to dissolve in).");
    }
    OfxPropertySetHandle p = defDouble(ps, "dissolveSpeed", "Dissolve Speed", 3.0, 1, 20, 1, 20, 3, nullptr,
                                       "How fast the dissolve happens. 3 = dissolve during the middle third.");
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}

static void defCurveEditorParams(OfxParamSetHandle ps) {
    const char* g = "curveGroup";
    defGroup(ps, g, "Curve Editor", false);
    OfxPropertySetHandle p;
    gParam->paramDefine(ps, kOfxParamTypeBoolean, "showCurves", &p);
    gProp->propSetString(p, kOfxPropLabel, 0, "Show In Preview");
    gProp->propSetInt(p, kOfxParamPropDefault, 0, 1);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    gProp->propSetString(p, kOfxParamPropParent, 0, g);
    gProp->propSetString(p, kOfxParamPropHint, 0, "Draw the shake (yellow) and dissolve (blue) curves over the preview; drag the dots to edit.");
    struct D { const char* n; const char* l; double def, mn, mx; const char* hint; };
    const D ds[] = {
        {"dissolveCenter", "Dissolve Center", 0.5, 0, 1, "Where the dissolve is 50/50 (blue line)."},
        {"shakePeakPos", "Shake Peak Pos", 0.5, 0.001, 0.999, "Where the shake is strongest (yellow peak)."},
        {"shakePeakAmp", "Shake Peak Amp", 1.0, 0, 10, "Shake strength at the peak (multiplies Amplitude)."},
        {"easeIn", "Ease In", 0.5, 0, 1, "Handle at the start: higher = shake starts more gently."},
        {"easeOut", "Ease Out", 0.5, 0, 1, "Handle at the end: higher = shake ends more gently."},
        {"peakEase", "Peak Ease", 0.2, 0, 1, "Handle at the peak: higher = rounder peak, 0 = sharp spike."},
        {"bendIn", "Bend In", 0.0, -1, 1, "Bends the rising shake: < 0 exponential (slow start), > 0 fast start. Drag the yellow curve up/down."},
        {"bendOut", "Bend Out", 0.0, -1, 1, "Bends the falling shake: < 0 drops fast, > 0 lingers. Drag the yellow curve up/down."},
        {"dissolveBend", "Dissolve Bend", 0.0, -1, 1, "Bends the dissolve: < 0 exponential, > 0 logarithmic. Drag the blue line up/down."},
    };
    for (const D& d : ds) {
        p = defDouble(ps, d.n, d.l, d.def, d.mn, d.mx, d.mn, d.mx, 3, g, d.hint);
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    }
}

static OfxStatus describeInContext(OfxImageEffectHandle effect, int mode) {
    const bool transition = mode == kModeDissolveTransition;
    if (transition) {
        defClip(effect, kOfxImageEffectTransitionSourceFromClipName);
        defClip(effect, kOfxImageEffectTransitionSourceToClipName);
    } else {
        defClip(effect, kOfxImageEffectSimpleSourceClipName);
    }
    defClip(effect, kOfxImageEffectOutputClipName);

    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    if (mode != kModeShake) defCurveParams(ps, transition);
    defShakeParams(ps, mode);
    if (mode != kModeShake) defCurveEditorParams(ps);
    return kOfxStatOK;
}

// ------------------------------------------------------ misc. actions ----
static OfxStatus getRoI(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs, int mode) {
    const bool transition = mode == kModeDissolveTransition;
    OfxTime time = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    // the shake can pull pixels from anywhere in the source
    const char* names[2] = {transition ? kOfxImageEffectTransitionSourceFromClipName : kOfxImageEffectSimpleSourceClipName,
                            transition ? kOfxImageEffectTransitionSourceToClipName : nullptr};
    for (const char* n : names) {
        if (!n) continue;
        OfxImageClipHandle clip;
        OfxRectD rod;
        if (gEffect->clipGetHandle(effect, n, &clip, nullptr) != kOfxStatOK) continue;
        if (gEffect->clipGetRegionOfDefinition(clip, time, &rod) != kOfxStatOK) continue;
        double r[4] = {rod.x1, rod.y1, rod.x2, rod.y2};
        gProp->propSetDoubleN(outArgs, (std::string("OfxImageClipPropRoI_") + n).c_str(), 4, r);
    }
    return kOfxStatOK;
}

static OfxStatus isIdentity(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs, int mode) {
    OfxTime time = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    if (getD(ps, "cropLeft", time) != 0 || getD(ps, "cropRight", time) != 0 || getD(ps, "cropTop", time) != 0 ||
        getD(ps, "cropBottom", time) != 0 || getD(ps, "zDist", time) != 1.0)
        return kOfxStatReplyDefault;
    double amp = getD(ps, "amplitude", time);
    const char* clip = kOfxImageEffectSimpleSourceClipName;
    if (mode != kModeShake) {
        Curves c;
        readCurves(ps, time, c);
        const double prog = dissolveProgress(ps, time, mode);
        const double mixW = dissolveMix(c, prog);
        amp *= shakeEnvelope(c, prog);
        if (mode == kModeDissolveTransition) {
            if (mixW <= 0.0) clip = kOfxImageEffectTransitionSourceFromClipName;
            else if (mixW >= 1.0) clip = kOfxImageEffectTransitionSourceToClipName;
            else return kOfxStatReplyDefault;
        } else if (mixW > 0.0) {
            return kOfxStatReplyDefault;  // fading out
        }
    }
    if (amp != 0) return kOfxStatReplyDefault;
    gProp->propSetString(outArgs, kOfxPropName, 0, clip);
    return kOfxStatOK;
}

// ------------------------------------------------- curve editor overlay ----
static const OfxInteractSuiteV1* gInteract = nullptr;
static const OfxDrawSuiteV1* gDraw = nullptr;

struct Painter {
    OfxDrawContextHandle dc = nullptr;
    void color(float r, float g, float b, float a) {
        if (dc) { OfxRGBAColourF c = {r, g, b, a}; gDraw->setColour(dc, &c); }
        else glColor4f(r, g, b, a);
    }
    void width(float w) { if (dc) gDraw->setLineWidth(dc, w); else glLineWidth(w); }
    void prim(int dsPrim, unsigned glPrim, const std::vector<OfxPointD>& pts) {
        if (pts.empty()) return;
        if (dc) { gDraw->draw(dc, (OfxDrawPrimitive)dsPrim, pts.data(), (int)pts.size()); return; }
        glBegin(glPrim);
        for (const OfxPointD& p : pts) glVertex2d(p.x, p.y);
        glEnd();
    }
    void strip(const std::vector<OfxPointD>& pts) { prim(kOfxDrawPrimitiveLineStrip, GL_LINE_STRIP, pts); }
    void lines(const std::vector<OfxPointD>& pts) { prim(kOfxDrawPrimitiveLines, GL_LINES, pts); }
    void poly(const std::vector<OfxPointD>& pts) { prim(kOfxDrawPrimitivePolygon, GL_POLYGON, pts); }
    void dot(OfxPointD c, double rx, double ry) {
        poly({{c.x - rx, c.y - ry}, {c.x + rx, c.y - ry}, {c.x + rx, c.y + ry}, {c.x - rx, c.y + ry}});
    }
    void text(const char* s, OfxPointD p) {
        if (dc) gDraw->drawText(dc, s, &p, kOfxDrawTextAlignmentLeft);
    }
};

// Maps curve space (t = progress 0..1, v = value) to canonical image coordinates.
struct CurveView {
    double x0, y0, x1, y1, vmax;
    OfxPointD env(double t, double v) const { return {x0 + t * (x1 - x0), y0 + v / vmax * (y1 - y0)}; }
    OfxPointD mixp(double t, double m) const { return {x0 + t * (x1 - x0), y0 + m * (y1 - y0)}; }
    double tOf(double x) const { return (x - x0) / (x1 - x0); }
    double vOf(double y) const { return (y - y0) / (y1 - y0) * vmax; }
    double mOf(double y) const { return (y - y0) / (y1 - y0); }
};

static CurveView makeView(OfxImageEffectHandle effect, const Curves& c, const double pxs[2]) {
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    double off[2] = {0, 0}, size[2] = {1920, 1080};
    gProp->propGetDoubleN(props, kOfxImageEffectPropProjectOffset, 2, off);
    gProp->propGetDoubleN(props, kOfxImageEffectPropProjectSize, 2, size);
    // VEGAS' overlay space is normalised (frame = 0..1), recognisable by a tiny pixel scale.
    // In canonical space one screen pixel is roughly 0.2 .. 10 units.
    if (pxs[0] > 0 && pxs[0] < 0.05 && pxs[1] > 0 && pxs[1] < 0.05) {
        off[0] = off[1] = 0;
        size[0] = size[1] = 1;
    }
    CurveView v;
    // fill the frame, with just enough margin that the handles at the edges stay visible
    v.x0 = off[0] + 0.01 * size[0]; v.x1 = off[0] + 0.99 * size[0];
    v.y0 = off[1] + 0.02 * size[1]; v.y1 = off[1] + 0.98 * size[1];
    v.vmax = std::max(1.0, c.peakY);
    return v;
}

enum Handle { kHPeak = 0, kHPeakEase, kHEaseIn, kHEaseOut, kHMixLo, kHMixHi, kHCount,
              // grabbing the curve itself (not a dot) bends it up/down
              kHBendIn = kHCount, kHBendOut, kHBendMix };

static void handlePositions(const Curves& c, const CurveView& v, OfxPointD out[kHCount]) {
    out[kHPeak] = v.env(c.peakX, c.peakY);
    out[kHPeakEase] = v.env(c.peakX, c.peakY * (1.0 - c.peakEase));
    out[kHEaseIn] = v.env(c.easeIn * c.peakX, 0);
    out[kHEaseOut] = v.env(1.0 - c.easeOut * (1.0 - c.peakX), 0);
    const double tLo = c.center - 0.25 / c.speed, tHi = c.center + 0.25 / c.speed;
    out[kHMixLo] = v.mixp(tLo, dissolveMix(c, tLo));  // sit on the (possibly bent) blue line
    out[kHMixHi] = v.mixp(tHi, dissolveMix(c, tHi));
}

struct DragState { int handle = -1; int hover = -1; double vmax = 1; double startY = 0, startBend = 0; };

// handle under the pen (within 12 screen pixels), else a curve segment (within 10), or -1
static int pickHandle(const Curves& c, const CurveView& v, const double pen[2], const double pxs[2]) {
    OfxPointD h[kHCount];
    handlePositions(c, v, h);
    int best = -1;
    double bestD = 12.0;
    for (int k = 0; k < kHCount; ++k) {
        double dx = (pen[0] - h[k].x) / pxs[0], dy = (pen[1] - h[k].y) / pxs[1];
        double dist = std::sqrt(dx * dx + dy * dy);
        if (dist < bestD) { bestD = dist; best = k; }
    }
    if (best >= 0) return best;
    const double t = v.tOf(pen[0]);
    if (t <= 0.01 || t >= 0.99) return -1;
    // yellow curve (skip the tip, where the peak dots are)
    if (std::fabs(t - c.peakX) > 0.01 && std::fabs(pen[1] - v.env(t, shakeEnvelope(c, t)).y) / pxs[1] < 10)
        return t < c.peakX ? kHBendIn : kHBendOut;
    // blue line, only on its sloped part
    const double lo = c.center - 0.5 / c.speed, hi = c.center + 0.5 / c.speed;
    if (t > lo && t < hi && std::fabs(pen[1] - v.mixp(t, dissolveMix(c, t)).y) / pxs[1] < 10) return kHBendMix;
    return -1;
}

static const char* bendParam(int handle) {
    return handle == kHBendIn ? "bendIn" : handle == kHBendOut ? "bendOut" : "dissolveBend";
}
static std::mutex gDragMutex;
static std::vector<std::pair<OfxInteractHandle, DragState>> gDrags;

static DragState& dragFor(OfxInteractHandle h) {
    for (auto& d : gDrags) if (d.first == h) return d.second;
    gDrags.push_back({h, DragState{}});
    return gDrags.back().second;
}

static int modeOf(OfxParamSetHandle ps) {
    OfxParamHandle h;
    return gParam->paramGetHandle(ps, "dissolveAmount", &h, nullptr) == kOfxStatOK ? kModeDissolveFilter : kModeDissolveTransition;
}

// Sets a value from the overlay. Keeps it inside the param's slider (display) range, rounds it to
// the shown digits and skips no-op updates: VEGAS' parameter UI (WebView2) crashed when the
// overlay pushed values beyond the slider range at mouse-move rate.
static void setParam(OfxParamSetHandle ps, const char* name, double v) {
    if (!std::isfinite(v)) return;
    OfxParamHandle h;
    OfxPropertySetHandle props = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, &props) != kOfxStatOK) return;
    double lo = -1e9, hi = 1e9, dlo = -1e9, dhi = 1e9;
    if (props) {
        gProp->propGetDouble(props, kOfxParamPropMin, 0, &lo);
        gProp->propGetDouble(props, kOfxParamPropMax, 0, &hi);
        gProp->propGetDouble(props, kOfxParamPropDisplayMin, 0, &dlo);
        gProp->propGetDouble(props, kOfxParamPropDisplayMax, 0, &dhi);
    }
    lo = std::max(lo, dlo);
    hi = std::min(hi, dhi);
    if (lo > hi) std::swap(lo, hi);
    v = std::clamp(v, lo, hi);
    v = std::round(v * 1000.0) / 1000.0;
    double cur = 0;
    if (gParam->paramGetValue(h, &cur) == kOfxStatOK && std::fabs(cur - v) < 1e-9) return;
    gParam->paramSetValue(h, v);
}

static OfxStatus overlayDraw(OfxInteractHandle ih, OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    int active = -1;
    {
        std::lock_guard<std::mutex> lock(gDragMutex);
        const DragState& d = dragFor(ih);
        active = d.handle >= 0 ? d.handle : d.hover;
    }
    OfxTime time = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    if (!getI(ps, "showCurves", time)) return kOfxStatOK;
    Curves c;
    readCurves(ps, time, c);
    double pxs[2] = {1, 1};
    gProp->propGetDoubleN(inArgs, kOfxInteractPropPixelScale, 2, pxs);
    const CurveView v = makeView(effect, c, pxs);

    Painter pt;
    void* dc = nullptr;
    if (gDraw) gProp->propGetPointer(inArgs, kOfxInteractPropDrawContext, 0, &dc);
    pt.dc = (OfxDrawContextHandle)dc;
    {
        // diagnostics for the first few draws
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 4) {
            OfxPropertySetHandle ep;
            gEffect->getPropertySet(effect, &ep);
            double off[2] = {-1, -1}, size[2] = {-1, -1}, ext[2] = {-1, -1}, rs[2] = {-1, -1}, par = -1;
            OfxStatus s1 = gProp->propGetDoubleN(ep, kOfxImageEffectPropProjectOffset, 2, off);
            OfxStatus s2 = gProp->propGetDoubleN(ep, kOfxImageEffectPropProjectSize, 2, size);
            gProp->propGetDoubleN(ep, kOfxImageEffectPropProjectExtent, 2, ext);
            gProp->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, rs);
            gProp->propGetDouble(ep, kOfxImageEffectPropProjectPixelAspectRatio, 0, &par);
            OfxImageClipHandle oc;
            OfxRectD rod = {-1, -1, -1, -1};
            if (gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &oc, nullptr) == kOfxStatOK)
                gEffect->clipGetRegionOfDefinition(oc, time, &rod);
            char buf[512];
            snprintf(buf, sizeof buf,
                     "overlay draw #%d: %s | projOffset(%d)=%.1f,%.1f projSize(%d)=%.1f,%.1f extent=%.1f,%.1f par=%.3f "
                     "pixelScale=%.4f,%.4f renderScale=%.3f,%.3f outRoD=%.1f,%.1f,%.1f,%.1f view=%.1f,%.1f-%.1f,%.1f",
                     logged.load(), dc ? "draw suite" : "OpenGL", s1, off[0], off[1], s2, size[0], size[1], ext[0], ext[1],
                     par, pxs[0], pxs[1], rs[0], rs[1], rod.x1, rod.y1, rod.x2, rod.y2, v.x0, v.y0, v.x1, v.y1);
            appendLog(buf);
        }
    }
    if (!dc) { glPushAttrib(GL_ALL_ATTRIB_BITS); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_LINE_SMOOTH); }

    // backdrop
    pt.color(0, 0, 0, 0.35f);
    pt.poly({{v.x0, v.y0}, {v.x1, v.y0}, {v.x1, v.y1}, {v.x0, v.y1}});
    pt.width(1);
    pt.color(1, 1, 1, 0.25f);
    pt.strip({{v.x0, v.y0}, {v.x1, v.y0}, {v.x1, v.y1}, {v.x0, v.y1}, {v.x0, v.y0}});

    // dissolve (blue)
    const double lo = c.center - 0.5 / c.speed, hi = c.center + 0.5 / c.speed;
    pt.width(2);
    pt.color(0.45f, 0.45f, 1.0f, 1);
    std::vector<OfxPointD> mix;
    mix.push_back(v.mixp(0, dissolveMix(c, 0)));
    const double a = std::clamp(lo, 0.0, 1.0), b = std::clamp(hi, 0.0, 1.0);
    for (int i = 0; i <= 100; ++i) {  // sampled so a bent dissolve is drawn as a curve
        double t = a + (b - a) * i / 100.0;
        mix.push_back(v.mixp(t, dissolveMix(c, t)));
    }
    mix.push_back(v.mixp(1, dissolveMix(c, 1)));
    pt.strip(mix);

    // shake envelope (yellow)
    pt.color(1, 1, 0, 1);
    std::vector<OfxPointD> env;
    for (int i = 0; i <= 200; ++i) { double t = i / 200.0; env.push_back(v.env(t, shakeEnvelope(c, t))); }
    pt.strip(env);

    // handles
    OfxPointD h[kHCount];
    handlePositions(c, v, h);
    pt.color(1, 0.15f, 0.15f, 1);
    pt.width(1.5f);
    pt.lines({v.env(0, 0), h[kHEaseIn], v.env(1, 0), h[kHEaseOut], h[kHPeak], h[kHPeakEase]});
    const double rx = 5 * pxs[0], ry = 5 * pxs[1];
    for (int k : {kHPeak, kHPeakEase, kHEaseIn, kHEaseOut}) pt.dot(h[k], rx, ry);
    pt.color(0.45f, 0.45f, 1.0f, 1);
    pt.dot(h[kHMixLo], rx, ry);
    pt.dot(h[kHMixHi], rx, ry);
    // grabbable / grabbed handle: bigger, with a white ring (OFX can't change the mouse cursor)
    if (active >= 0 && active < kHCount) {
        const OfxPointD a = h[active];
        const double R = 2.2;
        pt.color(1, 1, 1, 1);
        pt.width(2);
        pt.strip({{a.x - R * rx, a.y - R * ry}, {a.x + R * rx, a.y - R * ry}, {a.x + R * rx, a.y + R * ry},
                  {a.x - R * rx, a.y + R * ry}, {a.x - R * rx, a.y - R * ry}});
        if (active >= kHMixLo) pt.color(0.45f, 0.45f, 1.0f, 1); else pt.color(1, 0.15f, 0.15f, 1);
        pt.dot(a, 1.6 * rx, 1.6 * ry);
    } else if (active >= kHBendIn) {
        // grabbable curve segment: draw it thick and bright
        std::vector<OfxPointD> seg;
        double a = 0, b = 1;
        if (active == kHBendIn) b = c.peakX;
        else if (active == kHBendOut) a = c.peakX;
        else { a = std::max(0.0, lo); b = std::min(1.0, hi); }
        for (int i = 0; i <= 100; ++i) {
            double t = a + (b - a) * i / 100.0;
            seg.push_back(active == kHBendMix ? v.mixp(t, dissolveMix(c, t)) : v.env(t, shakeEnvelope(c, t)));
        }
        pt.width(5);
        if (active == kHBendMix) pt.color(0.75f, 0.75f, 1.0f, 1); else pt.color(1, 1, 0.6f, 1);
        pt.strip(seg);
    }

    // current position
    const int mode = modeOf(ps);
    const double prog = dissolveProgress(ps, time, mode);
    pt.color(1, 1, 1, 0.6f);
    pt.width(1);
    pt.lines({{v.x0 + prog * (v.x1 - v.x0), v.y0}, {v.x0 + prog * (v.x1 - v.x0), v.y1}});
    pt.color(1, 1, 0, 1);
    pt.dot(v.env(prog, shakeEnvelope(c, prog)), 0.7 * rx, 0.7 * ry);
    pt.color(1, 1, 1, 0.8f);
    pt.text("B2K Dissolve Shake - yellow: shake, blue: dissolve", {v.x0 + 8 * pxs[0], v.y1 - 18 * pxs[1]});

    if (!dc) glPopAttrib();
    return kOfxStatOK;
}

static OfxStatus overlayPen(const char* action, OfxInteractHandle ih, OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    OfxTime time = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OfxParamSetHandle ps;
    gEffect->getParamSet(effect, &ps);
    if (!getI(ps, "showCurves", time)) return kOfxStatReplyDefault;
    double pen[2] = {0, 0}, pxs[2] = {1, 1};
    gProp->propGetDoubleN(inArgs, kOfxInteractPropPenPosition, 2, pen);
    gProp->propGetDoubleN(inArgs, kOfxInteractPropPixelScale, 2, pxs);
    Curves c;
    readCurves(ps, time, c);

    std::lock_guard<std::mutex> lock(gDragMutex);
    DragState& d = dragFor(ih);
    const bool down = !std::strcmp(action, kOfxInteractActionPenDown);
    if (d.handle < 0 && !down && !std::strcmp(action, kOfxInteractActionPenMotion)) {
        // hover: highlight the handle that would be grabbed
        int hv = pickHandle(c, makeView(effect, c, pxs), pen, pxs);
        if (hv != d.hover) {
            d.hover = hv;
            if (gInteract) gInteract->interactRedraw(ih);
        }
        return kOfxStatReplyDefault;
    }
    if (down) {
        CurveView v = makeView(effect, c, pxs);
        int best = pickHandle(c, v, pen, pxs);
        if (best < 0) return kOfxStatReplyDefault;
        d.handle = best;
        d.vmax = v.vmax;
        d.startY = pen[1];
        if (best >= kHBendIn) d.startBend = getD(ps, bendParam(best), time);
        gParam->paramEditBegin(ps, "Edit dissolve curve");
        return kOfxStatOK;
    }
    if (d.handle < 0) return kOfxStatReplyDefault;
    if (!std::strcmp(action, kOfxInteractActionPenUp)) {
        d.handle = -1;
        gParam->paramEditEnd(ps);
        return kOfxStatOK;
    }
    // pen motion while dragging
    CurveView v = makeView(effect, c, pxs);
    v.vmax = d.vmax;  // keep the scale stable while dragging
    const double t = std::clamp(v.tOf(pen[0]), 0.0, 1.0);
    switch (d.handle) {
        case kHBendIn:
        case kHBendOut:
        case kHBendMix:
            // dragging half the box height up/down = full bend
            setParam(ps, bendParam(d.handle), std::clamp(d.startBend + 2.0 * (pen[1] - d.startY) / (v.y1 - v.y0), -1.0, 1.0));
            break;
        case kHPeak:
            setParam(ps, "shakePeakPos", std::clamp(t, 0.02, 0.98));
            setParam(ps, "shakePeakAmp", std::clamp(v.vOf(pen[1]), 0.0, 10.0));
            break;
        case kHPeakEase:
            if (c.peakY > 0) setParam(ps, "peakEase", std::clamp(1.0 - v.vOf(pen[1]) / c.peakY, 0.0, 1.0));
            break;
        case kHEaseIn: setParam(ps, "easeIn", std::clamp(t / c.peakX, 0.0, 1.0)); break;
        case kHEaseOut: setParam(ps, "easeOut", std::clamp((1.0 - t) / (1.0 - c.peakX), 0.0, 1.0)); break;
        case kHMixLo:
        case kHMixHi: {
            // move one blue dot, keep the other one where it is
            double other = d.handle == kHMixLo ? c.center + 0.25 / c.speed : c.center - 0.25 / c.speed;
            // the dots may not cross; min gap 0.025 = max speed 20, max gap 0.5 = min speed 1
            double tt = d.handle == kHMixLo ? std::clamp(t, other - 0.5, other - 0.025)
                                            : std::clamp(t, other + 0.025, other + 0.5);
            double gap = std::fabs(other - tt);
            setParam(ps, "dissolveSpeed", 0.5 / gap);
            setParam(ps, "dissolveCenter", 0.5 * (tt + other));
            break;
        }
    }
    if (gInteract) gInteract->interactRedraw(ih);
    return kOfxStatOK;
}

static OfxStatus overlayMain(const char* action, const void* handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle) {
    try {
        if (!std::strcmp(action, kOfxActionDescribe) || !std::strcmp(action, kOfxActionCreateInstance)) return kOfxStatOK;
        OfxInteractHandle ih = (OfxInteractHandle)handle;
        if (!std::strcmp(action, kOfxActionDestroyInstance)) {
            std::lock_guard<std::mutex> lock(gDragMutex);
            gDrags.erase(std::remove_if(gDrags.begin(), gDrags.end(), [&](auto& d) { return d.first == ih; }), gDrags.end());
            return kOfxStatOK;
        }
        const bool draw = !std::strcmp(action, kOfxInteractActionDraw);
        const bool pen = !std::strcmp(action, kOfxInteractActionPenDown) || !std::strcmp(action, kOfxInteractActionPenMotion) ||
                         !std::strcmp(action, kOfxInteractActionPenUp);
        if (!draw && !pen) return kOfxStatReplyDefault;
        void* eff = nullptr;
        if (inArgs) gProp->propGetPointer(inArgs, kOfxPropEffectInstance, 0, &eff);
        if (!eff && gInteract) {
            OfxPropertySetHandle ip;
            if (gInteract->interactGetPropertySet(ih, &ip) == kOfxStatOK) gProp->propGetPointer(ip, kOfxPropEffectInstance, 0, &eff);
        }
        if (!eff) return kOfxStatReplyDefault;
        if (draw) return overlayDraw(ih, (OfxImageEffectHandle)eff, inArgs);
        return overlayPen(action, ih, (OfxImageEffectHandle)eff, inArgs);
    } catch (...) {
        return kOfxStatFailed;
    }
}

// Writes what the host offers (name, version, GPU APIs, overlays) to %TEMP%\B2KShake_host.txt
static void logHostInfo() {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) return;
    std::string path = std::string(tmp) + "\\B2KShake_host.txt";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    OfxPropertySetHandle h = gHost->host;
    const char* strProps[] = {kOfxPropName, kOfxPropLabel, kOfxPropVersionLabel,
                              kOfxImageEffectPropOpenGLRenderSupported, kOfxImageEffectPropCudaRenderSupported,
                              kOfxImageEffectPropCudaStreamSupported, kOfxImageEffectPropOpenCLRenderSupported,
                              kOfxImageEffectPropOpenCLSupported, kOfxImageEffectPropMetalRenderSupported,
                              kOfxImageEffectPropSupportsOverlays};
    for (const char* n : strProps) {
        char* v = nullptr;
        int iv = 0;
        if (gProp->propGetString(h, n, 0, &v) == kOfxStatOK && v) std::fprintf(f, "%s = %s\n", n, v);
        else if (gProp->propGetInt(h, n, 0, &iv) == kOfxStatOK) std::fprintf(f, "%s = %d\n", n, iv);
        else std::fprintf(f, "%s = <not set>\n", n);
    }
    std::fprintf(f, "InteractSuite = %s\nDrawSuite = %s\n", gInteract ? "yes" : "no", gDraw ? "yes" : "no");
    std::fclose(f);
}

static OfxStatus mainEntryImpl(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                               OfxPropertySetHandle outArgs, int mode) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    try {
        if (!std::strcmp(action, kOfxActionLoad)) {
            if (!gHost) return kOfxStatErrMissingHostFeature;
            gProp = (const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
            gEffect = (const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
            gParam = (const OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
            gInteract = (const OfxInteractSuiteV1*)gHost->fetchSuite(gHost->host, kOfxInteractSuite, 1);
            gDraw = (const OfxDrawSuiteV1*)gHost->fetchSuite(gHost->host, kOfxDrawSuite, 1);
            static std::once_flag logOnce;
            if (gProp) std::call_once(logOnce, logHostInfo);
            return (gProp && gEffect && gParam) ? kOfxStatOK : kOfxStatErrMissingHostFeature;
        }
        if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect, mode);
        if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect, mode);
        if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, inArgs, mode);
        if (!std::strcmp(action, kOfxImageEffectActionGetRegionsOfInterest)) return getRoI(effect, inArgs, outArgs, mode);
        if (!std::strcmp(action, kOfxImageEffectActionIsIdentity)) return isIdentity(effect, inArgs, outArgs, mode);
        if (!std::strcmp(action, kOfxActionCreateInstance) || !std::strcmp(action, kOfxActionDestroyInstance) ||
            !std::strcmp(action, kOfxActionUnload))
            return kOfxStatOK;
    } catch (...) {
        return kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

static OfxStatus mainEntryShake(const char* a, const void* h, OfxPropertySetHandle i, OfxPropertySetHandle o) {
    return mainEntryImpl(a, h, i, o, kModeShake);
}
static OfxStatus mainEntryDissolveTr(const char* a, const void* h, OfxPropertySetHandle i, OfxPropertySetHandle o) {
    return mainEntryImpl(a, h, i, o, kModeDissolveTransition);
}
static OfxStatus mainEntryDissolveFx(const char* a, const void* h, OfxPropertySetHandle i, OfxPropertySetHandle o) {
    return mainEntryImpl(a, h, i, o, kModeDissolveFilter);
}

static void setHost(OfxHost* host) { gHost = host; }

static OfxPlugin gPlugins[3] = {
    {kOfxImageEffectPluginApi, 1, "com.b2k.Shake", 1, 1, setHost, mainEntryShake},
    {kOfxImageEffectPluginApi, 1, "com.b2k.DissolveShake", 1, 1, setHost, mainEntryDissolveTr},
    {kOfxImageEffectPluginApi, 1, "com.b2k.DissolveShakeFX", 1, 0, setHost, mainEntryDissolveFx},
};

SFX_EXPORT int OfxGetNumberOfPlugins(void) { return 3; }
SFX_EXPORT OfxPlugin* OfxGetPlugin(int nth) { return (nth >= 0 && nth < 3) ? &gPlugins[nth] : nullptr; }
