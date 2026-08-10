using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>
/// 逐像素 HSL 着色器滤镜引擎（DXGI 模式），支持多显示器：每块显示器一个覆盖层实例。
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

    /// <summary>每个显示器一个引擎实例（key = 显示器索引）。</summary>
    private static readonly Dictionary<int, VorticeHslEngine> _engines = new();

    private static bool _capturable; // 覆盖层是否可被 OBS 等捕获（WDA_MONITOR）；重建引擎后要重新应用

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
    /// 应用（或更新）某块显示器的滤镜。需要 UI 线程调用。
    /// 首次调用时启动该显示器的 VorticeHslEngine；之后只更新着色器参数。
    /// </summary>
    public static bool Apply(int displayIndex, DisplayMonitor display, FilterSettings s)
    {
        lock (_lock)
        {
            if (!Initialize()) return false;
            try
            {
                var engine = GetOrCreateEngine(displayIndex, display);
                if (engine == null) return false;
                engine.Apply(s);
                return true;
            }
            catch (Exception ex)
            {
                _lastError = "着色器引擎启动失败：" + ex.Message;
                AppLog.Write("ShaderEngine", "Apply 异常: " + ex);
                try { RemoveEngine(displayIndex); } catch { }
                return false;
            }
        }
    }

    /// <summary>停止全部覆盖层与捕获（重置为未启用状态）。</summary>
    public static void Stop()
    {
        lock (_lock)
        {
            foreach (var e in _engines.Values.ToList())
            {
                try { e.Dispose(); } catch { }
            }
            _engines.Clear();
        }
    }

    /// <summary>停止指定显示器的覆盖层（该显示器不应用滤镜时调用）。</summary>
    public static void StopDisplay(int displayIndex)
    {
        lock (_lock)
        {
            if (_engines.TryGetValue(displayIndex, out var e))
            {
                try { e.Dispose(); } catch { }
                _engines.Remove(displayIndex);
            }
        }
    }

    /// <summary>获取（必要时创建）指定显示器的引擎实例。失败返回 null。</summary>
    private static VorticeHslEngine? GetOrCreateEngine(int displayIndex, DisplayMonitor display)
    {
        if (_engines.TryGetValue(displayIndex, out var engine) && engine != null && engine.IsRendering)
            return engine;

        AppLog.Write("ShaderEngine",
            $"{(engine != null ? "重建" : "新建")} 显示器 {displayIndex} 引擎 ({display.Width}x{display.Height} @{display.X},{display.Y}, output={displayIndex})");
        if (engine != null) { try { engine.Dispose(); } catch { } }
        _engines.Remove(displayIndex);

        engine = new VorticeHslEngine();
        if (!engine.Start(display.X, display.Y, display.Width, display.Height, displayIndex))
        {
            _lastError = "DXGI 引擎启动失败：" + (engine.LastError ?? "未知错误");
            AppLog.Write("ShaderEngine", "启动失败: " + _lastError);
            try { engine.Dispose(); } catch { }
            _engines.Remove(displayIndex);
            return null;
        }
        // 重建后重新应用“可被捕获”状态
        try { engine.Capturable = _capturable; } catch { }
        _engines[displayIndex] = engine;
        return engine;
    }

    /// <summary>设置覆盖层是否可被屏幕捕获（OBS 等）；转发给所有 VorticeHslEngine。
    /// 记录该状态，引擎重建后会自动重新应用。</summary>
    public static void SetOverlayCapturable(bool capturable)
    {
        lock (_lock)
        {
            _capturable = capturable;
            foreach (var e in _engines.Values)
            {
                try { e.Capturable = capturable; } catch { }
            }
        }
    }

    /// <summary>是否已有引擎正在渲染（任一显示器）。</summary>
    public static bool IsAnyRendering()
    {
        lock (_lock) return _engines.Values.Any(e => e.IsRendering);
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

    private static void RemoveEngine(int displayIndex)
    {
        if (_engines.TryGetValue(displayIndex, out var e))
        {
            try { e.Dispose(); } catch { }
            _engines.Remove(displayIndex);
        }
    }
}
