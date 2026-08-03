using System;
using System.Runtime.InteropServices;

namespace HScreenFilter.Services;

/// <summary>
/// 纯 Win32 消息窗口（message-only 风格）。
/// WinUI 3 没有公开 HwndSource，因此用 RegisterClass + CreateWindowEx 自建隐藏窗口，
/// 用于接收全局热键 WM_HOTKEY 与系统托盘回调消息。
/// </summary>
public sealed class MessageWindow : IDisposable
{
    private const string ClassName = "HScreenFilterMsgWindow";
    private const int GWLP_USERDATA = -21;
    private const int GWLP_WNDPROC = -4;

    private static readonly WndProcDelegate StaticProc = StaticWndProc;

    private IntPtr _hwnd;
    private WndProcDelegate? _instanceProc;
    private GCHandle _selfHandle;

    /// <summary>收到窗口消息时触发：(msg, wParam, lParam)。</summary>
    public event Action<int, IntPtr, IntPtr>? Message;

    public IntPtr Handle => _hwnd;

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    private MessageWindow()
    {
    }

    public static MessageWindow Create()
    {
        var w = new MessageWindow();
        w.CreateNative();
        return w;
    }

    private void CreateNative()
    {
        var wc = new WNDCLASS
        {
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(StaticProc),
            hInstance = GetModuleHandle(null),
            lpszClassName = ClassName,
        };
        if (RegisterClass(ref wc) == 0 && Marshal.GetLastWin32Error() != 1410 /* ERROR_CLASS_ALREADY_EXISTS */)
            throw new InvalidOperationException($"RegisterClass 失败: {Marshal.GetLastWin32Error()}");

        _selfHandle = GCHandle.Alloc(this);
        _hwnd = CreateWindowEx(0, ClassName, string.Empty, 0, 0, 0, 0, 0,
            IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero)
            throw new InvalidOperationException($"CreateWindowEx 失败: {Marshal.GetLastWin32Error()}");

        SetWindowLongPtr(_hwnd, GWLP_USERDATA, GCHandle.ToIntPtr(_selfHandle));
        _instanceProc = InstanceWndProc;
        SetWindowLongPtr(_hwnd, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(_instanceProc));
    }

    private static IntPtr StaticWndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        IntPtr userData = GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (userData == IntPtr.Zero) return DefWindowProc(hWnd, msg, wParam, lParam);
        var target = GCHandle.FromIntPtr(userData).Target as MessageWindow;
        return target?.InstanceWndProc(hWnd, msg, wParam, lParam) ?? DefWindowProc(hWnd, msg, wParam, lParam);
    }

    private IntPtr InstanceWndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        Message?.Invoke((int)msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    public void Dispose()
    {
        if (_hwnd != IntPtr.Zero)
        {
            DestroyWindow(_hwnd);
            _hwnd = IntPtr.Zero;
        }
        if (_selfHandle.IsAllocated) _selfHandle.Free();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASS
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

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern ushort RegisterClass(ref WNDCLASS lpWndClass);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowEx(uint dwExStyle, string lpClassName, string lpWindowName,
        uint dwStyle, int x, int y, int nWidth, int nHeight, IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

    [DllImport("user32.dll")]
    private static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtr", SetLastError = true)]
    private static extern IntPtr SetWindowLongPtr64(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtr", SetLastError = true)]
    private static extern IntPtr GetWindowLongPtr64(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll", EntryPoint = "SetWindowLong", SetLastError = true)]
    private static extern int SetWindowLong32(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

    [DllImport("user32.dll", EntryPoint = "GetWindowLong", SetLastError = true)]
    private static extern IntPtr GetWindowLong32(IntPtr hWnd, int nIndex);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? lpModuleName);

    private static IntPtr SetWindowLongPtr(IntPtr hWnd, int nIndex, IntPtr value)
        => IntPtr.Size == 8 ? SetWindowLongPtr64(hWnd, nIndex, value) : new IntPtr(SetWindowLong32(hWnd, nIndex, value));

    private static IntPtr GetWindowLongPtr(IntPtr hWnd, int nIndex)
        => IntPtr.Size == 8 ? GetWindowLongPtr64(hWnd, nIndex) : GetWindowLong32(hWnd, nIndex);
}
