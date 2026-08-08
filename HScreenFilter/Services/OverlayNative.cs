using System;
using System.Runtime.InteropServices;

namespace HScreenFilter.Services;

/// <summary>
/// 供着色器覆盖层使用的 Win32 原生互操作（窗口样式、置顶、屏幕捕获排除、显示器信息）。
/// </summary>
internal static class OverlayNative
{
    public const int GWL_EXSTYLE = -20;
    public const int GWL_STYLE = -16;

    // 去除标题栏/边框所需的窗口样式位
    public const long WS_CAPTION = 0x00C00000L;   // 标题栏
    public const long WS_THICKFRAME = 0x00040000L; // 可调整大小的边框

    public const uint WS_EX_TRANSPARENT = 0x00000020;   // 点击穿透（配合 WS_EX_LAYERED 跨进程生效）
    public const uint WS_EX_LAYERED = 0x00080000;      // 分层窗口，使点击真正穿透到下层其它进程窗口
    public const uint WS_EX_NOACTIVATE = 0x08000000;    // 不抢占焦点
    public const uint WS_EX_TOOLWINDOW = 0x00000080;    // 不进 Alt+Tab

    public const uint WDA_NONE = 0x00000000;               // 无排除（正常可被捕获）
    public const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011; // 排除在屏幕捕获/截图之外（防自反馈）
    public const uint WDA_MONITOR = 0x00000001;            // 仅从 DXGI Desktop Duplication 排除；BitBlt/WGC 仍可见（可被 OBS 捕获）

    // 点击穿透：WM_NCHITTEST 返回 HTTRANSPARENT(-1)，让鼠标事件穿透到下层窗口
    public const uint WM_NCHITTEST = 0x0084;
    public const long HTTRANSPARENT = -1L;

    public static readonly IntPtr HWND_TOPMOST = new(-1);
    public static readonly IntPtr HWND_NOTOPMOST = new(-2);

    public const uint SWP_NOSIZE = 0x0001;
    public const uint SWP_NOMOVE = 0x0002;
    public const uint SWP_NOACTIVATE = 0x0010;
    public const uint SWP_SHOWWINDOW = 0x0040;

    public const uint MONITOR_DEFAULTTOPRIMARY = 0x00000001;

    // 原始 Win32 窗口（用于全 Vortice D3D11 渲染管线的覆盖层）
    public const uint WS_POPUP = 0x80000000;
    public const uint WS_VISIBLE = 0x10000000;
    public const uint WS_CLIPSIBLINGS = 0x04000000;
    public const uint WS_EX_TOPMOST = 0x00000008;
    public const uint WM_DESTROY = 0x0002;
    public const uint WM_NCDESTROY = 0x0082;

    /// <summary>窗口过程委托（必须由调用方静态持有，防止被 GC）。</summary>
    public delegate IntPtr WNDPROC(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WNDCLASS
    {
        public uint style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        public string? lpszMenuName;
        public string lpszClassName;
    }

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern ushort RegisterClassW(ref WNDCLASS lpWndClass);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowExW(
        uint dwExStyle, string lpClassName, string lpWindowName, uint dwStyle,
        int x, int y, int nWidth, int nHeight,
        IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

    [DllImport("user32.dll")]
    public static extern IntPtr DefWindowProcW(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern void PostQuitMessage(int nExitCode);

    [DllImport("kernel32.dll")]
    public static extern IntPtr GetModuleHandle(string? lpModuleName);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetCursorPos(ref POINT lpPoint);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hwnd);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left, Top, Right, Bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [DllImport("user32.dll")]
    public static extern bool SetWindowDisplayAffinity(IntPtr hWnd, uint dwAffinity);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowDisplayAffinity(IntPtr hWnd, ref uint dwAffinity);

    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);

    public const uint GA_ROOT = 2; // 根窗口（最外层顶层窗口）

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    public static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int X, int Y, int cx, int cy, uint uFlags);

    /// <summary>
    /// 设置/取消窗口置顶。必须同时修改 WS_EX_TOPMOST 样式位再 SetWindowPos，
    /// 否则对分层窗口（WS_EX_LAYERED）和 WinUI 窗口，单独 SetWindowPos(HWND_TOPMOST) 无效（返回 True 但位不变）。
    /// </summary>
    public static void SetTopmost(IntPtr hwnd, bool topmost)
    {
        long ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE).ToInt64();
        if (topmost) ex |= WS_EX_TOPMOST;
        else ex &= ~WS_EX_TOPMOST;
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, (IntPtr)ex);
        SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // 前台窗口操作：用于把覆盖层显示后把激活/焦点还给之前的窗口，
    // 避免覆盖层拿到焦点后 WinUI 绘制整屏黄色焦点框。
    public const int SW_SHOWNOACTIVATE = 4;   // 显示但不激活
    public const int SW_HIDE = 0;

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr MonitorFromPoint(POINT pt, uint dwFlags);

    [DllImport("user32.dll")]
    public static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);

    // 窗口子类化（用于点击穿透）。注意：委托必须由调用方长期持有，防止被 GC。
    public delegate IntPtr SUBCLASSPROC(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam, UIntPtr uIdSubclass, UIntPtr dwRefData);

    [DllImport("comctl32.dll")]
    public static extern bool SetWindowSubclass(IntPtr hWnd, SUBCLASSPROC pfnSubclass, UIntPtr uIdSubclass, UIntPtr dwRefData);

    [DllImport("comctl32.dll")]
    public static extern IntPtr DefSubclassProc(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam);
}
