using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>
/// 前台应用监听器：轮询当前前台窗口，判断命中哪个已绑定进程。
/// 命中 = 进程名相等（忽略大小写，不含 .exe）且窗口标题包含绑定文字（绑定标题留空则任意标题）。
/// 命中状态变化时触发事件（在调用线程上）。
/// </summary>
public sealed class ForegroundAppWatcher
{
    private readonly System.Threading.Timer _timer;
    private List<AppBinding> _targets = new();
    private AppBinding? _currentHit;

    /// <summary>命中状态变化：(命中绑定, processName, windowTitle)。未命中任何绑定 → 第一个参数为 null。</summary>
    public event Action<AppBinding?, string, string>? MatchChanged;

    public ForegroundAppWatcher()
    {
        _timer = new System.Threading.Timer(OnTick, null, System.Threading.Timeout.Infinite, System.Threading.Timeout.Infinite);
    }

    /// <summary>启动轮询（interval 毫秒）。</summary>
    public void Start(int intervalMs = 500)
    {
        _timer.Change(0, intervalMs);
    }

    public void Stop()
    {
        _timer.Change(System.Threading.Timeout.Infinite, System.Threading.Timeout.Infinite);
    }

    /// <summary>设置监听目标列表（进程名 + 可选标题 + 绑定配置）。空/空白进程名的条目会被忽略。</summary>
    public void SetTargets(IEnumerable<AppBinding> targets)
    {
        _targets = targets?.Where(t => !string.IsNullOrWhiteSpace(t.ProcessName)).ToList() ?? new();
        // 立即重算一次，避免状态滞后
        CheckNow();
    }

    public AppBinding? CurrentHit => _currentHit;

    private void OnTick(object? state)
    {
        CheckNow();
    }

    private void CheckNow()
    {
        string proc = "";
        string title = "";
        try
        {
            IntPtr hwnd = GetForegroundWindow();
            if (hwnd != IntPtr.Zero)
            {
                title = GetWindowTitle(hwnd);
                proc = GetProcessName(hwnd);
            }
        }
        catch
        {
            // 忽略查询异常
        }

        AppBinding? hit = null;
        if (!string.IsNullOrEmpty(proc))
        {
            foreach (var t in _targets)
            {
                if (!proc.Equals(t.ProcessName.Trim(), StringComparison.OrdinalIgnoreCase)) continue;
                if (!string.IsNullOrWhiteSpace(t.WindowTitle) &&
                    title.IndexOf(t.WindowTitle.Trim(), StringComparison.OrdinalIgnoreCase) < 0) continue;
                hit = t;
                break;
            }
        }

        if (!IsSame(hit, _currentHit))
        {
            _currentHit = hit;
            MatchChanged?.Invoke(hit, proc, title);
        }
    }

    private static bool IsSame(AppBinding? a, AppBinding? b)
    {
        if (ReferenceEquals(a, b)) return true;
        if (a == null || b == null) return false;
        return a.ProcessName == b.ProcessName && a.WindowTitle == b.WindowTitle && a.ProfileIndex == b.ProfileIndex;
    }

    private static string GetProcessName(IntPtr hwnd)
    {
        GetWindowThreadProcessId(hwnd, out uint pid);
        if (pid == 0) return "";
        using var p = Process.GetProcessById((int)pid);
        return p.ProcessName;
    }

    private static string GetWindowTitle(IntPtr hwnd)
    {
        int len = GetWindowTextLength(hwnd);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 1);
        GetWindowText(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    /// <summary>获取当前前台窗口句柄（供 UI "选择当前前台应用"使用）。</summary>
    public static IntPtr GetForegroundWindowForPicker() => GetForegroundWindow();

    /// <summary>返回指定窗口所属进程名与窗口标题。</summary>
    public static (string process, string title) GetForegroundInfo(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return ("", "");
        return (GetProcessName(hwnd), GetWindowTitle(hwnd));
    }

    public void Dispose()
    {
        _timer.Dispose();
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

    [DllImport("user32.dll")]
    private static extern int GetWindowTextLength(IntPtr hWnd);
}
