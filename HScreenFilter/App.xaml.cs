using System;
using System.Threading;
using Microsoft.UI.Xaml;

namespace HScreenFilter;

public partial class App : Application
{
    private Mutex? _mutex;
    private Window? _window;

    public App()
    {
        InitializeComponent();
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
