// Minimal mock OFX host: loads ShakeFX.ofx, runs describe + render, writes PPMs.
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "ofxImageEffect.h"
#include "ofxGPURender.h"
#include "ofxDrawSuite.h"
#include "../src/cl_min.h"
#include <algorithm>

struct PropSet {
    std::map<std::string, std::vector<std::string>> s;
    std::map<std::string, std::vector<int>> i;
    std::map<std::string, std::vector<double>> d;
    std::map<std::string, std::vector<void*>> p;
};
#define PS(h) ((PropSet*)(h))
template <class M, class V> static void setv(M& m, const char* n, int idx, V v) { auto& vec = m[n]; if ((int)vec.size() <= idx) vec.resize(idx + 1); vec[idx] = v; }
template <class M, class V> static OfxStatus getv(M& m, const char* n, int idx, V* v) { auto it = m.find(n); if (it == m.end() || idx >= (int)it->second.size()) return kOfxStatErrUnknown; *v = it->second[idx]; return kOfxStatOK; }

static OfxStatus pSetPtr(OfxPropertySetHandle h, const char* n, int i, void* v) { setv(PS(h)->p, n, i, v); return kOfxStatOK; }
static std::vector<std::string*> gStrPool;
static OfxStatus pSetStr(OfxPropertySetHandle h, const char* n, int i, const char* v) { setv(PS(h)->s, n, i, std::string(v)); return kOfxStatOK; }
static OfxStatus pSetDbl(OfxPropertySetHandle h, const char* n, int i, double v) { setv(PS(h)->d, n, i, v); return kOfxStatOK; }
static OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int i, int v) { setv(PS(h)->i, n, i, v); return kOfxStatOK; }
static OfxStatus pSetPtrN(OfxPropertySetHandle h, const char* n, int c, void* const* v) { for (int k = 0; k < c; ++k) pSetPtr(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus pSetStrN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) { for (int k = 0; k < c; ++k) pSetStr(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus pSetDblN(OfxPropertySetHandle h, const char* n, int c, const double* v) { for (int k = 0; k < c; ++k) pSetDbl(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) { for (int k = 0; k < c; ++k) pSetInt(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus pGetPtr(OfxPropertySetHandle h, const char* n, int i, void** v) { return getv(PS(h)->p, n, i, v); }
static OfxStatus pGetStr(OfxPropertySetHandle h, const char* n, int i, char** v) {
    auto it = PS(h)->s.find(n); if (it == PS(h)->s.end() || i >= (int)it->second.size()) return kOfxStatErrUnknown;
    *v = (char*)it->second[i].c_str(); return kOfxStatOK;
}
static OfxStatus pGetDbl(OfxPropertySetHandle h, const char* n, int i, double* v) { return getv(PS(h)->d, n, i, v); }
static OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int i, int* v) { return getv(PS(h)->i, n, i, v); }
static OfxStatus pGetPtrN(OfxPropertySetHandle h, const char* n, int c, void** v) { for (int k = 0; k < c; ++k) if (pGetPtr(h, n, k, v + k)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetStrN(OfxPropertySetHandle h, const char* n, int c, char** v) { for (int k = 0; k < c; ++k) if (pGetStr(h, n, k, v + k)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetDblN(OfxPropertySetHandle h, const char* n, int c, double* v) { for (int k = 0; k < c; ++k) if (pGetDbl(h, n, k, v + k)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) { for (int k = 0; k < c; ++k) if (pGetInt(h, n, k, v + k)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pReset(OfxPropertySetHandle, const char*) { return kOfxStatOK; }
static OfxStatus pDim(OfxPropertySetHandle, const char*, int* c) { *c = 1; return kOfxStatOK; }
static OfxPropertySuiteV1 gPropSuite = {pSetPtr, pSetStr, pSetDbl, pSetInt, pSetPtrN, pSetStrN, pSetDblN, pSetIntN,
                                        pGetPtr, pGetStr, pGetDbl, pGetInt, pGetPtrN, pGetStrN, pGetDblN, pGetIntN, pReset, pDim};

struct Param { std::string type; PropSet props; };
struct ParamSet { std::map<std::string, Param*> params; std::map<std::string, double> overrides; PropSet props; };
struct Clip { PropSet props; std::vector<float> img; int w = 0, h = 0; void* clImage = nullptr; };
struct Effect { PropSet props; ParamSet params; std::map<std::string, Clip*> clips; };

static OfxStatus parDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* props) {
    auto* p = new Param{type, {}}; ((ParamSet*)ps)->params[name] = p; if (props) *props = (OfxPropertySetHandle)&p->props; return kOfxStatOK;
}
static OfxStatus parGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* h, OfxPropertySetHandle* props) {
    auto& m = ((ParamSet*)ps)->params; auto it = m.find(name);
    if (it == m.end()) { printf("  !! unknown param %s\n", name); return kOfxStatErrUnknown; }
    // encode param set + name: store pointer to a pair
    static std::map<std::string, std::pair<ParamSet*, Param*>> handles;
    auto& e = handles[name]; e = {(ParamSet*)ps, it->second};
    *h = (OfxParamHandle)&e; if (props) *props = (OfxPropertySetHandle)&it->second->props; return kOfxStatOK;
}
static OfxStatus parSetGetPS(OfxParamSetHandle ps, OfxPropertySetHandle* p) { *p = (OfxPropertySetHandle)&((ParamSet*)ps)->props; return kOfxStatOK; }
static OfxStatus parGetPS(OfxParamHandle h, OfxPropertySetHandle* p) { *p = (OfxPropertySetHandle)&((std::pair<ParamSet*, Param*>*)h)->second->props; return kOfxStatOK; }
static std::string nameOf(std::pair<ParamSet*, Param*>* e) { for (auto& kv : e->first->params) if (kv.second == e->second) return kv.first; return ""; }
static OfxStatus valueImpl(OfxParamHandle h, va_list ap) {
    auto* e = (std::pair<ParamSet*, Param*>*)h;
    std::string n = nameOf(e);
    auto ov = e->first->overrides.find(n);
    if (e->second->type == kOfxParamTypeDouble) {
        double* v = va_arg(ap, double*);
        *v = ov != e->first->overrides.end() ? ov->second : e->second->props.d[kOfxParamPropDefault][0];
    } else {
        int* v = va_arg(ap, int*);
        *v = ov != e->first->overrides.end() ? (int)ov->second : e->second->props.i[kOfxParamPropDefault][0];
    }
    return kOfxStatOK;
}
static OfxStatus parGetValue(OfxParamHandle h, ...) { va_list ap; va_start(ap, h); auto r = valueImpl(h, ap); va_end(ap); return r; }
static OfxStatus parGetValueAtTime(OfxParamHandle h, OfxTime t, ...) { va_list ap; va_start(ap, t); auto r = valueImpl(h, ap); va_end(ap); return r; }
static OfxStatus parNI(...) { return kOfxStatErrUnsupported; }
static OfxStatus parSetValue(OfxParamHandle h, ...) {
    auto* e = (std::pair<ParamSet*, Param*>*)h;
    va_list ap; va_start(ap, h);
    double v = e->second->type == kOfxParamTypeDouble ? va_arg(ap, double) : (double)va_arg(ap, int);
    va_end(ap);
    e->first->overrides[nameOf(e)] = v;
    return kOfxStatOK;
}
static OfxStatus parEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
static OfxStatus parEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

// ---- fake draw suite: rasterises lines into an RGB canvas (canonical coords == pixels)
struct Canvas { int w = 0, h = 0; std::vector<float> px; float col[4] = {1, 1, 1, 1}; int prims = 0; };
static Canvas gCanvas;
static void plot(int x, int y) {
    if (x < 0 || y < 0 || x >= gCanvas.w || y >= gCanvas.h) return;
    float* p = &gCanvas.px[(size_t)(y * gCanvas.w + x) * 4]; float a = gCanvas.col[3];
    for (int c = 0; c < 3; ++c) p[c] = p[c] * (1 - a) + gCanvas.col[c] * a;
}
static void line(OfxPointD a, OfxPointD b) {
    int n = (int)std::max(fabs(b.x - a.x), fabs(b.y - a.y)) + 1;
    for (int i = 0; i <= n; ++i) { double t = (double)i / n; for (int d = -1; d <= 1; ++d) { plot((int)(a.x + (b.x - a.x) * t) + d, (int)(a.y + (b.y - a.y) * t)); plot((int)(a.x + (b.x - a.x) * t), (int)(a.y + (b.y - a.y) * t) + d); } }
}
static OfxStatus dsGetColour(OfxDrawContextHandle, OfxStandardColour, OfxRGBAColourF*) { return kOfxStatOK; }
static OfxStatus dsSetColour(OfxDrawContextHandle, const OfxRGBAColourF* c) { gCanvas.col[0] = c->r; gCanvas.col[1] = c->g; gCanvas.col[2] = c->b; gCanvas.col[3] = c->a; return kOfxStatOK; }
static OfxStatus dsSetLineWidth(OfxDrawContextHandle, float) { return kOfxStatOK; }
static OfxStatus dsSetStipple(OfxDrawContextHandle, OfxDrawLineStipplePattern) { return kOfxStatOK; }
static OfxStatus dsDraw(OfxDrawContextHandle, OfxDrawPrimitive prim, const OfxPointD* p, int n) {
    ++gCanvas.prims;
    if (prim == kOfxDrawPrimitiveLines) for (int i = 0; i + 1 < n; i += 2) line(p[i], p[i + 1]);
    else if (prim == kOfxDrawPrimitiveLineStrip) for (int i = 0; i + 1 < n; ++i) line(p[i], p[i + 1]);
    else if (prim == kOfxDrawPrimitivePolygon && n == 4 && fabs(p[2].x - p[0].x) < 30) {  // handle dots: fill
        for (int y = (int)p[0].y; y <= (int)p[2].y; ++y) for (int x = (int)p[0].x; x <= (int)p[2].x; ++x) plot(x, y);
    }
    return kOfxStatOK;
}
static OfxStatus dsText(OfxDrawContextHandle, const char*, const OfxPointD*, int) { return kOfxStatOK; }
static OfxDrawSuiteV1 gDrawSuite = {dsGetColour, dsSetColour, dsSetLineWidth, dsSetStipple, dsDraw, dsText};
static PropSet gInteractProps;
static OfxStatus isSwap(OfxInteractHandle) { return kOfxStatOK; }
static OfxStatus isRedraw(OfxInteractHandle) { return kOfxStatOK; }
static OfxStatus isGetPS(OfxInteractHandle, OfxPropertySetHandle* p) { *p = (OfxPropertySetHandle)&gInteractProps; return kOfxStatOK; }
static OfxInteractSuiteV1 gInteractSuite = {isSwap, isRedraw, isGetPS};
static OfxParameterSuiteV1 gParamSuite;

static OfxStatus effGetPS(OfxImageEffectHandle e, OfxPropertySetHandle* p) { *p = (OfxPropertySetHandle)&((Effect*)e)->props; return kOfxStatOK; }
static OfxStatus effGetParamSet(OfxImageEffectHandle e, OfxParamSetHandle* p) { *p = (OfxParamSetHandle)&((Effect*)e)->params; return kOfxStatOK; }
static OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* p) { auto* c = new Clip; ((Effect*)e)->clips[name] = c; *p = (OfxPropertySetHandle)&c->props; return kOfxStatOK; }
static OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* c, OfxPropertySetHandle* p) {
    auto it = ((Effect*)e)->clips.find(name); if (it == ((Effect*)e)->clips.end()) return kOfxStatErrUnknown;
    *c = (OfxImageClipHandle)it->second; if (p) *p = (OfxPropertySetHandle)&it->second->props; return kOfxStatOK;
}
static OfxStatus clipGetPS(OfxImageClipHandle c, OfxPropertySetHandle* p) { *p = (OfxPropertySetHandle)&((Clip*)c)->props; return kOfxStatOK; }
static OfxStatus clipGetImage(OfxImageClipHandle ch, OfxTime, const OfxRectD*, OfxPropertySetHandle* out) {
    Clip* c = (Clip*)ch; auto* ps = new PropSet;
    pSetPtr((OfxPropertySetHandle)ps, kOfxImagePropData, 0, c->img.data());
    int b[4] = {0, 0, c->w, c->h}; pSetIntN((OfxPropertySetHandle)ps, kOfxImagePropBounds, 4, b);
    pSetInt((OfxPropertySetHandle)ps, kOfxImagePropRowBytes, 0, c->w * 16);
    pSetStr((OfxPropertySetHandle)ps, kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
    pSetStr((OfxPropertySetHandle)ps, kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
    pSetDbl((OfxPropertySetHandle)ps, kOfxImagePropPixelAspectRatio, 0, 1.0);
    if (c->clImage) pSetPtr((OfxPropertySetHandle)ps, kOfxImageEffectPropOpenCLImage, 0, c->clImage);
    *out = (OfxPropertySetHandle)ps; return kOfxStatOK;
}
static OfxStatus clipRelease(OfxPropertySetHandle h) { delete PS(h); return kOfxStatOK; }
static OfxStatus clipRoD(OfxImageClipHandle c, OfxTime, OfxRectD* r) { *r = {0, 0, (double)((Clip*)c)->w, (double)((Clip*)c)->h}; return kOfxStatOK; }
static int effAbort(OfxImageEffectHandle) { return 0; }
static OfxImageEffectSuiteV1 gEffSuite;

static const void* fetchSuite(OfxPropertySetHandle, const char* name, int) {
    if (!strcmp(name, kOfxPropertySuite)) return &gPropSuite;
    if (!strcmp(name, kOfxParameterSuite)) return &gParamSuite;
    if (!strcmp(name, kOfxImageEffectSuite)) return &gEffSuite;
    if (!strcmp(name, kOfxDrawSuite)) return &gDrawSuite;
    if (!strcmp(name, kOfxInteractSuite)) return &gInteractSuite;
    return nullptr;
}

static void writePPM(const char* fn, const std::vector<float>& img, int w, int h) {
    FILE* f = fopen(fn, "wb"); fprintf(f, "P6 %d %d 255\n", w, h);
    for (int y = h - 1; y >= 0; --y) for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
        float v = img[(size_t)(y * w + x) * 4 + c]; v = v < 0 ? 0 : v > 1 ? 1 : v; fputc((int)(v * 255 + 0.5f), f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    memset(&gParamSuite, 0, sizeof gParamSuite);
    gParamSuite.paramDefine = parDefine; gParamSuite.paramGetHandle = parGetHandle;
    gParamSuite.paramSetGetPropertySet = parSetGetPS; gParamSuite.paramGetPropertySet = parGetPS;
    gParamSuite.paramGetValue = parGetValue; gParamSuite.paramGetValueAtTime = parGetValueAtTime;
    gParamSuite.paramSetValue = parSetValue; gParamSuite.paramEditBegin = parEditBegin; gParamSuite.paramEditEnd = parEditEnd;
    memset(&gEffSuite, 0, sizeof gEffSuite);
    gEffSuite.getPropertySet = effGetPS; gEffSuite.getParamSet = effGetParamSet; gEffSuite.clipDefine = clipDefine;
    gEffSuite.clipGetHandle = clipGetHandle; gEffSuite.clipGetPropertySet = clipGetPS; gEffSuite.clipGetImage = clipGetImage;
    gEffSuite.clipReleaseImage = clipRelease; gEffSuite.clipGetRegionOfDefinition = clipRoD; gEffSuite.abort = effAbort;

    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto getN = (int (*)())GetProcAddress(m, "OfxGetNumberOfPlugins");
    auto getP = (OfxPlugin * (*)(int)) GetProcAddress(m, "OfxGetPlugin");
    if (!getN || !getP) { printf("missing exports\n"); return 1; }
    const int plIdx = argc > 4 ? atoi(argv[4]) : 0;
    OfxPlugin* pl = getP(plIdx);
    if (!pl) { printf("no plugin %d\n", plIdx); return 1; }
    printf("plugins=%d id=%s api=%s\n", getN(), pl->pluginIdentifier, pl->pluginApi);
    PropSet hostProps; OfxHost host = {(OfxPropertySetHandle)&hostProps, fetchSuite};
    pl->setHost(&host);
    Effect eff;
    printf("load=%d\n", pl->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr));
    printf("describe=%d\n", pl->mainEntry(kOfxActionDescribe, &eff, nullptr, nullptr));
    PropSet ctx; pSetStr((OfxPropertySetHandle)&ctx, kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    printf("describeInContext=%d params=%zu clips=%zu\n", pl->mainEntry(kOfxImageEffectActionDescribeInContext, &eff, (OfxPropertySetHandle)&ctx, nullptr), eff.params.params.size(), eff.clips.size());
    pSetDbl((OfxPropertySetHandle)&eff.props, kOfxImageEffectPropFrameRate, 0, 25.0);
    printf("label=%s clips=%zu params=%zu groups open:", eff.props.s[kOfxPropLabel][0].c_str(), eff.clips.size(), eff.params.params.size());
    for (auto& kv : eff.params.params) if (kv.second->type == kOfxParamTypeGroup) printf(" %s=%d", kv.first.c_str(), kv.second->props.i[kOfxParamPropGroupOpen][0]);
    printf("\n");

    const int W = argc > 2 ? atoi(argv[2]) : 640, H = argc > 3 ? atoi(argv[3]) : 360;
    const bool trans = eff.clips.count(kOfxImageEffectTransitionSourceToClipName) > 0;
    Clip* src = eff.clips[trans ? kOfxImageEffectTransitionSourceFromClipName : kOfxImageEffectSimpleSourceClipName];
    Clip* srcB = trans ? eff.clips[kOfxImageEffectTransitionSourceToClipName] : nullptr;
    Clip* dst = eff.clips[kOfxImageEffectOutputClipName];
    src->w = dst->w = W; src->h = dst->h = H;
    src->img.resize((size_t)W * H * 4); dst->img.resize((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        float* p = &src->img[(size_t)(y * W + x) * 4];
        bool chk = ((x / 40) + (y / 40)) & 1;
        p[0] = chk ? 0.9f : 0.1f; p[1] = (float)x / W; p[2] = (float)y / H; p[3] = 1.f;
        if (abs(x - W / 2) < 3 || abs(y - H / 2) < 3) p[0] = p[1] = p[2] = 1.f;
    }
    writePPM("out_src.ppm", src->img, W, H);
    if (srcB) {  // "to" clip: diagonal yellow/blue stripes
        srcB->w = W; srcB->h = H; srcB->img.resize((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            float* p = &srcB->img[(size_t)(y * W + x) * 4];
            bool st = ((x + y) / 30) & 1;
            p[0] = st ? 1.f : 0.f; p[1] = st ? 0.9f : 0.2f; p[2] = st ? 0.f : 1.f; p[3] = 1.f;
        }
    }

    struct Case { const char* name; std::map<std::string, double> ov; double time; };
    const bool dissFx = eff.params.params.count("dissolveAmount") > 0;
    double psz[2] = {(double)W, (double)H}, poff[2] = {0, 0};
    pSetDblN((OfxPropertySetHandle)&eff.props, kOfxImageEffectPropProjectSize, 2, psz);
    pSetDblN((OfxPropertySetHandle)&eff.props, kOfxImageEffectPropProjectOffset, 2, poff);
    std::vector<Case> cases = dissFx ? std::vector<Case>{
        {"fx_000", {{"dissolveAmount", 0.0}}, 0},
        {"fx_030", {{"dissolveAmount", 0.3}}, 6},
        {"fx_050", {{"dissolveAmount", 0.5}}, 12},
        {"fx_070", {{"dissolveAmount", 0.7}}, 18},
        {"fx_100", {{"dissolveAmount", 1.0}}, 24},
    } : trans ? std::vector<Case>{
        {"tr_000", {{"Transition", 0.0}}, 0},
        {"tr_025", {{"Transition", 0.25}}, 6},
        {"tr_050", {{"Transition", 0.5}}, 12},
        {"tr_060_mblur", {{"Transition", 0.6}, {"motionBlur", 1}, {"tiltRandAmp", 5}}, 14},
        {"tr_075_rgb", {{"Transition", 0.75}, {"rgbRandomness", 0.5}}, 18},
        {"tr_100", {{"Transition", 1.0}}, 24},
    } : std::vector<Case>{
        {"default_f10", {}, 10},
        {"default_f11", {}, 11},
        {"mblur_tilt_zoom", {{"motionBlur", 1}, {"tiltRandAmp", 8}, {"zRandAmp", 0.2}}, 10},
        {"rgbsplit", {{"rgbRandomness", 0.5}, {"redPhase", 0.1}}, 10},
        {"wrap_black_crop", {{"wrapX", 0}, {"wrapY", 0}, {"cropLeft", 0.1}, {"amplitude", 1.5}}, 12},
        {"jerky_wave", {{"style", 2}, {"xWaveAmp", 0.1}, {"seed", 0.0005}}, 13},
        {"zdist_0.25", {{"zDist", 0.25}}, 10},
        {"zdist_4", {{"zDist", 4}}, 10},
        {"zdist_0.5_noshake", {{"zDist", 0.5}, {"xRandAmp", 0}, {"yRandAmp", 0}}, 10},
        {"mblur_rgb_worst", {{"motionBlur", 1}, {"moBlurLength", 2}, {"rgbRandomness", 0.5}, {"tiltRandAmp", 8}}, 10},
    };
    for (auto& c : cases) {
        eff.params.overrides = c.ov;
        PropSet in, out;
        pSetDbl((OfxPropertySetHandle)&in, kOfxPropTime, 0, c.time);
        int rw[4] = {0, 0, W, H}; pSetIntN((OfxPropertySetHandle)&in, kOfxImageEffectPropRenderWindow, 4, rw);
        double rs[2] = {1, 1}; pSetDblN((OfxPropertySetHandle)&in, kOfxImageEffectPropRenderScale, 2, rs);
        int st = pl->mainEntry(kOfxImageEffectActionGetRegionsOfInterest, &eff, (OfxPropertySetHandle)&in, (OfxPropertySetHandle)&out);
        DWORD t0 = GetTickCount();
        int r = pl->mainEntry(kOfxImageEffectActionRender, &eff, (OfxPropertySetHandle)&in, (OfxPropertySetHandle)&out);
        DWORD dt = GetTickCount() - t0;
        double sum = 0; for (float v : dst->img) sum += v;
        printf("%-18s roi=%d render=%d  %lums  mean=%.4f\n", c.name, st, r, dt, sum / dst->img.size());
        char fn[128]; snprintf(fn, sizeof fn, "out_%s.ppm", c.name); writePPM(fn, dst->img, W, H);
    }

    // ---- OpenCL path: render every case on the GPU and compare to the CPU result
    ClApi cl;
    cl_platform_id plats[8]; cl_uint np = 0; cl_device_id dev = nullptr;
    if (cl.load() && cl.GetPlatformIDs(8, plats, &np) == CL_SUCCESS) {
        for (cl_uint i = 0; i < np && !dev; ++i) cl.GetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 1, &dev, nullptr);
    }
    if (!dev) { printf("no OpenCL GPU\n"); }
    else {
        cl_int err;
        cl_context ctx = cl.CreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
        cl_command_queue q = cl.CreateCommandQueue(ctx, dev, 0, &err);
        cl_image_format fmt = {CL_RGBA, CL_FLOAT};
        cl_image_desc desc = {}; desc.image_type = CL_MEM_OBJECT_IMAGE2D; desc.image_width = W; desc.image_height = H;
        cl_mem sImg = cl.CreateImage(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, &fmt, &desc, src->img.data(), &err);
        cl_mem dImg = cl.CreateImage(ctx, CL_MEM_READ_WRITE, &fmt, &desc, nullptr, &err);
        cl_mem bImg = srcB ? cl.CreateImage(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, &fmt, &desc, srcB->img.data(), &err) : nullptr;
        src->clImage = sImg; dst->clImage = dImg; if (srcB) srcB->clImage = bImg;
        std::vector<float> gpu((size_t)W * H * 4);
        for (auto& c : cases) {
            eff.params.overrides = c.ov;
            PropSet in, out;
            pSetDbl((OfxPropertySetHandle)&in, kOfxPropTime, 0, c.time);
            int rw[4] = {0, 0, W, H}; pSetIntN((OfxPropertySetHandle)&in, kOfxImageEffectPropRenderWindow, 4, rw);
            pSetInt((OfxPropertySetHandle)&in, kOfxImageEffectPropOpenCLEnabled, 0, 1);
            pSetPtr((OfxPropertySetHandle)&in, kOfxImageEffectPropOpenCLCommandQueue, 0, q);
            DWORD t0 = GetTickCount();
            int r = pl->mainEntry(kOfxImageEffectActionRender, &eff, (OfxPropertySetHandle)&in, (OfxPropertySetHandle)&out);
            cl.Finish(q);
            DWORD dt = GetTickCount() - t0;
            size_t origin[3] = {0, 0, 0}, region[3] = {(size_t)W, (size_t)H, 1};
            cl.EnqueueReadImage(q, dImg, 1, origin, region, 0, 0, gpu.data(), 0, nullptr, nullptr);
            // CPU reference
            src->clImage = dst->clImage = nullptr; if (srcB) srcB->clImage = nullptr;
            PropSet in2; pSetDbl((OfxPropertySetHandle)&in2, kOfxPropTime, 0, c.time); pSetIntN((OfxPropertySetHandle)&in2, kOfxImageEffectPropRenderWindow, 4, rw);
            pl->mainEntry(kOfxImageEffectActionRender, &eff, (OfxPropertySetHandle)&in2, (OfxPropertySetHandle)&out);
            src->clImage = sImg; dst->clImage = dImg; if (srcB) srcB->clImage = bImg;
            double maxd = 0, sumd = 0;
            for (size_t k = 0; k < gpu.size(); ++k) { double d = fabs(gpu[k] - dst->img[k]); maxd = d > maxd ? d : maxd; sumd += d; }
            printf("GPU %-18s render=%d  %lums  meanDiff=%.6f maxDiff=%.4f\n", c.name, r, dt, sumd / gpu.size(), maxd);
        }
        src->clImage = dst->clImage = nullptr;
    }
    // ---- overlay: draw curves, drag the yellow peak and a blue dot, draw again
    void* ov = nullptr;
    pGetPtr((OfxPropertySetHandle)&eff.props, kOfxImageEffectPluginPropOverlayInteractV2, 0, &ov);
    if (ov) {
        auto entry = (OfxStatus(*)(const char*, const void*, OfxPropertySetHandle, OfxPropertySetHandle))ov;
        pSetPtr((OfxPropertySetHandle)&gInteractProps, kOfxPropEffectInstance, 0, &eff);
        const char* progName = dissFx ? "dissolveAmount" : "Transition";
        eff.params.overrides = {{progName, 0.4}};
        auto args = [&](PropSet& a, double x, double y) {
            pSetPtr((OfxPropertySetHandle)&a, kOfxPropEffectInstance, 0, &eff);
            pSetDbl((OfxPropertySetHandle)&a, kOfxPropTime, 0, 10);
            double sc[2] = {1, 1}, pp[2] = {x, y};
            pSetDblN((OfxPropertySetHandle)&a, kOfxInteractPropPixelScale, 2, sc);
            pSetDblN((OfxPropertySetHandle)&a, kOfxInteractPropPenPosition, 2, pp);
            pSetPtr((OfxPropertySetHandle)&a, kOfxInteractPropDrawContext, 0, (void*)&gCanvas);
        };
        auto drawTo = [&](const char* fn) {
            gCanvas.w = W; gCanvas.h = H; gCanvas.px.assign((size_t)W * H * 4, 0.f); gCanvas.prims = 0;
            PropSet a; args(a, 0, 0);
            int r = entry(kOfxInteractActionDraw, (void*)1, (OfxPropertySetHandle)&a, nullptr);
            printf("overlay draw=%d prims=%d\n", r, gCanvas.prims);
            writePPM(fn, gCanvas.px, W, H);
        };
        auto pen = [&](const char* act, double t, double v) {
            PropSet a; args(a, 0.01 * W + t * 0.98 * W, 0.02 * H + v * 0.96 * H);
            return entry(act, (void*)1, (OfxPropertySetHandle)&a, nullptr);
        };
        drawTo("overlay_before.ppm");
        int prims0 = gCanvas.prims;
        pen(kOfxInteractActionPenMotion, 0.5, 1.0);  // hover over the peak
        drawTo("overlay_hover.ppm");
        printf("hover adds prims: %d -> %d\n", prims0, gCanvas.prims);
        pen(kOfxInteractActionPenMotion, 0.9, 0.9);  // hover away
        int d1 = pen(kOfxInteractActionPenDown, 0.5, 1.0);
        pen(kOfxInteractActionPenMotion, 0.4, 0.5);
        pen(kOfxInteractActionPenUp, 0.4, 0.5);
        int d2 = pen(kOfxInteractActionPenDown, 0.5 - 0.25 / 3.0, 0.25);
        pen(kOfxInteractActionPenMotion, 0.3, 0.25);
        pen(kOfxInteractActionPenUp, 0.3, 0.25);
        int miss = pen(kOfxInteractActionPenDown, 0.9, 0.9);
        auto& o = eff.params.overrides;
        printf("drag peak hit=%d -> peakPos=%.3f peakAmp=%.3f | drag blue hit=%d -> speed=%.3f center=%.3f | empty click=%d\n",
               d1, o["shakePeakPos"], o["shakePeakAmp"], d2, o["dissolveSpeed"], o["dissolveCenter"], miss);
        drawTo("overlay_after.ppm");

        // crash case: drag a blue dot far past the other one and out of the frame
        eff.params.overrides = {{progName, 0.4}};
        int hb = pen(kOfxInteractActionPenDown, 0.5 - 0.25 / 3.0, 0.25);
        pen(kOfxInteractActionPenMotion, 1.5, 3.0);
        pen(kOfxInteractActionPenMotion, -2.0, -1.0);
        pen(kOfxInteractActionPenUp, -2.0, -1.0);
        printf("far drag blue hit=%d -> speed=%.3f center=%.3f\n", hb, eff.params.overrides["dissolveSpeed"], eff.params.overrides["dissolveCenter"]);

        // bend: grab the yellow curve at t=0.25 and drag up; grab the blue line at t=0.5 and drag down
        eff.params.overrides = {{progName, 0.4}};
        auto grabAndDrag = [&](double t, double dv) {
            for (int i = 0; i <= 100; ++i) {
                double vv = i / 100.0;
                if (pen(kOfxInteractActionPenDown, t, vv) == 0) {
                    pen(kOfxInteractActionPenMotion, t, vv + dv);
                    pen(kOfxInteractActionPenUp, t, vv + dv);
                    return vv;
                }
            }
            return -1.0;
        };
        double gy = grabAndDrag(0.2, 0.25);
        double gb = grabAndDrag(0.5, -0.25);
        printf("bend: yellow grabbed at v=%.2f -> bendIn=%.3f | blue grabbed at v=%.2f -> dissolveBend=%.3f\n", gy,
               eff.params.overrides["bendIn"], gb, eff.params.overrides["dissolveBend"]);
        drawTo("overlay_bent.ppm");

        // VEGAS-style normalised overlay space: frame = 0..1, pixelScale = 1/preview size
        eff.params.overrides = {{progName, 0.4}};
        gCanvas.w = W; gCanvas.h = H; gCanvas.px.assign((size_t)W * H * 4, 0.f); gCanvas.prims = 0;
        PropSet a; args(a, 0, 0);
        double sc[2] = {1.0 / 1783, 1.0 / 1003};
        pSetDblN((OfxPropertySetHandle)&a, kOfxInteractPropPixelScale, 2, sc);
        entry(kOfxInteractActionDraw, (void*)1, (OfxPropertySetHandle)&a, nullptr);
        // canvas expects pixels: count lit pixels inside the unit square scaled... just report max extent used
        printf("normalised draw prims=%d\n", gCanvas.prims);
        PropSet b; args(b, 0.01 + 0.5 * 0.98, 0.02 + 1.0 * 0.96);
        pSetDblN((OfxPropertySetHandle)&b, kOfxInteractPropPixelScale, 2, sc);
        int hit = entry(kOfxInteractActionPenDown, (void*)1, (OfxPropertySetHandle)&b, nullptr);
        PropSet m; args(m, 0.01 + 0.3 * 0.98, 0.02 + 0.6 * 0.96);
        pSetDblN((OfxPropertySetHandle)&m, kOfxInteractPropPixelScale, 2, sc);
        entry(kOfxInteractActionPenMotion, (void*)1, (OfxPropertySetHandle)&m, nullptr);
        entry(kOfxInteractActionPenUp, (void*)1, (OfxPropertySetHandle)&m, nullptr);
        printf("normalised drag peak hit=%d -> peakPos=%.3f peakAmp=%.3f\n", hit, eff.params.overrides["shakePeakPos"], eff.params.overrides["shakePeakAmp"]);
    }

    PropSet in, out; pSetDbl((OfxPropertySetHandle)&in, kOfxPropTime, 0, 0);
    eff.params.overrides = {{"amplitude", 0}};
    printf("isIdentity(amp=0)=%d\n", pl->mainEntry(kOfxImageEffectActionIsIdentity, &eff, (OfxPropertySetHandle)&in, (OfxPropertySetHandle)&out));
    return 0;
}
