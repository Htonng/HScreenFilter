#include "gamma_engine.h"

namespace hsf {

static HDC ScreenDC()
{
    return GetDC(nullptr);
}

static float SmoothstepF(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = ClampF(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// sRGB 编解码（标量版）：高光/阴影的增益要做在线性光下才是纯曝光变化。
static float SrgbToLin(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float LinToSrgb(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
}

static float Curve(float v, float tint, float brightness, float contrast, float highlights, float shadows,
                   float darkScene)
{
    // 色温(tint) → 对比度(围绕 0.5) → 亮度(偏移)，与 LUT 路径一致
    float x = contrast * (v * tint) + 0.5f * (1.f - contrast) + brightness;
    if (x < 0.f) x = 0.f;
    if (x > 1.f) x = 1.f;

    // 暗场景优化：与 LUT 路径同一条幂曲线 Y' = Y^(1/(1+k))，白位不动、不裁切。
    // 伽马是逐通道曲线（拿不到亮度），因此这里逐通道应用，方向一致、饱和略增。
    // 注意：伽马引擎没有场景探测，所以没有 LUT 路径那个场景自适应权重 —— 滑块多少就是多少。
    if (darkScene > 0.001f && x > 1e-4f)
    {
        float e = 1.f / (1.f + darkScene * 1.2f);
        float scale = powf(x, e) / x;
        if (scale > 6.f) scale = 6.f;
        x *= scale;
        if (x > 1.f) x = 1.f;
    }

    // 高光 / 阴影：与 LUT 着色器同一套算法 —— 掩码（中间调留死区）+ 线性光曝光增益
    // + 高光软拐点（白位基本不动，高光不再被硬裁成死白，曲线严格单调、无色调反转）。
    // 掩码的域与 LUT 路径一致：高光取线性光值、阴影取感知值。伽马是逐通道曲线（拿不到亮度），
    // 两者都改用通道值本身，方向与 LUT 一致。
    // 旧的 `x*(1-s)+s` 是整幅向白/黑混合：既把整幅画面抬亮（高光裁切），又把暗部颜色洗成灰。
    float lin = SrgbToLin(x);
    float hiMask = SmoothstepF(0.18f, 0.95f, lin);
    float loMask = 1.f - SmoothstepF(0.00f, 0.50f, x);
    float gain = 1.f + hiMask * highlights * 0.5f + loMask * shadows * 0.9f;
    if (fabsf(gain - 1.f) > 1e-4f && x > 1e-6f)
    {
        float boosted = lin * gain;
        if (gain > 1.f)
        {
            float knee = 1.f / gain;   // gain=1 时拐点=1 ⇒ 曲线在 [0,1] 上严格恒等
            if (boosted > knee)
            {
                float t = (boosted - knee) / (1.f - knee);
                boosted = knee + (1.f - knee) * (1.f - expf(-t));
            }
        }
        x = LinToSrgb(boosted);
    }
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
    float highlights = (float)(s.Highlights / 100.0);   // -1..1
    float shadows = (float)(s.Shadows / 100.0);         // -1..1
    float temp = (float)(s.Temperature / 100.0);
    float darkScene = (float)(s.DarkSceneBoost / 100.0);

    float tr = 1.f + 0.18f * temp;
    float tg = 1.f + 0.04f * temp;
    float tb = 1.f - 0.18f * temp;

    Ramp r{};
    for (int i = 0; i < 256; i++)
    {
        float v = i / 255.f;
        r.red[i] = ToRamp(Curve(v, tr, brightness, contrast, highlights, shadows, darkScene));
        r.green[i] = ToRamp(Curve(v, tg, brightness, contrast, highlights, shadows, darkScene));
        r.blue[i] = ToRamp(Curve(v, tb, brightness, contrast, highlights, shadows, darkScene));
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

bool GammaEngine::ApplyIfChanged(const FilterSettings& s)
{
    Ramp want = BuildRamp(s);
    Ramp now{};
    HDC hdc = ScreenDC();
    if (!hdc) return false;
    bool same = false;
    if (GetDeviceGammaRamp(hdc, &now))
        same = memcmp(&want, &now, sizeof(Ramp)) == 0;
    if (same)
    {
        ReleaseDC(nullptr, hdc);
        return true;
    }
    bool ok = SetDeviceGammaRamp(hdc, &want) != FALSE;
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
