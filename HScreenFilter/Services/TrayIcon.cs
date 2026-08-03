using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace HScreenFilter.Services;

/// <summary>
/// 系统托盘图标（Win32 Shell_NotifyIcon）。左键单击显示主窗口，右键弹出菜单。
/// </summary>
public sealed class TrayIcon : IDisposable
{
    private const uint NIM_ADD = 0x00000000;
    private const uint NIM_MODIFY = 0x00000001;
    private const uint NIM_DELETE = 0x00000002;
    private const uint NIF_MESSAGE = 0x00000001;
    private const uint NIF_ICON = 0x00000002;
    private const uint NIF_TIP = 0x00000004;
    private const uint NIF_INFO = 0x00000010;
    private const uint NIIF_INFO = 0x00000001;

    private const int WM_LBUTTONUP = 0x0202;
    private const int WM_RBUTTONUP = 0x0205;
    private const uint WM_TRAYICON = 0x8000 + 0x64;

    private const uint MF_STRING = 0x00000000;
    private const uint MF_SEPARATOR = 0x00000800;
    private const uint TPM_RIGHTBUTTON = 0x0002;
    private const uint TPM_RETURNCMD = 0x0100;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NOTIFYICONDATA
    {
        public int cbSize;
        public IntPtr hWnd;
        public int uID;
        public uint uFlags;
        public uint uCallbackMessage;
        public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string szTip;
        public uint dwState;
        public uint dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string szInfo;
        public uint uTimeoutOrVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string szInfoTitle;
        public uint dwInfoFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X;
        public int Y;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool Shell_NotifyIcon(uint dwMessage, ref NOTIFYICONDATA lpdata);

    [DllImport("user32.dll")]
    private static extern IntPtr CreatePopupMenu();

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool AppendMenu(IntPtr hMenu, uint uFlags, IntPtr uIDNewItem, string? lpNewItem);

    [DllImport("user32.dll")]
    private static extern int TrackPopupMenuEx(IntPtr hmenu, uint fuFlags, int x, int y, IntPtr hwnd, IntPtr lptpm);

    [DllImport("user32.dll")]
    private static extern bool DestroyMenu(IntPtr hMenu);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr hIcon);

    private readonly MessageWindow _window;
    private NOTIFYICONDATA _nid;
    private bool _added;
    private IntPtr _hIcon;
    private readonly Action _onShow;
    private readonly Action _onExit;

    public TrayIcon(MessageWindow window, Action onShow, Action onExit)
    {
        _window = window;
        _onShow = onShow;
        _onExit = onExit;
        _window.Message += OnMessage;
    }

    public void Show()
    {
        if (_window.Handle == IntPtr.Zero) return;

        _hIcon = CreateTrayIcon();

        _nid = new NOTIFYICONDATA
        {
            cbSize = Marshal.SizeOf<NOTIFYICONDATA>(),
            hWnd = _window.Handle,
            uID = 1001,
            uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
            uCallbackMessage = WM_TRAYICON,
            hIcon = _hIcon,
            szTip = "屏幕滤镜",
        };
        _added = Shell_NotifyIcon(NIM_ADD, ref _nid);
    }

    public void ShowBalloon(string title, string message)
    {
        if (!_added) return;
        _nid.uFlags |= NIF_INFO;
        _nid.szInfo = message;
        _nid.szInfoTitle = title;
        _nid.dwInfoFlags = NIIF_INFO;
        _nid.uTimeoutOrVersion = 3000;
        Shell_NotifyIcon(NIM_MODIFY, ref _nid);
    }

    private IntPtr CreateTrayIcon()
    {
        using var bmp = new Bitmap(32, 32);
        using (var g = Graphics.FromImage(bmp))
        {
            g.Clear(Color.Transparent);
            g.SmoothingMode = SmoothingMode.AntiAlias;

            // 圆形底 + 小太阳
            using (var brush = new SolidBrush(Color.FromArgb(255, 0, 120, 212)))
                g.FillEllipse(brush, 4, 4, 24, 24);

            using var white = new SolidBrush(Color.White);
            using var pen = new Pen(Color.White, 2f);
            g.FillEllipse(white, 13, 8, 6, 6);   // 太阳本体
            g.DrawLine(pen, 16, 8, 16, 4);       // 光芒（上）
            g.DrawLine(pen, 16, 20, 16, 24);     // 光芒（下）
            g.DrawLine(pen, 8, 16, 4, 16);       // 光芒（左）
            g.DrawLine(pen, 24, 16, 28, 16);     // 光芒（右）
            g.DrawLine(pen, 10, 10, 7, 7);       // 对角光芒
            g.DrawLine(pen, 22, 10, 25, 7);
            g.DrawLine(pen, 10, 22, 7, 25);
            g.DrawLine(pen, 22, 22, 25, 25);
        }
        return bmp.GetHicon();
    }

    private void OnMessage(int msg, IntPtr wParam, IntPtr lParam)
    {
        if ((uint)msg != WM_TRAYICON) return;
        int mouseMsg = lParam.ToInt32();
        if (mouseMsg == WM_LBUTTONUP)
        {
            _onShow();
        }
        else if (mouseMsg == WM_RBUTTONUP)
        {
            ShowContextMenu();
        }
    }

    private void ShowContextMenu()
    {
        GetCursorPos(out var pt);

        IntPtr menu = CreatePopupMenu();
        AppendMenu(menu, MF_STRING, (IntPtr)1, "显示主窗口");
        AppendMenu(menu, MF_SEPARATOR, IntPtr.Zero, null);
        AppendMenu(menu, MF_STRING, (IntPtr)2, "退出");

        if (_window.Handle != IntPtr.Zero)
        {
            SetForegroundWindow(_window.Handle);
            int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.X, pt.Y, _window.Handle, IntPtr.Zero);
            if (cmd == 1) _onShow();
            else if (cmd == 2) _onExit();
        }

        DestroyMenu(menu);
    }

    public void Dispose()
    {
        _window.Message -= OnMessage;
        if (_added) Shell_NotifyIcon(NIM_DELETE, ref _nid);
        if (_hIcon != IntPtr.Zero) DestroyIcon(_hIcon);
    }
}
