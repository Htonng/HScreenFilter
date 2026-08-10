using System;
using System.Collections.Generic;

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
    private int _nextId = 1;

    public HotkeyService(MessageWindow window)
    {
        _window = window;
        _window.Message += OnMessage;
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
        if (ok)
        {
            _callbacks[id] = callback;
            _comboIds[(modifiers, key)] = id;
            return true;
        }
        return false;
    }

    public bool Unregister(int id)
    {
        if (_window.Handle == IntPtr.Zero || id == 0 || !_callbacks.TryGetValue(id, out _)) return false;
        _window.InvokeOnMessageThread(() => NativeMethods.UnregisterHotKey(_window.Handle, id));
        _callbacks.Remove(id);
        foreach (var kv in _comboIds)
        {
            if (kv.Value == id)
            {
                _comboIds.Remove(kv.Key);
                break;
            }
        }
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

    public void Dispose()
    {
        _window.Message -= OnMessage;
        if (_window.Handle != IntPtr.Zero)
        {
            foreach (var id in _callbacks.Keys)
                _window.InvokeOnMessageThread(() => NativeMethods.UnregisterHotKey(_window.Handle, id));
        }
        _callbacks.Clear();
        _comboIds.Clear();
    }
}
