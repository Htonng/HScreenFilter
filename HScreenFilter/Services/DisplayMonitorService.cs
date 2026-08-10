using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>枚举当前系统所有显示器（虚拟坐标 + 尺寸 + 主显示器标记）。</summary>
public static class DisplayMonitorService
{
    /// <summary>用 EnumDisplayMonitors 枚举全部显示器。失败返回空列表。</summary>
    public static List<DisplayMonitor> Enumerate()
    {
        var list = new List<DisplayMonitor>();
        var proc = new OverlayNative.MONITORENUMPROC((IntPtr hMonitor, IntPtr hdc, ref OverlayNative.RECT rc, IntPtr data) =>
        {
            var mi = new OverlayNative.MONITORINFO { cbSize = Marshal.SizeOf<OverlayNative.MONITORINFO>() };
            if (OverlayNative.GetMonitorInfo(hMonitor, ref mi))
            {
                list.Add(new DisplayMonitor
                {
                    X = mi.rcMonitor.Left,
                    Y = mi.rcMonitor.Top,
                    Width = mi.rcMonitor.Right - mi.rcMonitor.Left,
                    Height = mi.rcMonitor.Bottom - mi.rcMonitor.Top,
                    IsPrimary = (mi.dwFlags & OverlayNative.MONITORINFOF_PRIMARY) != 0,
                });
            }
            return true;
        });
        // 枚举所有（hdc=null, lprcClip=null）
        OverlayNative.EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, proc, IntPtr.Zero);
        return list;
    }

    /// <summary>获取虚拟桌面的总边界（把所有显示器并起来）。</summary>
    public static (int Left, int Top, int Right, int Bottom) VirtualBounds(List<DisplayMonitor>? monitors)
    {
        if (monitors == null || monitors.Count == 0)
            return (0, 0, 1920, 1080);
        int l = int.MaxValue, t = int.MaxValue, r = int.MinValue, b = int.MinValue;
        foreach (var m in monitors)
        {
            l = Math.Min(l, m.X);
            t = Math.Min(t, m.Y);
            r = Math.Max(r, m.X + m.Width);
            b = Math.Max(b, m.Y + m.Height);
        }
        return (l, t, r, b);
    }
}
