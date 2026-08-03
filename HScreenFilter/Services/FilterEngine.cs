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
    /// 处理顺序：色温(tint) → 鲜艳度(saturation) → 对比度(contrast) → 亮度(brightness) → 亮部(highlights gain) → 暗部(shadows lift)。
    /// 亮部 = 乘法增益(gain)；暗部 = 保持白色的提亮(lift)：out = (1-l)*v + l（阴影抬起、白场不变）。
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

        // 合成 3×3 = S × diag(tr,tg,tb) × matrixScale
        // mXY 表示"X 通道的输出由 Y 通道输入贡献多少"
        float m00 = s00 * tr * matrixScale;   // R 输出 ← R 输入
        float m01 = s01 * tg * matrixScale;   // R 输出 ← G 输入
        float m02 = s02 * tb * matrixScale;   // R 输出 ← B 输入
        float m10 = s10 * tr * matrixScale;   // G 输出 ← R 输入
        float m11 = s11 * tg * matrixScale;   // G 输出 ← G 输入
        float m12 = s12 * tb * matrixScale;   // G 输出 ← B 输入
        float m20 = s20 * tr * matrixScale;   // B 输出 ← R 输入
        float m21 = s21 * tg * matrixScale;   // B 输出 ← G 输入
        float m22 = s22 * tb * matrixScale;   // B 输出 ← B 输入

        // 总偏移：o = (1-l)*g*(0.5*(1-c) + b) + l
        float offset = (1f - shadows) * highlights * (0.5f * (1f - contrast) + brightness) + shadows;

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
        t[20] = offset;
        t[21] = offset;
        t[22] = offset;

        return m;
    }
}
