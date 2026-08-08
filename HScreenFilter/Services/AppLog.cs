using System;
using System.IO;

namespace HScreenFilter.Services;

/// <summary>
/// 简单文件日志（%LocalAppData%\HScreenFilter\debug.log），线程安全。
/// 用于排查闪退/异常：启动、引擎生命周期、渲染线程退出原因、全局崩溃堆栈。
/// </summary>
public static class AppLog
{
    private static readonly object _lock = new();
    private static readonly string _path;

    static AppLog()
    {
        try
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "HScreenFilter");
            Directory.CreateDirectory(dir);
            _path = Path.Combine(dir, "debug.log");
        }
        catch
        {
            _path = Path.Combine(AppContext.BaseDirectory, "debug.log");
        }
    }

    /// <summary>日志文件路径。</summary>
    public static string FilePath => _path;

    /// <summary>写一行日志（自动带时间戳，线程安全，失败静默）。</summary>
    public static void Write(string category, string message)
    {
        try
        {
            lock (_lock)
            {
                File.AppendAllText(_path,
                    $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{category}] {message}{Environment.NewLine}");
            }
        }
        catch
        {
            // 日志写入失败不影响主流程
        }
    }

    /// <summary>清空日志（调试开始时调用）。</summary>
    public static void Clear()
    {
        try
        {
            lock (_lock)
            {
                if (File.Exists(_path)) File.Delete(_path);
            }
        }
        catch
        {
        }
    }
}
