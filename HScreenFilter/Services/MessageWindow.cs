using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Threading;

namespace HScreenFilter.Services;

/// <summary>
/// 纯 Win32 消息窗口，运行在**独立后台线程**（自带 GetMessage/DispatchMessage 消息循环）。
///
/// 为什么必须独立线程：WinUI 3 没有公开 HwndSource，只能用 RegisterClass + CreateWindowEx
/// 自建隐藏窗口来接收全局热键 WM_HOTKEY 与系统托盘回调。若把消息窗口建在 WinUI 的 UI 线程上，
/// 当其它应用在前台（本应用在后台/最小化到托盘）时，WinUI 的 DispatcherQueue 消息泵**不会可靠地
/// 派发 RegisterHotKey 投递的 WM_HOTKEY**，导致全局快捷键在别的应用在前台时检测不到。
///
/// 解决办法：把消息窗口放到一个独立的 STA 线程，用标准 Win32 消息循环（GetMessage/TranslateMessage/
/// DispatchMessage）处理，无论哪个应用在前台都能收到 WM_HOTKEY。**Message 事件就在这个后台线程上
/// 直接触发**（不再 marshal 到 UI 线程）：
///   - RegisterHotKey/UnregisterHotKey 与托盘的 TrackPopupMenuEx 都**必须**在“拥有窗口的线程”上运行，
///     因此它们也要在这个后台线程上执行（RegisterHotKey 通过 InvokeOnMessageThread 切过来，托盘
///     菜单因为 Message 事件直接在此线程触发所以天然正确）。
///   - 热键/托盘的处理器（ApplyProfile/ToggleGlobal/ShowMainWindow/OnTrayExit 等）内部都各自
///     DispatcherQueue.TryEnqueue 切回 UI 线程，所以后台线程触发事件是线程安全的。
/// </summary>
public sealed class MessageWindow : IDisposable
{
    private const string ClassName = "HScreenFilterMsgWindow";
    private const int GWLP_USERDATA = -21;
    private const int GWLP_WNDPROC = -4;
    private const uint WM_QUIT = 0x0012;
    private const uint WM_APP_INVOKE = 0x8000; // WM_APP：唤醒消息线程执行入队委托

    private static readonly WndProcDelegate StaticProc = StaticWndProc;

    private readonly AutoResetEvent _created = new(false);
    private readonly object _lock = new();
    private readonly ConcurrentQueue<InvokeRequest> _invokeQueue = new();

    private Thread? _thread;
    private IntPtr _hwnd;
    private uint _threadId;
    private WndProcDelegate? _instanceProc;
    private GCHandle _selfHandle;
    private Exception? _createError;
    private bool _disposed;

    /// <summary>收到窗口消息时触发：(msg, wParam, lParam)。回调在后台消息线程上执行。</summary>
    public event Action<int, IntPtr, IntPtr>? Message;

    public IntPtr Handle => _hwnd;

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    private sealed class InvokeRequest
    {
        public Action? Action;
        public Exception? Error;
        public ManualResetEventSlim? Done;
    }

    /// <summary>把委托切到“拥有窗口的后台消息线程”上同步执行（阻塞到执行完成）。
    /// 用于 RegisterHotKey/UnregisterHotKey 这类必须在窗口创建线程上调用、否则返回
    /// ERROR_INVALID_WINDOW_HANDLE(1408) 的 Win32 API。</summary>
    public void InvokeOnMessageThread(Action action)
    {
        if (_thread != null && Thread.CurrentThread == _thread)
        {
            action();
            return;
        }

        var req = new InvokeRequest
        {
            Action = action,
            Done = new ManualResetEventSlim(false),
        };
        _invokeQueue.Enqueue(req);
        IntPtr h = _hwnd;
        if (h == IntPtr.Zero || !PostMessage(h, WM_APP_INVOKE, IntPtr.Zero, IntPtr.Zero))
        {
            // 窗口不存在/投递失败：从队列移除，避免挂起等待。
            if (_invokeQueue.TryDequeue(out var same) && same != req) _invokeQueue.Enqueue(same);
            req.Done.Dispose();
            return;
        }
        req.Done.Wait(3000);
        req.Done.Dispose();
        if (req.Error != null)
            throw req.Error;
    }

    private MessageWindow()
    {
    }

    public static MessageWindow Create()
    {
        var w = new MessageWindow();

        w._thread = new Thread(w.ThreadMain)
        {
            IsBackground = true,
            Name = "HScreenFilterMsgWindow",
        };
        // 提高消息线程优先级，确保 WM_HOTKEY 等消息在前台全屏/游戏进程时仍能及时被调度
        w._thread.Priority = ThreadPriority.AboveNormal;
        w._thread.SetApartmentState(ApartmentState.STA); // 托盘/COM 需要 STA
        w._thread.Start();

        if (!w._created.WaitOne(5000))
            throw new InvalidOperationException("消息窗口线程启动超时");
        if (w._createError != null)
            throw w._createError;

        return w;
    }

    private void ThreadMain()
    {
        try
        {
            // 尽量把当前线程的 Win32 优先级提到最高，增加在游戏/全屏场景下收到 WM_HOTKEY 的概率。
            // 这里使用 SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)。
            try { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); } catch { }

            lock (_lock)
            {
                _threadId = GetCurrentThreadId();
                CreateNative();
            }
        }
        catch (Exception ex)
        {
            _createError = ex;
            _created.Set();
            return;
        }
        _created.Set();

        // 标准 Win32 消息循环：只要线程活着就一直派发消息。
        // WM_HOTKEY / 托盘消息都投递到这个线程的队列，由 DispatchMessage 交给窗口过程。
        MSG msg;
        while (GetMessage(out msg, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref msg);
            DispatchMessage(ref msg);
        }

        // 消息循环退出（收到 WM_QUIT）后的清理
        lock (_lock)
        {
            if (_hwnd != IntPtr.Zero)
            {
                DestroyWindow(_hwnd);
                _hwnd = IntPtr.Zero;
            }
            if (_selfHandle.IsAllocated)
                _selfHandle.Free();
        }
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
        // WM_APP_INVOKE：把消息线程上排队等待执行的委托全部跑完（会设置各自的 Done）。
        if (msg == WM_APP_INVOKE)
        {
            DrainInvokeQueue();
            return IntPtr.Zero;
        }

        // 窗口过程运行在后台消息线程，Message 事件在此线程直接触发。
        // 注意：不能 marshal 到 UI 线程 —— 托盘的 TrackPopupMenuEx 必须在“拥有窗口的线程”上运行，
        // 否则右键菜单失效/卡死。热键/托盘的处理器内部各自 TryEnqueue 回 UI 线程，故线程安全。
        Message?.Invoke((int)msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    private void DrainInvokeQueue()
    {
        while (_invokeQueue.TryDequeue(out var req))
        {
            try { req.Action?.Invoke(); }
            catch (Exception ex) { req.Error = ex; }
            req.Done?.Set();
        }
    }

    public void Dispose()
    {
        bool shouldQuit;
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            shouldQuit = _hwnd != IntPtr.Zero && _threadId != 0;
        }

        // 向消息线程投递 WM_QUIT，让 GetMessage 返回 0 从而退出消息循环。
        if (shouldQuit)
            PostThreadMessage(_threadId, WM_QUIT, IntPtr.Zero, IntPtr.Zero);

        _thread?.Join(2000);
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

    [StructLayout(LayoutKind.Sequential)]
    private struct MSG
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int pt_x;
        public int pt_y;
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

    [DllImport("user32.dll")]
    private static extern int GetMessage(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    private static extern bool TranslateMessage(ref MSG lpMsg);

    [DllImport("user32.dll")]
    private static extern IntPtr DispatchMessage(ref MSG lpMsg);

    [DllImport("user32.dll")]
    private static extern bool PostThreadMessage(uint idThread, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

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

    // 提高消息线程的 Win32 级别优先级（在 ThreadMain 中调用 SetThreadPriority(GetCurrentThread(), ...)）。
    private const int THREAD_PRIORITY_HIGHEST = 2; // Win32 THREAD_PRIORITY_HIGHEST
    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll")]
    private static extern bool SetThreadPriority(IntPtr hThread, int nPriority);

    private static IntPtr SetWindowLongPtr(IntPtr hWnd, int nIndex, IntPtr value)
        => IntPtr.Size == 8 ? SetWindowLongPtr64(hWnd, nIndex, value) : new IntPtr(SetWindowLong32(hWnd, nIndex, value));

    private static IntPtr GetWindowLongPtr(IntPtr hWnd, int nIndex)
        => IntPtr.Size == 8 ? GetWindowLongPtr64(hWnd, nIndex) : GetWindowLong32(hWnd, nIndex);
}
