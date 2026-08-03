using System;
using System.Runtime.InteropServices;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>
/// 回退引擎：通过显卡伽马表（SetDeviceGammaRamp）逐通道调整。
/// 可表达亮度/对比度/亮部/暗部/色温，但不能做跨通道的"鲜艳度"。
/// </summary>
public static class GammaRampEngine
{
    [StructLayout(LayoutKind.Sequential)]
    private struct RAMP
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public ushort[] Red;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public ushort[] Green;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public ushort[] Blue;
    }

    [DllImport("gdi32.dll")]
    private static extern bool SetDeviceGammaRamp(IntPtr hDC, ref RAMP lpRamp);

    [DllImport("gdi32.dll")]
    private static extern bool GetDeviceGammaRamp(IntPtr hDC, ref RAMP lpRamp);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

    private static IntPtr ScreenDC()
    {
        // GetDC(NULL) 返回整个屏幕的设备上下文，支持伽马表。
        return GetDC(IntPtr.Zero);
    }

    public static bool Test()
    {
        IntPtr hdc = ScreenDC();
        if (hdc == IntPtr.Zero) return false;
        var ramp = new RAMP { Red = new ushort[256], Green = new ushort[256], Blue = new ushort[256] };
        bool ok = GetDeviceGammaRamp(hdc, ref ramp);
        ReleaseDC(IntPtr.Zero, hdc);
        return ok;
    }

    public static bool Apply(FilterSettings s)
    {
        var ramp = BuildRamp(s);
        IntPtr hdc = ScreenDC();
        if (hdc == IntPtr.Zero) return false;
        bool ok = SetDeviceGammaRamp(hdc, ref ramp);
        ReleaseDC(IntPtr.Zero, hdc);
        return ok;
    }

    public static bool Reset()
    {
        var ramp = LinearRamp();
        IntPtr hdc = ScreenDC();
        if (hdc == IntPtr.Zero) return false;
        bool ok = SetDeviceGammaRamp(hdc, ref ramp);
        ReleaseDC(IntPtr.Zero, hdc);
        return ok;
    }

    private static RAMP LinearRamp()
    {
        var ramp = new RAMP { Red = new ushort[256], Green = new ushort[256], Blue = new ushort[256] };
        for (int i = 0; i < 256; i++)
        {
            ushort v = (ushort)(i * 257);
            ramp.Red[i] = v;
            ramp.Green[i] = v;
            ramp.Blue[i] = v;
        }
        return ramp;
    }

    private static RAMP BuildRamp(FilterSettings s)
    {
        float brightness = (float)(s.Brightness / 100.0 * 0.5);
        float contrast = (float)(s.Contrast / 100.0);
        float highlights = (float)(1.0 + s.Highlights / 100.0 * 0.5);
        float shadows = (float)(s.Shadows / 100.0 * 0.5);
        float temp = (float)(s.Temperature / 100.0);

        float tr = 1f + 0.18f * temp;
        float tg = 1f + 0.04f * temp;
        float tb = 1f - 0.18f * temp;

        var ramp = new RAMP { Red = new ushort[256], Green = new ushort[256], Blue = new ushort[256] };
        for (int i = 0; i < 256; i++)
        {
            float v = i / 255f;
            ramp.Red[i] = ToRamp(Curve(v, tr, brightness, contrast, highlights, shadows));
            ramp.Green[i] = ToRamp(Curve(v, tg, brightness, contrast, highlights, shadows));
            ramp.Blue[i] = ToRamp(Curve(v, tb, brightness, contrast, highlights, shadows));
        }
        return ramp;
    }

    private static float Curve(float v, float tint, float brightness, float contrast, float highlights, float shadows)
    {
        // 与颜色矩阵引擎保持一致的语义：
        //   色温(tint) → 对比度(围绕 0.5) → 亮度(偏移) → 亮部(增益) → 暗部(保持白色的提亮)
        float x = contrast * (v * tint) + 0.5f * (1f - contrast) + brightness;
        x = x * highlights;
        x = x * (1f - shadows) + shadows;
        return Math.Clamp(x, 0f, 1f);
    }

    private static ushort ToRamp(float v) => (ushort)Math.Clamp((int)(v * 65535f), 0, 65535);
}
