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

// 色系软掩码：以参考色相为中心，半宽约 40°，平滑衰减到 0。
// 8 个色系并非等距分布（相邻最小 30°、最大 60°），40° 半宽可避免
// 60° 间隔的相邻色系之间出现“不受任何色系影响”的死区。
float hueMask(float h, float refHue)
{
    float a = abs(h - refHue);
    a = min(a, 360.0 - a);
    float t = a / 40.0;
    float w = saturate(1.0 - t);
    return w * w * (3.0 - 2.0 * w); // smoothstep
}

// 该色系是否有实际调整（中性时权重归零，避免稀释其它色系）
float isActive(float h, float sat, float light)
{
    return (abs(h) > 0.01 || abs(sat - 1.0) > 0.001 || abs(light) > 0.001) ? 1.0 : 0.0;
}

float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 texel : TEXCOORD0) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, texel.xy);
    float3 c = color.rgb;

    float h, s, l;
    rgbToHsl(c, h, s, l);

    // 主色系（全部）
    h = wrapHue(h + MasterHue);
    s = saturate(s * MasterSat);
    l = saturate(l + MasterLight);

    // 全局鲜艳度（折入 HSL 饱和度）
    s = saturate(s * GlobalSat);

    // 六个分色系的软掩码（用主色系处理后的色相选择，所见即所选）
    float wR = hueMask(h, 0.0)   * isActive(HueR, SatR, LightR);
    float wO = hueMask(h, 30.0)  * isActive(HueO, SatO, LightO);
    float wY = hueMask(h, 60.0)  * isActive(HueY, SatY, LightY);
    float wG = hueMask(h, 120.0) * isActive(HueG, SatG, LightG);
    float wC = hueMask(h, 180.0) * isActive(HueC, SatC, LightC);
    float wB = hueMask(h, 240.0) * isActive(HueB, SatB, LightB);
    float wP = hueMask(h, 270.0) * isActive(HueP, SatP, LightP);
    float wM = hueMask(h, 300.0) * isActive(HueM, SatM, LightM);

    float wSum = wR + wO + wY + wG + wC + wB + wP + wM;
    if (wSum > 1e-5)
    {
        float hShift = (wR*HueR + wO*HueO + wY*HueY + wG*HueG + wC*HueC + wB*HueB + wP*HueP + wM*HueM) / wSum;
        float sMul = 1.0 + (wR*(SatR-1.0) + wO*(SatO-1.0) + wY*(SatY-1.0) + wG*(SatG-1.0) + wC*(SatC-1.0) + wB*(SatB-1.0) + wP*(SatP-1.0) + wM*(SatM-1.0)) / wSum;
        float lShift = (wR*LightR + wO*LightO + wY*LightY + wG*LightG + wC*LightC + wB*LightB + wP*LightP + wM*LightM) / wSum;
        h = wrapHue(h + hShift);
        s = saturate(s * sMul);
        l = saturate(l + lShift);
    }

    float3 rgb = hslToRgb(h, s, l);

    // 基础调节（与 FilterEngine.BuildMatrix 语义一致）
    rgb.r *= 1.0 + 0.18 * Temperature;
    rgb.g *= 1.0 + 0.04 * Temperature;
    rgb.b *= 1.0 - 0.18 * Temperature;

    rgb = (rgb - 0.5) * Contrast + 0.5 + Brightness;
    rgb *= 1.0 + Highlights * 0.5;
    rgb = rgb * (1.0 - Shadows) + Shadows;

    rgb = saturate(rgb);

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
