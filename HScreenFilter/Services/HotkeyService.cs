using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace HScreenFilter.Services;

/// <summary>
/// 全局热键服务：通过 Win32 RegisterHotKey 注册全局快捷键，
/// 由 MessageWindow 接收 WM_HOTKEY 后分发回调。
/// </summary>
public sealed class HotkeyService : IDisposable
{
    private const uint MOD_NOREPEAT = 0x4000;

    private readonly MessageWindow _window;
    private readonly Dictionary<int, Action> _callbacks = new();
    private readonly Dictionary<(int, int), int> _comboIds = new();
    private readonly HashSet<int> _registeredWithWin32 = new();
    private int _nextId = 1;

    // Low-level keyboard hook
    private IntPtr _hookHandle = IntPtr.Zero;
    private NativeMethods.LowLevelKeyboardProc? _hookProc;
    private readonly object _hookLock = new();

    public HotkeyService(MessageWindow window)
    {
        _window = window;
        _window.Message += OnMessage;
        // 安装低级键盘钩子作为注册失败或游戏场景的备选方案
        TryInstallKeyboardHook();
    }

    /// <summary>注册全局热键。组合键已被占用时返回 false。</summary>
    public bool Register(int modifiers, int key, Action callback, out int id)
    {
        id = 0;
        if (_window.Handle == IntPtr.Zero || key == 0) return false;
        if (_comboIds.ContainsKey((modifiers, key))) return false;

        id = _nextId++;
        int regId = id; // 复制到局部变量，避免在 lambda 中捕获 out 参数（CS1628）
        uint fsMods = (uint)modifiers | MOD_NOREPEAT;
        bool ok = false;
        // RegisterHotKey 必须在“拥有窗口的消息线程”上调用，否则返回 ERROR_INVALID_WINDOW_HANDLE(1408)。
        _window.InvokeOnMessageThread(() =>
            ok = NativeMethods.RegisterHotKey(_window.Handle, regId, fsMods, (uint)key));

        // 无论 RegisterHotKey 是否成功，都把回调和组合记录在本地；若 RegisterHotKey 成功也保留，这样 Unregister 时可一并取消
        _callbacks[id] = callback;
        _comboIds[(modifiers, key)] = id;
        if (ok) _registeredWithWin32.Add(id);
        else
        {
            // 确保键盘钩子已安装以在游戏/全屏场景时仍可捕获按键
            TryInstallKeyboardHook();
        }
        return true;
    }

    public bool Unregister(int id)
    {
        if (id == 0 || !_callbacks.TryGetValue(id, out _)) return false;
        if (_window.Handle != IntPtr.Zero && _registeredWithWin32.Contains(id))
            _window.InvokeOnMessageThread(() => NativeMethods.UnregisterHotKey(_window.Handle, id));
        _registeredWithWin32.Remove(id);
        _callbacks.Remove(id);
        foreach (var kv in _comboIds)
        {
            if (kv.Value == id)
            {
                _comboIds.Remove(kv.Key);
                break;
            }
        }

        // 若无剩余本地组合，则卸载钩子
        if (_comboIds.Count == 0)
            TryUninstallKeyboardHook();

        return true;
    }

    private void OnMessage(int msg, IntPtr wParam, IntPtr lParam)
    {
        if (msg == NativeMethods.WM_HOTKEY)
        {
            int id = wParam.ToInt32();
            if (_callbacks.TryGetValue(id, out var callback))
                callback();
        }
    }

    private void TryInstallKeyboardHook()
    {
        lock (_hookLock)
        {
            if (_hookHandle != IntPtr.Zero) return;
            try
            {
                _hookProc = HookCallback;
                IntPtr mod = NativeMethods.GetModuleHandle(null);
                _hookHandle = NativeMethods.SetWindowsHookEx(NativeMethods.WH_KEYBOARD_LL, _hookProc, mod, 0);
                // ignore errors; hook may fail in restricted environments
            }
            catch { _hookHandle = IntPtr.Zero; }
        }
    }

    private void TryUninstallKeyboardHook()
    {
        lock (_hookLock)
        {
            if (_hookHandle == IntPtr.Zero) return;
            try { NativeMethods.UnhookWindowsHookEx(_hookHandle); } catch { }
            _hookHandle = IntPtr.Zero;
            _hookProc = null;
        }
    }

    private IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        try
        {
            if (nCode >= 0)
            {
                int msg = wParam.ToInt32();
                bool keyDown = msg == NativeMethods.WM_KEYDOWN || msg == NativeMethods.WM_SYSKEYDOWN;
                if (keyDown)
                {
                    int vk = Marshal.ReadInt32(lParam); // KBDLLHOOKSTRUCT.vkCode

                    int mods = 0;
                    if ((NativeMethods.GetAsyncKeyState(0x11) & 0x8000) != 0) mods |= Models.HotkeyText.MOD_CONTROL; // VK_CONTROL
                    if ((NativeMethods.GetAsyncKeyState(0x10) & 0x8000) != 0) mods |= Models.HotkeyText.MOD_SHIFT;   // VK_SHIFT
                    if ((NativeMethods.GetAsyncKeyState(0x12) & 0x8000) != 0) mods |= Models.HotkeyText.MOD_ALT;     // VK_MENU
                    if ((NativeMethods.GetAsyncKeyState(0x5B) & 0x8000) != 0 || (NativeMethods.GetAsyncKeyState(0x5C) & 0x8000) != 0) mods |= Models.HotkeyText.MOD_WIN;

                    if (_comboIds.TryGetValue((mods, vk), out var id))
                    {
                        // 关键：若该组合已成功注册到 RegisterHotKey（Win32），系统会自行投递 WM_HOTKEY，
                        // 这里必须跳过 —— 否则同一按键会既走 Win32 又走钩子，回调触发两次（toggle 两次=开→关），
                        // 导致滤镜启用时一直抽搐/合不上。钩子只负责处理 RegisterHotKey 失败（如组合被占用）的兜底方案。
                        bool registeredWithWin32;
                        lock (_hookLock) { registeredWithWin32 = _registeredWithWin32.Contains(id); }
                        if (registeredWithWin32) return NativeMethods.CallNextHookEx(_hookHandle, nCode, wParam, lParam);

                        // Post WM_HOTKEY to the message window so the existing WM_HOTKEY handling path runs on the message thread.
                        try
                        {
                            if (_window.Handle != IntPtr.Zero)
                            {
                                NativeMethods.PostMessage(_window.Handle, NativeMethods.WM_HOTKEY, new IntPtr(id), IntPtr.Zero);
                            }
                            else if (_callbacks.TryGetValue(id, out var cb))
                            {
                                // Fallback: directly invoke if message window is not available (best-effort)
                                try { cb(); } catch { }
                            }
                        }
                        catch { }
                    }
                }
            }
        }
        catch { }
        return NativeMethods.CallNextHookEx(_hookHandle, nCode, wParam, lParam);
    }

    public void Dispose()
    {
        _window.Message -= OnMessage;
        if (_window.Handle != IntPtr.Zero)
        {
            foreach (var id in _registeredWithWin32)
                _window.InvokeOnMessageThread(() => NativeMethods.UnregisterHotKey(_window.Handle, id));
        }
        _registeredWithWin32.Clear();
        _callbacks.Clear();
        _comboIds.Clear();
        TryUninstallKeyboardHook();
    }
}
