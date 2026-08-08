using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace HScreenFilter;

public static class Program
{
    [DllImport("user32.dll")]
    private static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2：按显示器 DPI 感知。
    // 否则 2K/4K 高分屏会被虚拟化为 1080p，导致覆盖层/捕获分辨率不对。
    private static readonly IntPtr DpiAwarenessPerMonitorV2 = new(-4);

    [STAThread]
    private static void Main(string[] args)
    {
        try
        {
            SetProcessDpiAwarenessContext(DpiAwarenessPerMonitorV2);

            WinRT.ComWrappersSupport.InitializeComWrappers();
            Application.Start(p =>
            {
                var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
                SynchronizationContext.SetSynchronizationContext(context);
                _ = new App();
            });
        }
        catch (Exception ex)
        {
            // 启动失败时把异常写入日志，便于排查。
            try
            {
                File.WriteAllText(Path.Combine(AppContext.BaseDirectory, "startup-error.log"), ex.ToString());
            }
            catch
            {
                // 忽略日志写入失败
            }
            throw;
        }
    }
}
