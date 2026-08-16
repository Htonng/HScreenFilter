#include "gamma_engine.h"

namespace hsf {

static HDC ScreenDC()
{
    return GetDC(nullptr);
}

static float Curve(float v, float tint, float brightness, float contrast, float highlights, float shadows)
{
    // 与颜色矩阵引擎保持一致的语义：
    //   色温(tint) → 对比度(围绕 0.5) → 亮度(偏移) → 亮部(增益) → 暗部(保持白色的提亮)
    float x = contrast * (v * tint) + 0.5f * (1.f - contrast) + brightness;
    x = x * highlights;
    x = x * (1.f - shadows) + shadows;
    return ClampF(x, 0.f, 1.f);
}

static unsigned short ToRamp(float v)
{
    int iv = (int)(v * 65535.f);
    return (unsigned short)Clamp((double)iv, 0.0, 65535.0);
}

GammaEngine::Ramp GammaEngine::LinearRamp()
{
    Ramp r{};
    for (int i = 0; i < 256; i++)
    {
        unsigned short v = (unsigned short)(i * 257);
        r.red[i] = v; r.green[i] = v; r.blue[i] = v;
    }
    return r;
}

GammaEngine::Ramp GammaEngine::BuildRamp(const FilterSettings& s)
{
    float brightness = (float)(s.Brightness / 100.0 * 0.5);
    float contrast = (float)(s.Contrast / 100.0);
    float highlights = (float)(1.0 + s.Highlights / 100.0 * 0.5);
    float shadows = (float)(s.Shadows / 100.0 * 0.5);
    float temp = (float)(s.Temperature / 100.0);

    float tr = 1.f + 0.18f * temp;
    float tg = 1.f + 0.04f * temp;
    float tb = 1.f - 0.18f * temp;

    Ramp r{};
    for (int i = 0; i < 256; i++)
    {
        float v = i / 255.f;
        r.red[i] = ToRamp(Curve(v, tr, brightness, contrast, highlights, shadows));
        r.green[i] = ToRamp(Curve(v, tg, brightness, contrast, highlights, shadows));
        r.blue[i] = ToRamp(Curve(v, tb, brightness, contrast, highlights, shadows));
    }
    return r;
}

bool GammaEngine::Test()
{
    HDC hdc = ScreenDC();
    if (!hdc) return false;
    Ramp r = LinearRamp();
    bool ok = GetDeviceGammaRamp(hdc, &r) != FALSE;
    ReleaseDC(nullptr, hdc);
    return ok;
}

bool GammaEngine::Apply(const FilterSettings& s)
{
    Ramp r = BuildRamp(s);
    HDC hdc = ScreenDC();
    if (!hdc) return false;
    bool ok = SetDeviceGammaRamp(hdc, &r) != FALSE;
    ReleaseDC(nullptr, hdc);
    return ok;
}

bool GammaEngine::Reset()
{
    Ramp r = LinearRamp();
    HDC hdc = ScreenDC();
    if (!hdc) return false;
    bool ok = SetDeviceGammaRamp(hdc, &r) != FALSE;
    ReleaseDC(nullptr, hdc);
    return ok;
}

} // namespace hsf
