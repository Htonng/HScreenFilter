using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace HScreenFilter.Models;

/// <summary>一个可一键切换的滤镜配置。</summary>
public class Profile : INotifyPropertyChanged
{
    private string _name = "新配置";
    private string _hotkeyDisplay = "";

    public event PropertyChangedEventHandler? PropertyChanged;

    public string Name
    {
        get => _name;
        set => SetProperty(ref _name, value);
    }

    public FilterSettings Settings { get; set; } = new();

    /// <summary>Win32 修饰键掩码：1=Alt, 2=Ctrl, 4=Shift, 8=Win（可为 0，即不要求修饰键）。</summary>
    public int HotkeyModifiers { get; set; }

    /// <summary>Win32 虚拟键码（0 表示未设置）。</summary>
    public int HotkeyKey { get; set; }

    public string HotkeyDisplay
    {
        get => _hotkeyDisplay;
        set => SetProperty(ref _hotkeyDisplay, value);
    }

    public bool HasHotkey => HotkeyKey != 0;

    private void SetProperty<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}
