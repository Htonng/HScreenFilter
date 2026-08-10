using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace HScreenFilter.Models;

/// <summary>一块显示器的信息（虚拟坐标 + 尺寸 + 是否主显示器）。</summary>
public class DisplayMonitor
{
    public string DeviceName { get; set; } = "";
    public int X { get; set; }      // 虚拟桌面 X（左上角）
    public int Y { get; set; }      // 虚拟桌面 Y（左上角）
    public int Width { get; set; }
    public int Height { get; set; }
    public bool IsPrimary { get; set; }

    public string DisplayName => IsPrimary ? "主显示器" : "显示器";
    public string Label => IsPrimary ? $"主显示器 ({Width}×{Height})" : $"显示器 {Width}×{Height}";
}
