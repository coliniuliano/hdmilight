/* Reference check: HyperHDR v22's own math (compiled from its sources, with its
 * hdr_to_srgb/doToneMapping/prepareLCH text included verbatim) versus the C port. */
#include <infinite-color-engine/ColorSpace.h>
#include <infinite-color-engine/YuvConverter.h>
#include <infinite-color-engine/InfiniteHybridRgbInterpolator.h>

using namespace linalg;
using namespace aliases;
using namespace ColorSpaceMath;
namespace ColorSpaceMath { double bt2020_nonlinear_to_linear(double); double bt2020_linear_to_nonlinear(double); }

/* --- upstream structs (BestResult.h, minus Qt/serialize) --- */
struct LchLists { std::list<double4> low; std::list<double4> mid; std::list<double4> high; };
struct BestResult {
    YuvConverter::YUV_COEFS coef = YuvConverter::YUV_COEFS::BT601;
    double4x4 coefMatrix; double2 coefDelta; int coloredAspectMode = 0;
    std::pair<double3, double3> colorAspect; double3 aspect; int bt2020Range = 0; int altConvert = 0;
    double3x3 altPrimariesToSrgb; HDR_GAMMA gamma = HDR_GAMMA::PQ; double gammaHLG = 0; double nits = 0;
    bool lchEnabled = false; LchLists lchPrimaries;
    struct Signal { YuvConverter::COLOR_RANGE range = YuvConverter::COLOR_RANGE::FULL; double yRange = 0, upYLimit = 0, downYLimit = 0, yShift = 0; bool isSourceP010 = false; } signal;
};
struct CapturedColors {
    static void correctYRange(double3& yuv, double yRange, double upYLimit, double downYLimit, double yShift)
    { yuv.x = ((yuv.x - downYLimit) / (upYLimit - downYLimit)) * yRange + yShift; }   /* BoardUtils.cpp:457 */
};
static void unpackP010(double3&) {}   /* not exercised */

#include "ref_tonemapping.inc"   /* doToneMapping, verbatim */
#include "ref_hdr_to_srgb.inc"   /* hdr_to_srgb, verbatim */
#include "ref_preplch.inc"       /* prepareLCH, verbatim */

/* --- InfiniteProcessing: the header is Qt-bound, so mirror its members here and compile the upstream member
 *     functions verbatim (ref_processing.inc = InfiniteProcessing.cpp lines 240-668). --- */
#include <optional>
#include <array>
#include <image/ColorRgb.h>
#define Info(...)  do {} while (0)
#define Error(...) do {} while (0)
enum class CalibrationMode { None, Matrix, Lut };
struct CalibrationSnapshot { CalibrationMode mode = CalibrationMode::None; std::vector<float3> lut; float3x3 primary_calib_matrix{}; };
class CalibrationData {
public:
    void update(std::vector<float3>&& lut, float3x3&& matrix, CalibrationMode mode) { _snapshot = std::make_shared<CalibrationSnapshot>(CalibrationSnapshot{ mode, std::move(lut), std::move(matrix) }); }
    std::shared_ptr<const CalibrationSnapshot> getSnapshot() const { return _snapshot; }
private:
    std::shared_ptr<CalibrationSnapshot> _snapshot{ std::make_shared<CalibrationSnapshot>() };
};
namespace { float3 to_float3(const ColorRgb& c) { return { c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f }; } }
class InfiniteProcessing {
public:
    enum class TemperaturePreset { Disabled, Warm, Neutral, Cold, Custom };
    static constexpr int LUT_SIZE = 512, LUT_DIMENSION = 9;
    std::string _log;
    std::optional<float3> _temperature_tint;
    std::optional<float> _gamma, _brightness, _saturation, _scaleOutput, _powerLimit, _minimalBacklight;
    std::optional<bool> _coloredBacklight;
    std::array<float, LUT_SIZE> _user_gamma_lut{};
    CalibrationData _colorCalibrationData;
    std::optional<float> getMinimalBacklight();
    void setMinimalBacklight(float, bool);
    void applyMinimalBacklight(float3&) const;
    void generateColorspace(bool, ColorRgb, ColorRgb, ColorRgb, ColorRgb, ColorRgb, ColorRgb, ColorRgb, ColorRgb);
    void calibrateColorInColorspace(const std::shared_ptr<const CalibrationSnapshot>&, float3&) const;
    void generateUserGamma(float);
    void applyUserGamma(float3&) const;
    void setTemperature(TemperaturePreset, float3);
    void applyTemperature(float3&) const;
    void setBrightnessAndSaturation(float, float);
    void applyBrightnessAndSaturation(float3&) const;
    void setScaleOutput(float);
    void applyScaleOutput(float3&) const;
    void setPowerLimit(float);
    void applyPowerLimit(std::vector<float3>&) const;
    std::shared_ptr<const CalibrationSnapshot> getColorspaceCalibrationSnapshot();
    /* InfiniteProcessing.h: 512-entry sRGB encode with lerp */
    static float3 srgbLinearToNonlinear(const float3& color) {
        static std::array<float, LUT_SIZE> lut = [] { std::array<float, LUT_SIZE> l{}; for (int i = 0; i < LUT_SIZE; i++) { float v = (float)i / (LUT_SIZE - 1); l[i] = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; } return l; }();
        float3 r; for (int i = 0; i < 3; ++i) { float val = std::max(0.0f, std::min(color[i], 1.0f)); float pos = val * (LUT_SIZE - 1); int i0 = (int)pos, i1 = std::min(i0 + 1, LUT_SIZE - 1); r[i] = linalg::lerp(lut[i0], lut[i1], pos - i0); } return r; }
    /* applyyAllProcessingSteps, enabled branch (InfiniteProcessing.cpp 185-198) */
    void run(std::vector<float3>& v) {
        auto cal = getColorspaceCalibrationSnapshot();
        for (auto& color : v) { applyTemperature(color); calibrateColorInColorspace(cal, color); applyScaleOutput(color); color = srgbLinearToNonlinear(color); applyUserGamma(color); applyBrightnessAndSaturation(color); applyMinimalBacklight(color); }
        applyPowerLimit(v);
    }
};
#include "ref_processing.inc"

extern "C" {
#include "hl_colorspace.h"
#include "hl_output.h"
#include "hl_yuv.h"
#include "hl_tonemap.h"
#include "hl_smooth.h"
}

static int fails = 0;
#define REPORT(name, maxdiff, tol) do { printf("%-44s max|diff| = %-12.3g %s\n", name, (double)(maxdiff), ((maxdiff) <= (tol)) ? "ok" : "MISMATCH"); if ((maxdiff) > (tol)) fails++; } while (0)
static double dmax(double a, double b) { return a > b ? a : b; }

static void default_model(BestResult& br)      /* LutCalibrator::CreateDefaultLut */
{
    br.coef = YuvConverter::YUV_COEFS(1);
    br.coefMatrix = double4x4{ {1.16438356164, 1.16438356164, 1.16438356164, 0}, {-4.43119136738e-17, -0.387076923077, 2.02178571429, 0}, {1.58691964286, -0.821942994505, -1.77247654695e-16, 0}, {-0.869630789302, 0.53382122535, -1.08791650359, 1} };
    br.coefDelta = double2{ 0.004, -0.002 };
    br.coloredAspectMode = 3;
    br.colorAspect = std::pair<double3, double3>(double3{ 1.08301468119, 1.00959155269, 1.10863646026 }, double3{ 1.06441799482, 1.00700595321, 1.06831049137 });
    br.aspect = double3{ 0.9925, 1, 1.035 };
    br.bt2020Range = 0;
    br.altConvert = 1;
    br.altPrimariesToSrgb = double3x3{ {1.62995144161, -0.159968936426, -0.0191389994477}, {-0.556261837808, 1.17281602107, -0.104271698501}, {-0.0736896038051, -0.0128470846442, 1.12341069795} };
    br.gamma = HDR_GAMMA(0);
    br.gammaHLG = 0.0;
    br.lchEnabled = false;
    br.nits = 229.625687;
    br.signal.range = YuvConverter::COLOR_RANGE(2);
    br.signal.yRange = 0.858824; br.signal.upYLimit = 0.572549; br.signal.downYLimit = 0.062745; br.signal.yShift = 0.062745;
    br.signal.isSourceP010 = false;
}

/* CreateLutWorker::run semantics for one entry */
static double3 ref_eval(const BestResult& br, const YuvConverter& yc, byte3 YUV, int phase)
{
    double3 yuv = static_cast<double3>(YUV) / 255.0;
    if (phase == 0) {
        yuv = yc.toYuv(br.signal.range, br.coef, yuv);
        YUV = round_to_0_255<byte3>(yuv * 255);
    }
    yuv = hdr_to_srgb(&yc, yuv, byte2(YUV.y, YUV.z), br.aspect, br.coefMatrix, br.gamma, br.gammaHLG, br.nits,
                      br.altConvert, br.altPrimariesToSrgb, br.bt2020Range, br.signal, br.coloredAspectMode, br.colorAspect);
    if (br.lchEnabled) doToneMapping(br.lchPrimaries, yuv);
    return yuv;
}

static void copy_model(const BestResult& br, hl_model* m)
{
    hl_model_default(m);
    m->coef = (hl_yuv_coef_t)br.coef;
    for (int c = 0; c < 4; c++) { hl_f3* d = c == 0 ? &m->coefMatrix.c0 : c == 1 ? &m->coefMatrix.c1 : c == 2 ? &m->coefMatrix.c2 : &m->coefMatrix.c3;
        *d = hl_f3_make((float)br.coefMatrix[c].x, (float)br.coefMatrix[c].y, (float)br.coefMatrix[c].z); }
    m->coloredAspectMode = br.coloredAspectMode;
    m->colorAspectFirst = hl_f3_make((float)br.colorAspect.first.x, (float)br.colorAspect.first.y, (float)br.colorAspect.first.z);
    m->colorAspectSecond = hl_f3_make((float)br.colorAspect.second.x, (float)br.colorAspect.second.y, (float)br.colorAspect.second.z);
    m->aspect = hl_f3_make((float)br.aspect.x, (float)br.aspect.y, (float)br.aspect.z);
    m->bt2020Range = br.bt2020Range; m->altConvert = br.altConvert;
    for (int c = 0; c < 3; c++) { hl_f3* d = c == 0 ? &m->altPrimariesToSrgb.c0 : c == 1 ? &m->altPrimariesToSrgb.c1 : &m->altPrimariesToSrgb.c2;
        *d = hl_f3_make((float)br.altPrimariesToSrgb[c].x, (float)br.altPrimariesToSrgb[c].y, (float)br.altPrimariesToSrgb[c].z); }
    m->gamma = (hl_gamma_t)(int)br.gamma; m->gammaHLG = (float)br.gammaHLG; m->nits = (float)br.nits;
    m->lchEnabled = br.lchEnabled;
    auto cp = [](const std::list<double4>& src, hl_f4* dst, int* n) { *n = 0; for (auto& v : src) { dst[*n] = hl_f4{ (float)v.x, (float)v.y, (float)v.z, (float)v.w }; (*n)++; } };
    cp(br.lchPrimaries.low, m->lch.low, &m->lch.n_low); cp(br.lchPrimaries.mid, m->lch.mid, &m->lch.n_mid); cp(br.lchPrimaries.high, m->lch.high, &m->lch.n_high);
    m->signal.range = (hl_range_t)br.signal.range;
    m->signal.yRange = (float)br.signal.yRange; m->signal.upYLimit = (float)br.signal.upYLimit; m->signal.downYLimit = (float)br.signal.downYLimit; m->signal.yShift = (float)br.signal.yShift;
}

static void compare_tonemap(const char* label, const BestResult& br, int step)
{
    YuvConverter yc; hl_model m; copy_model(br, &m);
    double maxf = 0; long n = 0, over1 = 0, over2 = 0, nanref = 0; int maxu8 = 0;
    for (int phase = 1; phase >= 0; phase--) {
        for (int y = 0; y < 256; y += step) for (int u = 0; u < 256; u += step) for (int v = 0; v < 256; v += step) {
            double3 r = ref_eval(br, yc, byte3(y, u, v), phase);
            hl_f3 mine = phase == 1 ? hl_tonemap_yuv8(&m, y, u, v) : hl_tonemap_rgb8(&m, y, u, v);
            if (std::isnan(r.x) || std::isnan(r.y) || std::isnan(r.z)) { nanref++; continue; }   /* upstream lround(NaN) is UB: not comparable */
            /* HyperHDR's LUT entry is round_to_0_255(srgb * 255): clamp then round. Compare there. */
            byte3 rb = round_to_0_255<byte3>(r * 255.0);
            int mb[3] = { (int)lroundf(hl_clampf(mine.x, 0, 1) * 255), (int)lroundf(hl_clampf(mine.y, 0, 1) * 255), (int)lroundf(hl_clampf(mine.z, 0, 1) * 255) };
            int du8 = std::max({ abs((int)rb.x - mb[0]), abs((int)rb.y - mb[1]), abs((int)rb.z - mb[2]) });
            double d = du8 / 255.0;
            if (d > maxf) maxf = d;
            if (du8 > maxu8) maxu8 = du8; if (du8 > 1) over1++; if (du8 > 2) over2++; n++;
        }
    }
    char name[128]; snprintf(name, sizeof name, "%s (%ld pts, 8-bit max %d, >1: %ld, >2: %ld, ref NaN skipped: %ld)", label, n, maxu8, over1, over2, nanref);
    REPORT(name, maxf, 1.0 / 255.0);
}

int main()
{
    printf("HyperHDR v22 reference vs hdmilight-v3 core\n");

    /* 1. transfer functions and colour spaces */
    { double m1 = 0, m2 = 0, m3 = 0, m4 = 0, m5 = 0;
      for (int i = 0; i <= 1000; i++) { double v = i / 1000.0;
        m1 = dmax(m1, fabs(srgb_nonlinear_to_linear(v) - hl_srgb_to_linear((float)v)));
        m1 = dmax(m1, fabs(srgb_linear_to_nonlinear(v) - hl_linear_to_srgb((float)v)));
        m2 = dmax(m2, fabs(bt2020_nonlinear_to_linear(v) - hl_bt2020_to_linear((float)v)));
        m2 = dmax(m2, fabs(bt2020_linear_to_nonlinear(v) - hl_linear_to_bt2020((float)v)));
        m3 = dmax(m3, fabs(PQ_ST2084(10000.0 / 229.625687, v) - hl_pq_st2084(10000.0f / 229.625687f, (float)v)) / 50.0);   /* relative to ~50 range */
        m4 = dmax(m4, fabs(inverse_OETF_HLG(v) - hl_hlg_inverse_oetf((float)v)));
        double3 o = OOTF_HLG(double3(v, v * 0.5, v * 0.2), 1.2); hl_f3 p = hl_hlg_ootf(hl_f3_make(v, v * 0.5f, v * 0.2f), 1.2f);
        m4 = dmax(m4, dmax(fabs(o.x - p.x), dmax(fabs(o.y - p.y), fabs(o.z - p.z))));
        double3 rgb(v, 1 - v, v * v); double3 xyz = from_sRGB_to_XYZ(rgb); hl_f3 hx = hl_srgb_to_xyz(hl_f3_make(rgb.x, rgb.y, rgb.z));
        m5 = dmax(m5, dmax(fabs(xyz.x - hx.x), dmax(fabs(xyz.y - hx.y), fabs(xyz.z - hx.z))));
        double3 lch = xyz_to_lch(xyz * 100.0); hl_f3 hl = hl_xyz_to_lch(hl_f3_scale(hx, 100.f));
        m5 = dmax(m5, dmax(fabs(lch.x - hl.x), dmax(fabs(lch.y - hl.y), fabs(lch.z - hl.z) / 360.0)));
        double3 back = from_XYZ_to_sRGB(lch_to_xyz(lch) / 100.0); hl_f3 hb = hl_xyz_to_srgb(hl_f3_scale(hl_lch_to_xyz(hl), 0.01f));
        m5 = dmax(m5, dmax(fabs(back.x - hb.x), dmax(fabs(back.y - hb.y), fabs(back.z - hb.z))));
        double3 b7 = from_BT2020_to_BT709(rgb); hl_f3 h7 = hl_bt2020_to_bt709(hl_f3_make(rgb.x, rgb.y, rgb.z));
        m5 = dmax(m5, dmax(fabs(b7.x - h7.x), dmax(fabs(b7.y - h7.y), fabs(b7.z - h7.z))));
      }
      REPORT("sRGB transfer", m1, 1e-5); REPORT("BT.2020 transfer", m2, 1e-5); REPORT("PQ ST.2084 (rel. to 50)", m3, 5e-5);
      REPORT("HLG inverse OETF + OOTF", m4, 1e-5); REPORT("XYZ / Lab / LCH / BT2020->709", m5, 2e-4);
      double m6 = 0; for (int i = 0; i < 256; i++) m6 = dmax(m6, fabs(srgb_nonlinear_to_linear(i / 255.0) * 65535.0 - hl_srgb8_to_linear16((uint8_t)i)));
      REPORT("sRGB8 -> linear16 table (counts)", m6, 1.0); }

    /* 2. YUV matrices */
    { YuvConverter yc; double m = 0;
      for (int c = 0; c < 4; c++) for (int r = 1; r <= 2; r++) {
        double4x4 ref = yc.create_yuv_to_rgb_matrix((YuvConverter::COLOR_RANGE)r, yc.getCoef((YuvConverter::YUV_COEFS)c).x, yc.getCoef((YuvConverter::YUV_COEFS)c).y);
        hl_m34 mine = hl_yuv_yuv2rgb_matrix((hl_range_t)r, (hl_yuv_coef_t)c);
        const hl_f3* cols[4] = { &mine.c0, &mine.c1, &mine.c2, &mine.c3 };
        for (int k = 0; k < 4; k++) m = dmax(m, dmax(fabs(ref[k].x - cols[k]->x), dmax(fabs(ref[k].y - cols[k]->y), fabs(ref[k].z - cols[k]->z))));
        for (int i = 0; i <= 20; i++) { double3 in(i / 20.0, 1 - i / 20.0, 0.3);
          double3 a = yc.toYuv((YuvConverter::COLOR_RANGE)r, (YuvConverter::YUV_COEFS)c, in); hl_f3 b = hl_rgb_to_yuv((hl_range_t)r, (hl_yuv_coef_t)c, hl_f3_make(in.x, in.y, in.z));
          m = dmax(m, dmax(fabs(a.x - b.x), dmax(fabs(a.y - b.y), fabs(a.z - b.z)))); }
      }
      REPORT("YuvConverter rgb<->yuv (all coefs/ranges)", m, 1e-5); }

    /* 3. tone mapping: default model, then variants exercising every branch */
    { BestResult br; default_model(br); compare_tonemap("hdr_to_srgb PQ default model", br, 5); }
    { BestResult br; default_model(br); br.altConvert = 0; br.coloredAspectMode = 1; br.bt2020Range = 1; compare_tonemap("PQ, BT2020->709 path, aspect 1, bt2020Range", br, 8); }
    { BestResult br; default_model(br); br.coloredAspectMode = 2; br.signal.range = YuvConverter::COLOR_RANGE::FULL; compare_tonemap("PQ full range, aspect mode 2", br, 8); }
    { BestResult br; default_model(br); br.gamma = HDR_GAMMA::HLG; br.gammaHLG = 1.2; br.nits = 1.0; compare_tonemap("HLG", br, 8); }
    { BestResult br; default_model(br); br.gamma = HDR_GAMMA::sRGB; compare_tonemap("sRGB (Y-range correction path)", br, 8); }
    { BestResult br; default_model(br); br.gamma = HDR_GAMMA::BT2020inSRGB; compare_tonemap("BT2020 in sRGB", br, 8); }
    { BestResult br; default_model(br); br.gamma = HDR_GAMMA::PQinSRGB; compare_tonemap("PQ in sRGB", br, 8); }
    { /* LCH correction: synthetic primaries through upstream prepareLCH, then the same lists on both sides */
      BestResult br; default_model(br); br.lchEnabled = true;
      std::list<std::list<std::pair<double3, double3>>> prim;
      for (int band = 0; band < 3; band++) {
        std::list<std::pair<double3, double3>> l; double scale = 0.5 + 0.25 * band;
        double3 cols[6] = { {1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1},{1,0,1} };
        for (auto& c : cols) { double3 org = xyz_to_lch(from_sRGB_to_XYZ(c * scale) * 100.0);
          double3 captured = c * scale * double3(0.9, 1.05, 0.97) + double3(0.02, 0.0, 0.03);   /* a slightly wrong capture */
          l.push_back({ org, clamp01(captured) }); }
        prim.push_back(l);
      }
      br.lchPrimaries = prepareLCH(prim);
      compare_tonemap("PQ + LCH correction lists", br, 8); }

    /* 4. smoothing: identical scenario, step by step */
    { InfiniteHybridRgbInterpolator ref; ref.setSpringiness(150.f, 26.f); ref.setTransitionDuration(150.f);
      hl_smooth mine; hl_smooth_init(&mine, 2);
      std::vector<float3> A = { {0,0,0}, {1,0.5f,0.2f} }, B = { {1,1,1}, {0.1f,0.1f,0.9f} }, C = { {0.3f,0.7f,0.2f}, {0,0,0} };
      hl_f3 a[2] = { {0,0,0}, {1,0.5f,0.2f} }, b[2] = { {1,1,1}, {0.1f,0.1f,0.9f} }, c[2] = { {0.3f,0.7f,0.2f}, {0,0,0} };
      ref.resetToColors(A, 0.f); hl_smooth_set_targets(&mine, a, 0);
      ref.setTargetColors(std::vector<float3>(B), 10, false); hl_smooth_set_targets(&mine, b, 10);
      double m = 0; bool retargeted = false;
      for (long long t = 20; t <= 2000; t += 20) {
        if (t >= 300 && !retargeted) { ref.setTargetColors(std::vector<float3>(C), t, false); hl_smooth_set_targets(&mine, c, t); retargeted = true; }
        ref.updateCurrentColors(t, 0.f); hl_smooth_update(&mine, t, 0.f);
        auto cur = ref.getCurrentColors(0.f); hl_f3 out[2]; hl_smooth_get(&mine, 0.f, out);
        for (int i = 0; i < 2; i++) m = dmax(m, dmax(fabs((*cur)[i].x - out[i].x), dmax(fabs((*cur)[i].y - out[i].y), fabs((*cur)[i].z - out[i].z))));
      }
      REPORT("HybridRgb spring-damper trajectory (2 LEDs, retarget)", m, 1e-5);
      hl_smooth_free(&mine); }

    /* 5. output processing: every stage on, 8-colour LUT calibration, against the verbatim upstream functions */
    { struct Case { const char *name; bool classic; ColorRgb t[8]; InfiniteProcessing::TemperaturePreset tp; float3 tint; float scale, gamma, lum, sat, bl; bool blc; float pl; } cases[] = {
        { "output: HyperHDR defaults (gamma 1.5, backlight)", true, { {255,0,0},{0,255,0},{0,0,255},{0,255,255},{255,0,255},{255,255,0},{255,255,255},{0,0,0} }, InfiniteProcessing::TemperaturePreset::Disabled, {1,1,1}, 1.f, 1.5f, 1.f, 1.f, 0.0039f, true, 1.f },
        { "output: matrix calibration + warm + scale",       true, { {255,40,10},{30,255,20},{0,20,255},{0,255,255},{255,0,255},{255,255,0},{255,255,255},{0,0,0} }, InfiniteProcessing::TemperaturePreset::Warm, {1,1,1}, 1.2f, 2.2f, 1.f, 1.f, 0.f, true, 1.f },
        { "output: 8-colour LUT + custom tint + HSV + grey backlight + power", false, { {250,10,0},{20,240,30},{10,0,230},{0,230,240},{240,0,250},{250,240,0},{240,230,220},{2,2,4} }, InfiniteProcessing::TemperaturePreset::Custom, {0.95f,1.f,0.9f}, 0.9f, 1.3f, 1.1f, 0.8f, 0.02f, false, 0.6f },
      };
      for (auto& cs : cases) {
        InfiniteProcessing ref; ref.setTemperature(cs.tp, cs.tint); ref.generateColorspace(cs.classic, cs.t[0], cs.t[1], cs.t[2], cs.t[3], cs.t[4], cs.t[5], cs.t[6], cs.t[7]);
        ref.setScaleOutput(cs.scale); ref.generateUserGamma(cs.gamma); ref.setBrightnessAndSaturation(cs.lum, cs.sat); ref.setMinimalBacklight(cs.bl, cs.blc); ref.setPowerLimit(cs.pl);
        hl_output_cfg oc; hl_output_cfg_default(&oc); oc.temp = (hl_temp_preset)(cs.tp == InfiniteProcessing::TemperaturePreset::Disabled ? 0 : cs.tp == InfiniteProcessing::TemperaturePreset::Cold ? 1 : cs.tp == InfiniteProcessing::TemperaturePreset::Neutral ? 2 : cs.tp == InfiniteProcessing::TemperaturePreset::Warm ? 3 : 4);
        oc.temp_tint = hl_f3_make(cs.tint.x, cs.tint.y, cs.tint.z); oc.classic = cs.classic;
        for (int i = 0; i < 8; i++) { oc.cal[i][0] = cs.t[i].red; oc.cal[i][1] = cs.t[i].green; oc.cal[i][2] = cs.t[i].blue; }
        oc.scale = cs.scale; oc.gamma = cs.gamma; oc.luminance = cs.lum; oc.saturation = cs.sat; oc.backlight = cs.bl; oc.backlight_colored = cs.blc; oc.power_limit = cs.pl;
        hl_output mine; hl_output_init(&mine, &oc);
        const int N = 17 * 17 * 17; std::vector<float3> rv; rv.reserve(N); std::vector<hl_f3> mv; mv.reserve(N);
        for (int r = 0; r < 17; r++) for (int g = 0; g < 17; g++) for (int b = 0; b < 17; b++) { rv.push_back({ r / 16.f, g / 16.f, b / 16.f }); mv.push_back(hl_f3_make(r / 16.f, g / 16.f, b / 16.f)); }
        ref.run(rv); hl_output_process(&mine, mv.data(), N);
        double m = 0; for (int i = 0; i < N; i++) { m = dmax(m, fabs(rv[i].x - mv[i].x)); m = dmax(m, fabs(rv[i].y - mv[i].y)); m = dmax(m, fabs(rv[i].z - mv[i].z)); }
        REPORT(cs.name, m * 255.0, 1.0);   /* in 8-bit counts; the only difference is the 512-entry sRGB LUT upstream vs pow() here */
      } }

    printf("%s\n", fails ? "REFERENCE CHECK FAILED" : "all reference checks passed");
    return fails ? 1 : 0;
}
