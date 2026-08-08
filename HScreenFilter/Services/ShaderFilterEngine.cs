using System;
using System.Runtime.InteropServices;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>
/// 逐像素 HSL 着色器滤镜引擎（DXGI 模式）。
///
/// 底层委托给 <see cref="VorticeHslEngine"/>：原始 Win32 覆盖层窗口 + D3D11 交换链 +
/// 逐像素 HSL 像素着色器 + DXGI Desktop Duplication 捕获（GPU 内 CopyResource），
/// 全程 GPU、无 CPU 往返，不触发 Windows 11 屏幕捕获隐私边框。
///
/// 若引擎启动失败，上层 <see cref="FilterEngine"/> 会自动回退到放大镜/伽马引擎。
/// </summary>
public static class ShaderFilterEngine
{
    private static readonly object _lock = new();
    private static bool _checked;
    private static bool _available;
    private static string _lastError = "";
    private static VorticeHslEngine? _engine;
    private static bool _running;

    public static bool IsAvailable
    {
        get
        {
            lock (_lock) { Initialize(); return _available; }
        }
    }

    public static string LastError => _lastError;

    /// <summary>
    /// 轻量探测：创建设备并编译着色器（不创建覆盖层窗口）。
    /// 幂等。若失败返回 false 并记录 <see cref="LastError"/>。
    /// </summary>
    public static bool Initialize()
    {
        lock (_lock)
        {
            if (_checked) return _available;
            _checked = true;
            try
            {
                // 快速验证：创建临时设备（着色器在 Start 时编译）
                using (Vortice.Direct3D11.D3D11.D3D11CreateDevice(
                    Vortice.Direct3D.DriverType.Hardware,
                    Vortice.Direct3D11.DeviceCreationFlags.BgraSupport, null!))
                {
                }
                _available = true;
            }
            catch (Exception ex)
            {
                _lastError = ex.Message;
                _available = false;
            }
            return _available;
        }
    }

    /// <summary>
    /// 应用（或更新）滤镜。需要 UI 线程调用。
    /// 首次调用时启动 VorticeHslEngine；之后只更新着色器参数。
    /// </summary>
    public static bool Apply(FilterSettings s)
    {
        lock (_lock)
        {
            if (!Initialize()) return false;
            try
            {
                var engine = _engine;
                // 渲染线程意外退出（桌面分辨率变化/捕获丢失/异常）时自动重建，避免画面永久冻结
                if (!_running || engine == null || !engine.IsRendering)
                {
                    AppLog.Write("ShaderEngine",
                        $"重建引擎 (running={_running}, engine={(engine != null)}, isRendering={(engine?.IsRendering ?? false)})");
                    engine?.Dispose();
                    _engine = null;
                    engine = new VorticeHslEngine();
                    if (!engine.Start(GetDesktopWidth(), GetDesktopHeight()))
                    {
                        _lastError = "DXGI 引擎启动失败：" + (engine.LastError ?? "未知错误");
                        AppLog.Write("ShaderEngine", "启动失败: " + _lastError);
                        engine.Dispose();
                        _engine = null;
                        return false;
                    }
                    _engine = engine;
                    _running = true;
                }
                engine.Apply(s);
                return _running;
            }
            catch (Exception ex)
            {
                _lastError = "着色器引擎启动失败：" + ex.Message;
                AppLog.Write("ShaderEngine", "Apply 异常: " + ex);
                try { StopInternal(); } catch { }
                return false;
            }
        }
    }

    /// <summary>停止覆盖层与捕获（重置为未启用状态）。</summary>
    public static void Stop()
    {
        lock (_lock) StopInternal();
    }

    /// <summary>设置覆盖层是否可被屏幕捕获（OBS 等）；转发给 VorticeHslEngine（若已启动，切换 WDA_MONITOR/EXCLUDEFROMCAPTURE）。</summary>
    public static void SetOverlayCapturable(bool capturable)
    {
        lock (_lock)
        {
            try { if (_engine != null) _engine.Capturable = capturable; } catch { }
        }
    }

    public static void Shutdown() => Stop();

    /// <summary>运行自检（轻量：验证 Vortice D3D11 引擎可用）。</summary>
    public static string RunSelfTest()
    {
        lock (_lock)
        {
            try
            {
                if (!Initialize()) return "着色器引擎不可用：" + LastError;
                return "Vortice D3D11 引擎可用";
            }
            catch (Exception ex)
            {
                return "着色器自检异常：" + ex.Message;
            }
        }
    }

    // ---------------- 内部实现 ----------------

    private static int GetDesktopWidth()
    {
        var monitor = OverlayNative.MonitorFromPoint(new OverlayNative.POINT(), OverlayNative.MONITOR_DEFAULTTOPRIMARY);
        var mi = new OverlayNative.MONITORINFO { cbSize = Marshal.SizeOf<OverlayNative.MONITORINFO>() };
        if (OverlayNative.GetMonitorInfo(monitor, ref mi))
        {
            int w = mi.rcMonitor.Right - mi.rcMonitor.Left;
            if (w > 0) return w;
        }
        return 1920;
    }

    private static int GetDesktopHeight()
    {
        var monitor = OverlayNative.MonitorFromPoint(new OverlayNative.POINT(), OverlayNative.MONITOR_DEFAULTTOPRIMARY);
        var mi = new OverlayNative.MONITORINFO { cbSize = Marshal.SizeOf<OverlayNative.MONITORINFO>() };
        if (OverlayNative.GetMonitorInfo(monitor, ref mi))
        {
            int h = mi.rcMonitor.Bottom - mi.rcMonitor.Top;
            if (h > 0) return h;
        }
        return 1080;
    }

    private static void StopInternal()
    {
        _running = false;
        try { _engine?.Dispose(); } catch { }
        _engine = null;
    }
}
