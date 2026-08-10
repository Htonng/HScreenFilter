using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.Graphics.Canvas;
using Microsoft.Graphics.Canvas.Effects;
using Windows.Foundation;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>
/// 逐像素 HSL 分色系滤镜着色器。
///
/// 与"整屏单个颜色矩阵"不同，它由 GPU 对每个像素独立计算：
///   1) 把 RGB 转成 HSL；
///   2) 先施加主色系（全部）的 色相/饱和度/明度；
///   3) 再按 红/黄/绿/青/蓝/品红 六个色系各自的软掩码（基于色相距离的平滑衰减），
///      分别施加各自的 色相/饱和度/明度 调整，互不干扰；
///   4) 最后做 色温/鲜艳度/对比度/亮度/亮部/暗部 等基础调节（与颜色矩阵引擎语义一致）。
///
/// 这是真正意义上的"分色系 HSL 调整"，也是本文件引入 Win2D PixelShaderEffect
/// （自定义逐像素着色器）的原因。
/// </summary>
public static class SelectiveHslShader
{
    // 着色器源码（编译目标 ps_4_0）。注意：cbuffer 只能有一个且绑定到 b0，且不能用 struct。
    public const string Source = """
// 逐像素 HSL 分色系滤镜着色器（Win2D PixelShaderEffect / D2D 像素着色器）
Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

cbuffer Params : register(b0)
{
    float MasterHue;      // 主色系（全部）色相旋转（度）
    float MasterSat;      // 主色系饱和度缩放（1=中性）
    float MasterLight;    // 主色系明度偏移（-1..1）
    float GlobalSat;      // 鲜艳度（1=中性）
    float Temperature;    // 色温（-1..1）
    float Contrast;       // 对比度（1=中性）
    float Brightness;     // 亮度（-1..1）
    float Highlights;     // 亮部（-1..1）
    float Shadows;        // 暗部（-1..1）
    float HueR, SatR, LightR;    // 红
    float HueO, SatO, LightO;    // 橙
    float HueY, SatY, LightY;    // 黄
    float HueG, SatG, LightG;    // 绿
    float HueC, SatC, LightC;    // 青
    float HueB, SatB, LightB;    // 蓝
    float HueP, SatP, LightP;    // 紫
    float HueM, SatM, LightM;    // 品红
};

float wrapHue(float h)
{
    h = fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    return h;
}

void rgbToHsl(float3 c, out float h, out float s, out float l)
{
    float maxc = max(c.r, max(c.g, c.b));
    float minc = min(c.r, min(c.g, c.b));
    l = (maxc + minc) * 0.5;
    float d = maxc - minc;
    if (d < 1e-4)
    {
        h = 0.0;
        s = 0.0;
        return;
    }
    s = d / max(1.0 - abs(2.0 * l - 1.0), 1e-4);
    if (maxc == c.r)
        h = (c.g - c.b) / d;
    else if (maxc == c.g)
        h = (c.b - c.r) / d + 2.0;
    else
        h = (c.r - c.g) / d + 4.0;
    h = wrapHue(h * 60.0);
}

float3 hslToRgb(float h, float s, float l)
{
    float c = (1.0 - abs(2.0 * l - 1.0)) * s;
    float hp = wrapHue(h) / 60.0;
    float x = c * (1.0 - abs(fmod(hp, 2.0) - 1.0));
    float3 rgb;
    if (hp < 1.0) rgb = float3(c, x, 0.0);
    else if (hp < 2.0) rgb = float3(x, c, 0.0);
    else if (hp < 3.0) rgb = float3(0.0, c, x);
    else if (hp < 4.0) rgb = float3(0.0, x, c);
    else if (hp < 5.0) rgb = float3(x, 0.0, c);
    else rgb = float3(c, 0.0, x);
    float m = l - c * 0.5;
    return rgb + m;
}

// —— OKLab/OKLCH（Björn Ottosson 2020 官方参考实现，含健壮性处理）——
// 算法与官方一致：sRGB 线性化 → LMS → 立方根 → OKLab → 极坐标(OKLCH)。
// 健壮性：消色像素（C≈0）不调 atan2(0,0)（HLSL 未定义，部分 GPU 返回 NaN → 全白）；
// pow 底数用 max(x, 1e-8)，避免 pow(0, 小数) 产生 NaN。
float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(lo, hi, step(0.04045, c));
}

float3 LinearToSrgb(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 1e-8), 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, c));
}

void RgbToOklch(float3 c, out float L, out float C, out float H)
{
    float3 lin = SrgbToLinear(c);
    float l = 0.4122214708 * lin.r + 0.5363325363 * lin.g + 0.0514459929 * lin.b;
    float m = 0.2119034982 * lin.r + 0.6806995451 * lin.g + 0.1073969566 * lin.b;
    float s = 0.0883024619 * lin.r + 0.2817188376 * lin.g + 0.6299787005 * lin.b;
    l = pow(max(l, 1e-8), 1.0 / 3.0);
    m = pow(max(m, 1e-8), 1.0 / 3.0);
    s = pow(max(s, 1e-8), 1.0 / 3.0);
    float La = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    float a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    float b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    L = La;
    C = sqrt(a * a + b * b);
    // 消色像素避免 atan2(0,0)（HLSL 未定义）
    H = C < 1e-5 ? 0.0 : wrapHue(atan2(b, a) * 57.2957795);
}

float3 OklchToRgb(float L, float C, float H)
{
    float hr = H * 0.0174532925; // π/180
    float a = C * cos(hr);
    float b = C * sin(hr);
    float l_ = L + 0.3963377774 * a + 0.2158037573 * b;
    float m_ = L - 0.1055613458 * a - 0.0638541728 * b;
    float s_ = L - 0.0894841775 * a - 1.2914855480 * b;
    l_ = l_ * l_ * l_;
    m_ = m_ * m_ * m_;
    s_ = s_ * s_ * s_;
    float r =  4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_;
    float g = -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_;
    float b2 = -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_;
    return LinearToSrgb(float3(r, g, b2));
}

// 色系软掩码：以参考色相为中心，半宽 60°，平滑衰减到 0。
// 8 个色系并非等距分布（相邻最小 30°、最大 60°），60° 半宽无死区，
// 重叠区大、相邻色系调整互相融合，色彩过渡更柔和（滤镜调狠也不易出棱角分明的色块）。
float hueMask(float h, float refHue)
{
    float a = abs(h - refHue);
    a = min(a, 360.0 - a);
    float t = a / 60.0;
    float w = saturate(1.0 - t);
    return w * w * (3.0 - 2.0 * w); // smoothstep
}

// 该色系是否有实际调整（中性时权重归零，避免稀释其它色系）
float isActive(float h, float sat, float light)
{
    return (abs(h) > 0.01 || abs(sat - 1.0) > 0.001 || abs(light) > 0.001) ? 1.0 : 0.0;
}

// 明度掩码：试验阶段与色相/饱和度同为 60°（可随时改回 30° 收紧亮暗过渡）。
float hueMaskNarrow(float h, float refHue)
{
    float a = abs(h - refHue);
    a = min(a, 360.0 - a);
    float t = a / 60.0;
    float w = saturate(1.0 - t);
    return w * w * (3.0 - 2.0 * w); // smoothstep
}

// 该色系是否有明度调整（亮度掩码只看明度，避免被色相/饱和度调整连带放大）
float isActiveLight(float light)
{
    return abs(light) > 0.001 ? 1.0 : 0.0;
}

// 抖动噪声：按屏幕像素坐标的确定性伪随机（0..1），同一像素每帧一致，不会闪烁/蠕变。
float DitherNoise(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 texel : TEXCOORD0) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, texel.xy);
    float3 c = color.rgb;

    float h, s, l;
    rgbToHsl(c, h, s, l);

    // 主色系（全部）色相：先旋转再用于掩码选择（所见即所选）
    h = wrapHue(h + MasterHue);

    // 八个分色系的软掩码（用主色系处理后的色相选择，所见即所选）
    float wR = hueMask(h, 0.0)   * isActive(HueR, SatR, LightR);
    float wO = hueMask(h, 30.0)  * isActive(HueO, SatO, LightO);
    float wY = hueMask(h, 60.0)  * isActive(HueY, SatY, LightY);
    float wG = hueMask(h, 120.0) * isActive(HueG, SatG, LightG);
    float wC = hueMask(h, 180.0) * isActive(HueC, SatC, LightC);
    float wB = hueMask(h, 240.0) * isActive(HueB, SatB, LightB);
    float wP = hueMask(h, 270.0) * isActive(HueP, SatP, LightP);
    float wM = hueMask(h, 300.0) * isActive(HueM, SatM, LightM);

    // 明度用更窄的掩码（30°）：亮暗调整集中在色系中心，过渡区不放大原画面亮度差异
    float lR = hueMaskNarrow(h, 0.0)   * isActiveLight(LightR);
    float lO = hueMaskNarrow(h, 30.0)  * isActiveLight(LightO);
    float lY = hueMaskNarrow(h, 60.0)  * isActiveLight(LightY);
    float lG = hueMaskNarrow(h, 120.0) * isActiveLight(LightG);
    float lC = hueMaskNarrow(h, 180.0) * isActiveLight(LightC);
    float lB = hueMaskNarrow(h, 240.0) * isActiveLight(LightB);
    float lP = hueMaskNarrow(h, 270.0) * isActiveLight(LightP);
    float lM = hueMaskNarrow(h, 300.0) * isActiveLight(LightM);

    float wSum = wR + wO + wY + wG + wC + wB + wP + wM;
    float lSum = lR + lO + lY + lG + lC + lB + lP + lM;
    float hShift = 0.0;
    float sMul = 1.0;
    float lShift = 0.0;
    if (wSum > 1e-5 || lSum > 1e-5)
    {
        // 关键：除以 max(wSum,1) 而不是 wSum。原来除 wSum 会把单个色系的调整归一成“满强度”，
        // 效果从色系中心到掩码边界都是全量，只有到边界才骤降 → 出现棱角分明的色块。
        // 现在单个色系时效果随掩码权重平滑淡出（中心满、越靠近边界越弱）；
        // 只有多个色系重叠（wSum>1）才归一，避免叠加过冲。
        float norm = max(wSum, 1.0);
        float normL = max(lSum, 1.0);
        hShift = (wR*HueR + wO*HueO + wY*HueY + wG*HueG + wC*HueC + wB*HueB + wP*HueP + wM*HueM) / norm;
        sMul = 1.0 + (wR*(SatR-1.0) + wO*(SatO-1.0) + wY*(SatY-1.0) + wG*(SatG-1.0) + wC*(SatC-1.0) + wB*(SatB-1.0) + wP*(SatP-1.0) + wM*(SatM-1.0)) / norm;
        lShift = (lR*LightR + lO*LightO + lY*LightY + lG*LightG + lC*LightC + lB*LightB + lP*LightP + lM*LightM) / normL;
    }

    // OKLCH 中间调色：在感知均匀空间（OKLab/OKLCH，Ottosson 官方参考）施加
    // 主调整 + 分色系调整。感知均匀的亮度/色相可减缓渐变过渡的色带；
    // 暗部特殊处理同样基于感知亮度 L。
    float L2, C2, H2;
    RgbToOklch(c, L2, C2, H2);

    // 明度：主色系 + 分色系；暗部特殊处理 3：L 最小钳制 0.01，避免纯黑奇点
    L2 = saturate(L2 + MasterLight + lShift);
    L2 = max(L2, 0.01);

    // 暗部特殊处理 1+2：L < 0.15 时，彩度增量与色相调整权重按 smoothstep 衰减，
    // 防止暗部过饱和 / 色相偏移造成色阶断裂
    float darkDecay = smoothstep(0.0, 0.15, L2);

    // 色相：wrapHue 保证在 0..360 环上取最短路径；暗部降低色相调整权重
    H2 = wrapHue(H2 + hShift * darkDecay);

    // 彩度：主 × 全局 × 分色系，各自“相对 1.0 的增量”按 darkDecay 衰减
    float satTotal = MasterSat * GlobalSat * sMul;
    C2 = C2 * (1.0 + (satTotal - 1.0) * darkDecay);

    float3 rgb = OklchToRgb(L2, C2, H2);

    // 基础调节（与 FilterEngine.BuildMatrix 语义一致）
    rgb.r *= 1.0 + 0.18 * Temperature;
    rgb.g *= 1.0 + 0.04 * Temperature;
    rgb.b *= 1.0 - 0.18 * Temperature;

    rgb = (rgb - 0.5) * Contrast + 0.5 + Brightness;
    rgb *= 1.0 + Highlights * 0.5;
    rgb = rgb * (1.0 - Shadows) + Shadows;

    rgb = saturate(rgb);

    // 抖动：±1.2/255（1.2 LSB）的确定性噪声，打散 8-bit 量化在平滑渐变/掩码过渡区产生的色带
    // （banding），让过渡更自然；幅度很小，肉眼几乎不可见。
    float2 sp = pos.xy;
    float3 dither = float3(
        DitherNoise(sp),
        DitherNoise(sp + float2(7.1, 3.3)),
        DitherNoise(sp + float2(11.7, 5.9))) * (2.4 / 255.0) - (1.2 / 255.0);
    rgb = saturate(rgb + dither);

    return float4(rgb, color.a);
}
""";

    /// <summary>编译并创建一个中性参数的着色器效果。</summary>
    public static PixelShaderEffect CreateEffect()
    {
        var blob = HlslCompiler.CompilePixelShader(Source);
        var effect = new PixelShaderEffect(blob)
        {
            Source1Mapping = SamplerCoordinateMapping.OneToOne,
            Source1Interpolation = CanvasImageInterpolation.Linear,
        };
        ApplyNeutral(effect);
        return effect;
    }

    /// <summary>把所有参数置为中性（不产生任何效果）。</summary>
    public static void ApplyNeutral(PixelShaderEffect effect)
    {
        var p = effect.Properties;
        Set(p, "MasterHue", 0f);
        Set(p, "MasterSat", 1f);
        Set(p, "MasterLight", 0f);
        Set(p, "GlobalSat", 1f);
        Set(p, "Temperature", 0f);
        Set(p, "Contrast", 1f);
        Set(p, "Brightness", 0f);
        Set(p, "Highlights", 0f);
        Set(p, "Shadows", 0f);
        foreach (var suffix in new[] { "R", "O", "Y", "G", "C", "B", "P", "M" })
        {
            Set(p, "Hue" + suffix, 0f);
            Set(p, "Sat" + suffix, 1f);
            Set(p, "Light" + suffix, 0f);
        }
    }

    /// <summary>根据滤镜设置更新着色器常量缓冲。</summary>
    public static void Apply(PixelShaderEffect effect, FilterSettings s)
    {
        var p = effect.Properties;
        Set(p, "MasterHue", (float)s.Hue);
        Set(p, "MasterSat", (float)(s.HslSaturation / 100.0));
        Set(p, "MasterLight", (float)(s.Lightness / 100.0));
        Set(p, "GlobalSat", (float)(s.Saturation / 100.0));
        Set(p, "Temperature", (float)(s.Temperature / 100.0));
        Set(p, "Contrast", (float)(s.Contrast / 100.0));
        Set(p, "Brightness", (float)(s.Brightness / 100.0 * 0.5));
        Set(p, "Highlights", (float)(s.Highlights / 100.0));
        Set(p, "Shadows", (float)(s.Shadows / 100.0));

        foreach (var name in HslChannelNames.ColorNames)
        {
            var ch = s.HslChannels.FirstOrDefault(c => c.Name == name);
            if (ch == null) continue;
            string suffix = SuffixOf(name);
            Set(p, "Hue" + suffix, (float)ch.Hue);
            Set(p, "Sat" + suffix, (float)(ch.Saturation / 100.0));
            Set(p, "Light" + suffix, (float)(ch.Lightness / 100.0));
        }
    }

    /// <summary>
    /// 自检：渲染 红/绿/蓝 三个测试像素，仅对"红色系"做 +60° 色相旋转，
    /// 验证红色被改成黄色、而绿/蓝不受影响 —— 直接证明"分色系 HSL 调整"已生效。
    /// 返回人类可读的通过/失败信息。
    /// </summary>
    public static string SelfTest(CanvasDevice device)
    {
        try
        {
            using var effect = CreateEffect();
            ApplyNeutral(effect);
            effect.Properties["HueR"] = 60f; // 只调红色系

            using var rt = new CanvasRenderTarget(device, 3, 1, 96);
            using (var ds = rt.CreateDrawingSession())
            {
                ds.Clear(Microsoft.UI.Colors.Transparent);
                ds.FillRectangle(new Rect(0, 0, 1, 1), Microsoft.UI.Colors.Red);
                ds.FillRectangle(new Rect(1, 0, 2, 1), Microsoft.UI.Colors.Green);
                ds.FillRectangle(new Rect(2, 0, 3, 1), Microsoft.UI.Colors.Blue);
                ds.DrawImage(effect);
            }

            var px = rt.GetPixelColors(0, 0, 3, 1);
            var r = px[0];
            var g = px[1];
            var b = px[2];

            bool redMoved = r.G > 120 && r.B < 80;      // 红 → 黄（G 明显升高）
            bool greenUntouched = g.R < 80 && g.G > 150; // 绿不变
            bool blueUntouched = b.B > 150 && b.R < 80;  // 蓝不变

            string detail =
                $"红色系→(R{r.R} G{r.G} B{r.B})，绿色系→(R{g.R} G{g.G} B{g.B})，蓝色系→(R{b.R} G{b.G} B{b.B})";

            if (redMoved && greenUntouched && blueUntouched)
                return "✔ 通过：分色系 HSL 精确生效。\n" + detail + "\n（仅红色被色相旋转成黄色，绿/蓝完全不受影响）";
            return "✘ 未通过。\n" + detail;
        }
        catch (Exception ex)
        {
            return "着色器自检异常：" + ex.Message;
        }
    }

    private static void Set(IDictionary<string, object> p, string name, float value)
    {
        if (p.ContainsKey(name)) p[name] = value;
    }

    private static string SuffixOf(string name) => name switch
    {
        HslChannelNames.Red => "R",
        HslChannelNames.Orange => "O",
        HslChannelNames.Yellow => "Y",
        HslChannelNames.Green => "G",
        HslChannelNames.Cyan => "C",
        HslChannelNames.Blue => "B",
        HslChannelNames.Purple => "P",
        HslChannelNames.Magenta => "M",
        _ => "",
    };
}
