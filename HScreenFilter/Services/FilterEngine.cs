using System;
using System.Linq;
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
    PixelShader,          // 逐像素着色器（能力最强：真正的分色系 HSL）
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
    private static bool _useDxgi;           // 是否使用 DXGI 捕获引擎（开启=HSL 可用）
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

    /// <summary>是否启用 DXGI 捕获引擎（开启时 HSL 才可用）。</summary>
    public static bool UseDxgi
    {
        get { lock (_lock) return _useDxgi; }
        set { lock (_lock) SetUseDxgi(value); }
    }

    public static bool SupportsSaturation =>
        _kind is FilterEngineKind.FullScreenColorEffect or FilterEngineKind.PixelShader;

    /// <summary>
    /// 设置引擎模式并重新探测：
    ///   true  → 逐像素着色器引擎（DXGI 捕获，支持分色系 HSL）；
    ///   false → 放大镜颜色矩阵引擎（不支持分色系 HSL）。
    /// 切换时会先停止当前引擎，再重新初始化。
    /// </summary>
    private static void SetUseDxgi(bool useDxgi)
    {
        if (_useDxgi == useDxgi) return;
        _useDxgi = useDxgi;

        // 停止当前引擎
        ShaderFilterEngine.Stop();
        StopMag();
        try { GammaRampEngine.Reset(); } catch { }

        // 强制重新探测
        _checked = false;
        _kind = FilterEngineKind.None;
    }

    private static void StopMag()
    {
        if (!_magInitialized) return;
        try
        {
            var m = MAGCOLOREFFECT.Identity();
            MagSetFullscreenColorEffect(ref m);
        }
        catch { }
        try { MagUninitialize(); } catch { }
        _magInitialized = false;
    }

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

            // 开启 DXGI → 逐像素着色器引擎（HSL 可用）；否则跳过，直接用放大镜/伽马引擎
            if (_useDxgi)
            {
                // 这里只做轻量探测（编译着色器 + 创建设备），覆盖层与捕获在首次 Apply 时才建立。
                if (ShaderFilterEngine.Initialize())
                {
                    _kind = FilterEngineKind.PixelShader;
                    return true;
                }
                LastError = "DXGI 着色器引擎不可用：" + ShaderFilterEngine.LastError + "，已回退到放大镜引擎";
            }

            // 2) 全屏颜色矩阵（需要 Windows 10 1903+）。
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

            // 3) 回退：显卡伽马曲线
            if (GammaRampEngine.Test())
            {
                _kind = FilterEngineKind.GammaRamp;
                return true;
            }

            _kind = FilterEngineKind.None;
            return false;
        }
    }

    /// <summary>确保放大镜颜色矩阵引擎就绪（供着色器引擎回退时使用）。</summary>
    private static bool EnsureMagReady()
    {
        if (_magInitialized) return true;
        if (Environment.OSVersion.Version.Build < 18362) return false;
        try
        {
            if (MagInitialize())
            {
                _magInitialized = true;
                var id = MAGCOLOREFFECT.Identity();
                MagSetFullscreenColorEffect(ref id);
                return true;
            }
        }
        catch
        {
            // 忽略，交给上层报错
        }
        return false;
    }

    /// <summary>
    /// 对指定显示器应用滤镜。需要 UI 线程调用。
    /// DXGI（PixelShader）时每显示器一个覆盖层；放大镜/伽马引擎是整屏全局的（无法按显示器独立，
    /// 此时无论哪块显示器都用同一份设置，属于底层物理限制）。
    /// </summary>
    public static bool Apply(int displayIndex, DisplayMonitor display, FilterSettings s)
    {
        if (!Initialize()) return false;
        lock (_lock)
        {
            if (_kind == FilterEngineKind.PixelShader)
            {
                if (ShaderFilterEngine.Apply(displayIndex, display, s)) return true;

                // 覆盖层/捕获建立失败 → 回退到颜色矩阵/伽马引擎
                LastError = "逐像素着色器引擎不可用，已回退：" + ShaderFilterEngine.LastError;
                _kind = EnsureMagReady() ? FilterEngineKind.FullScreenColorEffect : FilterEngineKind.GammaRamp;
            }

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
            if (_kind == FilterEngineKind.PixelShader)
            {
                ShaderFilterEngine.Stop();
                return true;
            }
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

    /// <summary>关闭指定显示器的覆盖层（该显示器不应用滤镜时调用）。DXGI 引擎才支持逐显示器；其余引擎整屏全局忽略。</summary>
    public static void ResetDisplay(int displayIndex)
    {
        lock (_lock)
        {
            if (_kind == FilterEngineKind.PixelShader)
                ShaderFilterEngine.StopDisplay(displayIndex);
        }
    }

    /// <summary>设置滤镜覆盖层是否可被屏幕捕获（OBS 等）。仅 DXGI 引擎有覆盖层窗口。</summary>
    public static void SetOverlayCapturable(bool capturable) => ShaderFilterEngine.SetOverlayCapturable(capturable);

    public static void Shutdown()
    {
        lock (_lock)
        {
            ShaderFilterEngine.Shutdown();
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
        GetActiveHsl(s, out var hslIsSpecific, out var hslHue, out var hslSat, out var hslLight);
        HSLMatrix(hslIsSpecific, hslHue, hslSat, hslLight,
                  out var ah00, out var ah01, out var ah02,
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
    /// 取出当前选中色系的 HSL 值。
    /// 若选中“全部(主)”，使用 FilterSettings.Hue/HslSaturation/Lightness；
    /// 若选中某个具体色系，使用 HslChannels 中对应项的值并以 isSpecific 标记。
    /// </summary>
    private static void GetActiveHsl(
        FilterSettings s,
        out bool isSpecific, out double hue, out double sat, out double light)
    {
        var ch = s.HslChannels.FirstOrDefault(c => c.Name == s.ActiveHslChannel);
        if (ch != null && s.ActiveHslChannel != HslChannelNames.Master)
        {
            isSpecific = true;
            hue = ch.Hue;
            sat = ch.Saturation / 100.0;
            light = ch.Lightness / 100.0;
        }
        else
        {
            isSpecific = false;
            hue = s.Hue;
            sat = s.HslSaturation / 100.0;
            light = s.Lightness / 100.0;
        }
    }

    /// <summary>
    /// 构建 HSL 仿射变换（out = Ah·in + bh）。处理顺序：色相旋转 → HSL 饱和度 → 明度。
    /// <paramref name="isSpecific"/> 为真时，对该色系做近似选择性调整。
    /// 中性时 Ah 为单位阵、bh=0，不产生任何效果。
    /// </summary>
    private static void HSLMatrix(
        bool isSpecific, double hueDeg, double sat, double light,
        out float a00, out float a01, out float a02,
        out float a10, out float a11, out float a12,
        out float a20, out float a21, out float a22,
        out float offset)
    {
        // 基础（非选择性）HSL：色相旋转 → 饱和度 → 明度
        BuildHueSatMatrix(hueDeg, sat, out var m00, out var m01, out var m02,
                                            out var m10, out var m11, out var m12,
                                            out var m20, out var m21, out var m22);
        LightnessGain(light, out var ls, out var lo);

        if (isSpecific)
        {
            // 近似“选择性”调整（单一全屏矩阵无法做像素级分色，此处用该色系自身的
            // 色相旋转方向来体现差异，属于物理限制下的近似模拟）。
            BuildSelectiveMatrix(
                m00, m01, m02, m10, m11, m12, m20, m21, m22, ls, lo,
                out var s00, out var s01, out var s02,
                out var s10, out var s11, out var s12,
                out var s20, out var s21, out var s22,
                out var so);

            a00 = s00; a01 = s01; a02 = s02;
            a10 = s10; a11 = s11; a12 = s12;
            a20 = s20; a21 = s21; a22 = s22;
            offset = so;
            return;
        }

        a00 = m00 * ls; a01 = m01 * ls; a02 = m02 * ls;
        a10 = m10 * ls; a11 = m11 * ls; a12 = m12 * ls;
        a20 = m20 * ls; a21 = m21 * ls; a22 = m22 * ls;
        offset = lo;
    }

    /// <summary>构建 色相旋转 × HSL饱和度 的 3×3 矩阵。</summary>
    private static void BuildHueSatMatrix(
        double hueDeg, double sat,
        out float m00, out float m01, out float m02,
        out float m10, out float m11, out float m12,
        out float m20, out float m21, out float m22)
    {
        const float Wr = 0.2126f, Wg = 0.7152f, Wb = 0.0722f;
        float hueRad = (float)(hueDeg * Math.PI / 180.0);
        float cos = (float)Math.Cos(hueRad);
        float sin = (float)Math.Sin(hueRad);

        float hu00 = Wr + cos * (1f - Wr) - sin * Wr;
        float hu01 = Wg - cos * Wg - sin * Wg;
        float hu02 = Wb - cos * Wb + sin * (1f - Wb);
        float hu10 = Wr - cos * Wr + sin * Wr;
        float hu11 = Wg + cos * (1f - Wg) - sin * Wg;
        float hu12 = Wb - cos * Wb - sin * Wb;
        float hu20 = Wr - cos * Wr - sin * (1f - Wr);
        float hu21 = Wg - cos * Wg + sin * Wg;
        float hu22 = Wb + cos * (1f - Wb) + sin * Wb;

        float invSat = 1f - (float)sat;
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

    /// <summary>明度缩放/偏移参数。</summary>
    private static void LightnessGain(double light, out float scale, out float offset)
    {
        if (light >= 0f)
        {
            scale = 1f - (float)Math.Min(light, 1.0);
            offset = (float)Math.Min(light, 1.0);
        }
        else
        {
            scale = 1f + (float)Math.Max(light, -1.0);
            offset = 0f;
        }
    }

    /// <summary>
    /// 构建近似“选择性”仿射（out = S·in + so）。
    /// 具体色系的基础矩阵 M（该色系自身的 色相旋转 × 饱和度）整体按 ls 缩放，
    /// 明度偏移 lo 保持不变。由于当前引擎只能整屏套用单个线性矩阵、无法逐像素做
    /// 真正的“只改红色”，各色系的选择差异主要由其自身的色相旋转方向（各通道行和不同）
    /// 来体现，属于物理限制下的近似模拟。
    /// </summary>
    private static void BuildSelectiveMatrix(
        float m00, float m01, float m02,
        float m10, float m11, float m12,
        float m20, float m21, float m22,
        float ls, float lo,
        out float s00, out float s01, out float s02,
        out float s10, out float s11, out float s12,
        out float s20, out float s21, out float s22,
        out float so)
    {
        // 基础矩阵整体缩放（饱和度/色相）
        s00 = m00 * ls; s01 = m01 * ls; s02 = m02 * ls;
        s10 = m10 * ls; s11 = m11 * ls; s12 = m12 * ls;
        s20 = m20 * ls; s21 = m21 * ls; s22 = m22 * ls;
        so = lo;
    }
}