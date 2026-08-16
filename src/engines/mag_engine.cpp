#include "mag_engine.h"
#include "log.h"
#include <cmath>

namespace hsf {

typedef BOOL(WINAPI* MagInitializeFn)();
typedef BOOL(WINAPI* MagSetFullscreenColorEffectFn)(const MagColorEffect* pEffect);
typedef BOOL(WINAPI* MagUninitializeFn)();

bool MagEngine::LoadDll()
{
    if (mod_) return true;
    mod_ = LoadLibraryW(L"Magnification.dll");
    return mod_ != nullptr;
}

bool MagEngine::Initialize()
{
    if (initialized_) return true;
    if (!LoadDll())
    {
        LastError = L"无法加载 Magnification.dll";
        return false;
    }
    auto fn = (MagInitializeFn)GetProcAddress(mod_, "MagInitialize");
    if (!fn || !fn())
    {
        LastError = L"MagInitialize 失败";
        return false;
    }
    initialized_ = true;
    return true;
}

bool MagEngine::Apply(const FilterSettings& s)
{
    if (!Initialize()) return false;
    auto fn = (MagSetFullscreenColorEffectFn)GetProcAddress(mod_, "MagSetFullscreenColorEffect");
    if (!fn) return false;
    MagColorEffect m = BuildMatrix(s);
    return fn(&m) != FALSE;
}

void MagEngine::Reset()
{
    if (!initialized_) return;
    auto fn = (MagSetFullscreenColorEffectFn)GetProcAddress(mod_, "MagSetFullscreenColorEffect");
    if (fn)
    {
        MagColorEffect m = MagColorEffect::Identity();
        fn(&m);
    }
}

void MagEngine::Uninitialize()
{
    if (initialized_)
    {
        Reset();
        auto fn = (MagUninitializeFn)GetProcAddress(mod_, "MagUninitialize");
        if (fn) fn();
        initialized_ = false;
    }
    if (mod_)
    {
        FreeLibrary(mod_);
        mod_ = nullptr;
    }
}

// ---------------- 矩阵构建（移植自旧版 FilterEngine.BuildMatrix） ----------------

static void BuildHueSatMatrix(double hueDeg, double sat,
                              float& m00, float& m01, float& m02,
                              float& m10, float& m11, float& m12,
                              float& m20, float& m21, float& m22)
{
    const float Wr = 0.2126f, Wg = 0.7152f, Wb = 0.0722f;
    float hueRad = (float)(hueDeg * 3.14159265358979323846 / 180.0);
    float c = (float)cos(hueRad);
    float s = (float)sin(hueRad);

    float hu00 = Wr + c * (1.f - Wr) - s * Wr;
    float hu01 = Wg - c * Wg - s * Wg;
    float hu02 = Wb - c * Wb + s * (1.f - Wb);
    float hu10 = Wr - c * Wr + s * Wr;
    float hu11 = Wg + c * (1.f - Wg) - s * Wg;
    float hu12 = Wb - c * Wb - s * Wb;
    float hu20 = Wr - c * Wr - s * (1.f - Wr);
    float hu21 = Wg - c * Wg + s * Wg;
    float hu22 = Wb + c * (1.f - Wb) + s * Wb;

    float invSat = 1.f - (float)sat;
    float s00 = Wr * invSat + (float)sat;
    float s01 = Wg * invSat;
    float s02 = Wb * invSat;
    float s10 = Wr * invSat;
    float s11 = Wg * invSat + (float)sat;
    float s12 = Wb * invSat;
    float s20 = Wr * invSat;
    float s21 = Wg * invSat;
    float s22 = Wb * invSat + (float)sat;

    m00 = s00 * hu00 + s01 * hu10 + s02 * hu20;
    m01 = s00 * hu01 + s01 * hu11 + s02 * hu21;
    m02 = s00 * hu02 + s01 * hu12 + s02 * hu22;
    m10 = s10 * hu00 + s11 * hu10 + s12 * hu20;
    m11 = s10 * hu01 + s11 * hu11 + s12 * hu21;
    m12 = s10 * hu02 + s11 * hu12 + s12 * hu22;
    m20 = s20 * hu00 + s21 * hu10 + s22 * hu20;
    m21 = s20 * hu01 + s21 * hu11 + s22 * hu21;
    m22 = s20 * hu02 + s21 * hu12 + s22 * hu22;
}

static void LightnessGain(double light, float& scale, float& offset)
{
    if (light >= 0)
    {
        scale = 1.f - (float)(std::min)(light, 1.0);
        offset = (float)(std::min)(light, 1.0);
    }
    else
    {
        scale = 1.f + (float)(std::max)(light, -1.0);
        offset = 0.f;
    }
}

MagColorEffect MagEngine::BuildMatrix(const FilterSettings& s)
{
    float brightness = (float)(s.Brightness / 100.0 * 0.5);        // ±0.5 偏移
    float contrast = (float)(s.Contrast / 100.0);                  // 1 = 中性
    float saturation = (float)(s.Saturation / 100.0);              // 1 = 中性
    float highlights = (float)(1.0 + s.Highlights / 100.0 * 0.5);  // 0.5..1.5 增益
    float shadows = (float)(s.Shadows / 100.0 * 0.5);              // ±0.5
    float temp = (float)(s.Temperature / 100.0);                   // -1..1

    float tr = 1.f + 0.18f * temp;
    float tg = 1.f + 0.04f * temp;
    float tb = 1.f - 0.18f * temp;

    const float Wr = 0.2126f, Wg = 0.7152f, Wb = 0.0722f;
    float invSat = 1.f - saturation;

    float matrixScale = (1.f - shadows) * highlights * contrast;

    float s00 = Wr * invSat + saturation;
    float s01 = Wg * invSat;
    float s02 = Wb * invSat;
    float s10 = Wr * invSat;
    float s11 = Wg * invSat + saturation;
    float s12 = Wb * invSat;
    float s20 = Wr * invSat;
    float s21 = Wg * invSat;
    float s22 = Wb * invSat + saturation;

    float e00 = s00 * tr * matrixScale;
    float e01 = s01 * tg * matrixScale;
    float e02 = s02 * tb * matrixScale;
    float e10 = s10 * tr * matrixScale;
    float e11 = s11 * tg * matrixScale;
    float e12 = s12 * tb * matrixScale;
    float e20 = s20 * tr * matrixScale;
    float e21 = s21 * tg * matrixScale;
    float e22 = s22 * tb * matrixScale;
    float be = (1.f - shadows) * highlights * (0.5f * (1.f - contrast) + brightness) + shadows;

    // ---- HSL 部分（先于既有变换应用）----
    // 取“全部(主)”的 HSL（放大镜引擎为整屏线性矩阵，只能近似）
    double hue = s.Hue;
    double sat = s.HslSaturation / 100.0;
    double light = s.Lightness / 100.0;

    float ah00, ah01, ah02, ah10, ah11, ah12, ah20, ah21, ah22, bh;
    BuildHueSatMatrix(hue, sat, ah00, ah01, ah02, ah10, ah11, ah12, ah20, ah21, ah22);
    float ls, lo;
    LightnessGain(light, ls, lo);
    ah00 *= ls; ah01 *= ls; ah02 *= ls;
    ah10 *= ls; ah11 *= ls; ah12 *= ls;
    ah20 *= ls; ah21 *= ls; ah22 *= ls;
    bh = lo;

    // 合成 3×3：Ae × Ah
    float m00 = e00 * ah00 + e01 * ah10 + e02 * ah20;
    float m01 = e00 * ah01 + e01 * ah11 + e02 * ah21;
    float m02 = e00 * ah02 + e01 * ah12 + e02 * ah22;
    float m10 = e10 * ah00 + e11 * ah10 + e12 * ah20;
    float m11 = e10 * ah01 + e11 * ah11 + e12 * ah21;
    float m12 = e10 * ah02 + e11 * ah12 + e12 * ah22;
    float m20 = e20 * ah00 + e21 * ah10 + e22 * ah20;
    float m21 = e20 * ah01 + e21 * ah11 + e22 * ah21;
    float m22 = e20 * ah02 + e21 * ah12 + e22 * ah22;

    float offsetR = be + (e00 + e01 + e02) * bh;
    float offsetG = be + (e10 + e11 + e12) * bh;
    float offsetB = be + (e20 + e21 + e22) * bh;

    MagColorEffect m = MagColorEffect::Identity();
    float* t = m.transform;
    // 按 GDI+ 约定【转置】存储：out = [R,G,B,A,1] × M
    t[0] = m00; t[5] = m01; t[10] = m02;
    t[1] = m10; t[6] = m11; t[11] = m12;
    t[2] = m20; t[7] = m21; t[12] = m22;
    t[18] = 1.f;  // Alpha 直通
    t[20] = offsetR;
    t[21] = offsetG;
    t[22] = offsetB;
    return m;
}

} // namespace hsf
