using Windows.System;

namespace HScreenFilter.Models;

/// <summary>把修饰键/虚拟键组合格式化为人类可读文本。</summary>
public static class HotkeyText
{
    /// <summary>Win32 修饰键掩码。</summary>
    public const int MOD_ALT = 0x1;
    public const int MOD_CONTROL = 0x2;
    public const int MOD_SHIFT = 0x4;
    public const int MOD_WIN = 0x8;

    public static string Format(int modifiers, int key)
    {
        if (key == 0) return "";
        var parts = new List<string>();
        if ((modifiers & MOD_WIN) != 0) parts.Add("Win");
        if ((modifiers & MOD_CONTROL) != 0) parts.Add("Ctrl");
        if ((modifiers & MOD_SHIFT) != 0) parts.Add("Shift");
        if ((modifiers & MOD_ALT) != 0) parts.Add("Alt");
        parts.Add(KeyName(key));
        return string.Join("+", parts);
    }

    public static string KeyName(int vk)
    {
        var key = (VirtualKey)vk;
        if (key >= VirtualKey.Number0 && key <= VirtualKey.Number9)
            return ((int)(key - VirtualKey.Number0)).ToString();
        if (key >= VirtualKey.NumberPad0 && key <= VirtualKey.NumberPad9)
            return "Num" + ((int)(key - VirtualKey.NumberPad0)).ToString();
        if (key >= VirtualKey.A && key <= VirtualKey.Z)
            return key.ToString();
        if (key >= VirtualKey.F1 && key <= VirtualKey.F24)
            return key.ToString();

        return key switch
        {
            VirtualKey.Space => "Space",
            VirtualKey.Tab => "Tab",
            VirtualKey.Enter => "Enter",
            VirtualKey.Escape => "Esc",
            VirtualKey.Back => "Backspace",
            VirtualKey.Home => "Home",
            VirtualKey.End => "End",
            VirtualKey.PageUp => "PageUp",
            VirtualKey.PageDown => "PageDown",
            VirtualKey.Insert => "Insert",
            VirtualKey.Delete => "Delete",
            VirtualKey.Left => "←",
            VirtualKey.Right => "→",
            VirtualKey.Up => "↑",
            VirtualKey.Down => "↓",
            _ => key.ToString(),
        };
    }
}
