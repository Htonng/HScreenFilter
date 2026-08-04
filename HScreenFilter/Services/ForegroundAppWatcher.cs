using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace HScreenFilter.Services;

/// <summary>
/// 前台应用监听器：轮询当前前台窗口，判断是否命中目标应用。
/// 目标可通过进程名（不含 .exe）或窗口标题（子串匹配）指定。
/// 命中状态变化时触发事件（在调用线程上）。
/// </summary>
public sealed class ForegroundAppWatcher
{
    private readonly System.Threading.Timer _timer;
    private string _targetProcess = "";
    private string _targetTitle = "";
    private bool _currentMatch;

    /// <summary>命中状态变化：(isMatch, processName, windowTitle)</summary>
    public event Action<bool, string, string>? MatchChanged;

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

    /// <summary>设置监听目标。processName 可为空表示不按进程匹配；title 可为空表示不按标题匹配。</summary>
    public void SetTarget(string processName, string title)
    {
        _targetProcess = string.IsNullOrWhiteSpace(processName) ? "" : processName.Trim().ToLowerInvariant();
        _targetTitle = string.IsNullOrWhiteSpace(title) ? "" : title.Trim();
        // 立即重算一次，避免状态滞后
        CheckNow();
    }

    public bool CurrentMatch => _currentMatch;

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

        bool match = false;
        if (!string.IsNullOrEmpty(_targetProcess) && !string.IsNullOrEmpty(proc))
        {
            match = proc.Equals(_targetProcess, StringComparison.OrdinalIgnoreCase);
        }
        else if (!string.IsNullOrEmpty(_targetTitle) && !string.IsNullOrEmpty(title))
        {
            match = title.IndexOf(_targetTitle, StringComparison.OrdinalIgnoreCase) >= 0;
        }
        else if (string.IsNullOrEmpty(_targetProcess) && string.IsNullOrEmpty(_targetTitle))
        {
            match = false;
        }

        if (match != _currentMatch)
        {
            _currentMatch = match;
            MatchChanged?.Invoke(match, proc, title);
        }
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
