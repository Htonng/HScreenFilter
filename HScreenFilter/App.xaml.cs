using System;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using HScreenFilter.Services;

namespace HScreenFilter;

public partial class App : Application
{
    private Mutex? _mutex;
    private Window? _window;

    public App()
    {
        InitializeComponent();

        // 全局崩溃捕获：任何未处理托管异常都写入日志，便于排查"调整 HSL 闪退"等崩溃。
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            AppLog.Write("CRASH", "AppDomain.UnhandledException:\n" + (e.ExceptionObject as Exception)?.ToString());
        UnhandledException += (_, e) =>
        {
            AppLog.Write("CRASH", "Application.UnhandledException:\n" + e.Exception?.ToString());
            e.Handled = true; // 尽力阻止进程退出，保留现场
        };
        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            AppLog.Write("CRASH", "UnobservedTaskException:\n" + e.Exception);
            e.SetObserved();
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // 单实例：如果已有一个实例在运行，则直接退出，避免两个实例互相覆盖滤镜。
        _mutex = new Mutex(true, "HScreenFilter_SingleInstance", out bool createdNew);
        if (!createdNew)
        {
            Environment.Exit(0);
            return;
        }

        _window = new MainWindow();
        _window.Activate();
    }
}
