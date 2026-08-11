using System;
using Microsoft.Win32;

namespace HScreenFilter.Services;

/// <summary>开机自启动（写入 HKCU\...\Run，无需管理员权限）。</summary>
public static class AutoStart
{
    private const string ValueName = "HScreenFilter";
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";

    public static void Set(bool enabled)
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
        if (key == null) return;
        if (enabled)
        {
            var exe = Environment.ProcessPath;
            if (!string.IsNullOrEmpty(exe))
                // 带 --autostart 参数，便于启动时区分“开机自启”与手动启动（用于“自启后自动进入托盘”）。
                key.SetValue(ValueName, $"\"{exe}\" --autostart");
        }
        else
        {
            key.DeleteValue(ValueName, throwOnMissingValue: false);
        }
    }

    public static bool IsEnabled()
    {
        using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: false);
        return key?.GetValue(ValueName) != null;
    }
}
