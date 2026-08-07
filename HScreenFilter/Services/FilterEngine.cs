using System;
using System.Runtime.InteropServices;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>
/// 5×5 全屏颜色变换矩阵（MAGCOLOREFFECT）。
/// 布局：前 4 行 × 前 4 列为 RGBA 线性变换，第 5 列为偏移量（offset）。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct MAGCOLOREFFECT
{
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 25)]
    public float[] transform;

    public static MAGCOLOREFFECT Identity()
    {
        var m = new MAGCOLOREFFECT { transform = new float[25] };
        m.transform[0] = 1f;  // R
        m.transform[6] = 1f;  // G
        m.transform[12] = 1f; // B
        m.transform[18] = 1f; // A
        m.transform[24] = 1f; // 末行（未使用）
        return m;
    }
}

public enum FilterEngineKind
{
    None,
    FullScreenColorEffect,
    GammaRamp,
}

/// <summary>
/// 滤镜引擎：优先使用 Windows 放大镜 API 的全屏颜色效果（支持鲜艳度/色温等完整颜色矩阵），
/// 若系统不支持则回退到显卡伽马曲线（不支持鲜艳度）。
/// </summary>
public static class FilterEngine
{
    private static readonly object _lock = new();
    private static bool _checked;
    private static bool _magInitialized;
    private static FilterEngineKind _kind = FilterEngineKind.None;

    public static FilterEngineKind Kind
    {
        get
        {
            lock (_lock) { Initialize(); return _kind; }
        }
    }

    public static string LastError { get; private set; } = "";

    public static bool SupportsSaturation => _kind == FilterEngineKind.FullScreenColorEffect;

    [DllImport("Magnification.dll", SetLastError = true)]
    private static extern bool MagInitialize();

    [DllImport("Magnification.dll", SetLastError = true)]
    private static extern bool MagSetFullscreenColorEffect(ref MAGCOLOREFFECT pEffect);

    [DllImport("Magnification.dll", SetLastError = true)]
    private static extern bool MagUninitialize();

    /// <summary>探测并选定可用引擎（幂等）。</summary>
    public static bool Initialize()
    {
        lock (_lock)
        {
            if (_checked) return _kind != FilterEngineKind.None;
            _checked = true;

            // 全屏颜色效果需要 Windows 10 1903 (build 18362) 及以上。
            if (Environment.OSVersion.Version.Build >= 18362)
            {
                try
                {
                    if (MagInitialize())
                    {
                        _magInitialized = true;
                        var id = MAGCOLOREFFECT.Identity();
                        if (MagSetFullscreenColorEffect(ref id))
                        {
                            _kind = FilterEngineKind.FullScreenColorEffect;
                            return true;
                        }
                        LastError = "系统拒绝了全屏颜色效果（可能被组策略/安全策略禁用）";
                        MagUninitialize();
                        _magInitialized = false;
                    }
                    else
                    {
                        LastError = "MagInitialize 失败";
                    }
                }
                catch (Exception ex)
                {
                    LastError = ex.Message;
                }
            }
            else
            {
                LastError = "Windows 10 1903+ 才支持全屏颜色效果";
            }

            if (GammaRampEngine.Test())
            {
                _kind = FilterEngineKind.GammaRamp;
                return true;
            }

            _kind = FilterEngineKind.None;
            return false;
        }
    }

    public static bool Apply(FilterSettings s)
    {
        if (!Initialize()) return false;
        lock (_lock)
        {
            if (_kind == FilterEngineKind.FullScreenColorEffect)
            {
                var m = BuildMatrix(s);
                return MagSetFullscreenColorEffect(ref m);
            }
            if (_kind == FilterEngineKind.GammaRamp)
                return GammaRampEngine.Apply(s);
            return false;
        }
    }

    public static bool Reset()
    {
        if (!Initialize()) return false;
        lock (_lock)
        {
            if (_kind == FilterEngineKind.FullScreenColorEffect)
            {
                var m = MAGCOLOREFFECT.Identity();
                return MagSetFullscreenColorEffect(ref m);
            }
            if (_kind == FilterEngineKind.GammaRamp)
                return GammaRampEngine.Reset();
            return true;
        }
    }

    public static void Shutdown()
    {
        lock (_lock)
        {
            if (_magInitialized)
            {
                try
                {
                    var m = MAGCOLOREFFECT.Identity();
                    MagSetFullscreenColorEffect(ref m);
                }
                catch { }
                MagUninitialize();
                _magInitialized = false;
            }
        }
    }

    /// <summary>
    /// 由滤镜参数构建 5×5 颜色矩阵（GDI+ / MAGCOLOREFFECT 约定）。
    ///
    /// 重要：MAGCOLOREFFECT 遵循 GDI+ 颜色矩阵约定 ——
    ///   newColor = [R,G,B,A,1] × Matrix（行向量乘矩阵）
    /// 因此：
    ///   · 系数矩阵需要【转置】存储（第 j 列 = 输入通道 j 对各输出通道的贡献）；
    ///   · 偏移量放在【最后一行】Matrix[4][0..2]（而不是最后一列）。
    ///
    /// 处理顺序：
    ///   HSL（色相旋转 → 饱和度 → 明度）→ 色温(tint) → 鲜艳度(saturation)
    ///   → 对比度(contrast) → 亮度(brightness) → 亮部(highlights gain) → 暗部(shadows lift)。
    ///
    /// 定义（均按列向量约定，输出 = M × 输入）：
    ///   · 色相旋转 Hu：标准 RGB 色相旋转矩阵，灰度像素不受影响。
    ///   · 饱和度 Sat：围绕灰度轴缩放。
    ///   · 明度 L（-1..1）：正值 out=(1-L)*v + L（向白场混合，白场保持、黑场抬升）；
    ///     负值 out=(1+L)*v（向黑场收缩，黑场保持）。
    ///
    /// 最终仿射 = Ae × Ah（先做 HSL，再做既有引擎变换 Ae、偏移 be）：
    ///   final3x3   = Ae × Ah
    ///   finalOffset = Ae · bh + be
    /// 当 HSL 全中性时 Ah=I、bh=0，退化为原矩阵，不改变既有行为。
    /// </summary>
    public static MAGCOLOREFFECT BuildMatrix(FilterSettings s)
    {
        float brightness = (float)(s.Brightness / 100.0 * 0.5);        // ±0.5 偏移
        float contrast = (float)(s.Contrast / 100.0);                  // 1 = 中性
        float saturation = (float)(s.Saturation / 100.0);              // 1 = 中性
        float highlights = (float)(1.0 + s.Highlights / 100.0 * 0.5);  // 0.5..1.5 增益
        float shadows = (float)(s.Shadows / 100.0 * 0.5);              // ±0.5（负=压暗阴影）
        float temp = (float)(s.Temperature / 100.0);                   // -1..1

        // 色温 → 对角色调（暖：R↑ B↓；冷：R↓ B↑）
        float tr = 1f + 0.18f * temp;
        float tg = 1f + 0.04f * temp;
        float tb = 1f - 0.18f * temp;

        // Rec.709 亮度权重
        const float Wr = 0.2126f, Wg = 0.7152f, Wb = 0.0722f;
        float invSat = 1f - saturation;

        // 暗部 lift 会整体缩放 (1-l)，亮部 gain 与对比度叠加为统一矩阵系数
        float matrixScale = (1f - shadows) * highlights * contrast;

        // 鲜艳度矩阵 S（围绕灰度轴缩放，行=输出通道）
        float s00 = Wr * invSat + saturation;
        float s01 = Wg * invSat;
        float s02 = Wb * invSat;
        float s10 = Wr * invSat;
        float s11 = Wg * invSat + saturation;
        float s12 = Wb * invSat;
        float s20 = Wr * invSat;
        float s21 = Wg * invSat;
        float s22 = Wb * invSat + saturation;

        // 既有引擎变换 Ae（3×3）+ 偏移 be（仿射 out = Ae·in + be）
        // mXY 表示"X 通道的输出由 Y 通道输入贡献多少"
        float e00 = s00 * tr * matrixScale;   // R 输出 ← R 输入
        float e01 = s01 * tg * matrixScale;   // R 输出 ← G 输入
        float e02 = s02 * tb * matrixScale;   // R 输出 ← B 输入
        float e10 = s10 * tr * matrixScale;   // G 输出 ← R 输入
        float e11 = s11 * tg * matrixScale;   // G 输出 ← G 输入
        float e12 = s12 * tb * matrixScale;   // G 输出 ← B 输入
        float e20 = s20 * tr * matrixScale;   // B 输出 ← R 输入
        float e21 = s21 * tg * matrixScale;   // B 输出 ← G 输入
        float e22 = s22 * tb * matrixScale;   // B 输出 ← B 输入
        float be = (1f - shadows) * highlights * (0.5f * (1f - contrast) + brightness) + shadows;

        // ---- HSL 部分（先于既有变换应用）----
        HSLMatrix(s, out var ah00, out var ah01, out var ah02,
                       out var ah10, out var ah11, out var ah12,
                       out var ah20, out var ah21, out var ah22,
                       out var bh);

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

        // 总偏移（逐通道）：offset_i = be + (Ae 第 i 行行和) × bh
        // 既有 be 与明度 bh 均为各通道相同，此处按行和展开以保持仿射合成精确。
        float offsetR = be + (e00 + e01 + e02) * bh;
        float offsetG = be + (e10 + e11 + e12) * bh;
        float offsetB = be + (e20 + e21 + e22) * bh;

        var m = MAGCOLOREFFECT.Identity();
        float[] t = m.transform;

        // 按 GDI+ 约定【转置】存储：out = [R,G,B,A,1] × M
        t[0]  = m00;   // M[0][0] = R 输出 ← R
        t[5]  = m01;   // M[1][0] = R 输出 ← G
        t[10] = m02;   // M[2][0] = R 输出 ← B
        t[1]  = m10;   // M[0][1] = G 输出 ← R
        t[6]  = m11;   // M[1][1] = G 输出 ← G
        t[11] = m12;   // M[2][1] = G 输出 ← B
        t[2]  = m20;   // M[0][2] = B 输出 ← R
        t[7]  = m21;   // M[1][2] = B 输出 ← G
        t[12] = m22;   // M[2][2] = B 输出 ← B

        // Alpha 直通
        t[18] = 1f;    // M[3][3]

        // 偏移放在最后一行（GDI+ 约定）：M[4][0..2]
        t[20] = offsetR;
        t[21] = offsetG;
        t[22] = offsetB;

        return m;
    }

    /// <summary>
    /// 构建 HSL 仿射变换（out = Ah·in + bh），处理顺序：色相旋转 → HSL 饱和度 → 明度。
    /// 中性时 Ah 为单位阵、bh=0，不产生任何效果。
    /// </summary>
    private static void HSLMatrix(
        FilterSettings s,
        out float a00, out float a01, out float a02,
        out float a10, out float a11, out float a12,
        out float a20, out float a21, out float a22,
        out float offset)
    {
        // 色相旋转角度（度 → 弧度）
        float hueRad = (float)(s.Hue * Math.PI / 180.0);
        float cos = (float)Math.Cos(hueRad);
        float sin = (float)Math.Sin(hueRad);

        // Rec.709 亮度权重
        const float Wr = 0.2126f, Wg = 0.7152f, Wb = 0.0722f;

        // 标准 RGB 色相旋转矩阵 Hu（各行权重和为 1 → 灰度不变）
        float hu00 = Wr + cos * (1f - Wr) - sin * Wr;
        float hu01 = Wg - cos * Wg - sin * Wg;
        float hu02 = Wb - cos * Wb + sin * (1f - Wb);
        float hu10 = Wr - cos * Wr + sin * Wr;
        float hu11 = Wg + cos * (1f - Wg) - sin * Wg;
        float hu12 = Wb - cos * Wb - sin * Wb;
        float hu20 = Wr - cos * Wr - sin * (1f - Wr);
        float hu21 = Wg - cos * Wg + sin * Wg;
        float hu22 = Wb + cos * (1f - Wb) + sin * Wb;

        // HSL 饱和度（围绕灰度轴缩放）
        float sat = (float)(s.HslSaturation / 100.0);
        float invSat = 1f - sat;
        float sa00 = Wr * invSat + sat;
        float sa01 = Wg * invSat;
        float sa02 = Wb * invSat;
        float sa10 = Wr * invSat;
        float sa11 = Wg * invSat + sat;
        float sa12 = Wb * invSat;
        float sa20 = Wr * invSat;
        float sa21 = Wg * invSat;
        float sa22 = Wb * invSat + sat;

        // Sat × Hu
        float m00 = sa00 * hu00 + sa01 * hu10 + sa02 * hu20;
        float m01 = sa00 * hu01 + sa01 * hu11 + sa02 * hu21;
        float m02 = sa00 * hu02 + sa01 * hu12 + sa02 * hu22;
        float m10 = sa10 * hu00 + sa11 * hu10 + sa12 * hu20;
        float m11 = sa10 * hu01 + sa11 * hu11 + sa12 * hu21;
        float m12 = sa10 * hu02 + sa11 * hu12 + sa12 * hu22;
        float m20 = sa20 * hu00 + sa21 * hu10 + sa22 * hu20;
        float m21 = sa20 * hu01 + sa21 * hu11 + sa22 * hu21;
        float m22 = sa20 * hu02 + sa21 * hu12 + sa22 * hu22;

        // 明度 L（-1..1，0 为中性）
        float light = (float)(s.Lightness / 100.0);
        float ls, lo;
        if (light >= 0f)
        {
            // 向白场混合：out = (1-L)*in + L
            ls = 1f - Math.Min(light, 1f);
            lo = Math.Min(light, 1f);
        }
        else
        {
            // 向黑场收缩：out = (1+L)*in  （1+L 非负）
            ls = 1f + Math.Max(light, -1f);
            lo = 0f;
        }

        a00 = m00 * ls; a01 = m01 * ls; a02 = m02 * ls;
        a10 = m10 * ls; a11 = m11 * ls; a12 = m12 * ls;
        a20 = m20 * ls; a21 = m21 * ls; a22 = m22 * ls;
        offset = lo;
    }
}
