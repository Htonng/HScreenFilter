using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;
using Microsoft.UI.Xaml;

namespace HScreenFilter.Models;

/// <summary>一个可一键切换的滤镜配置。</summary>
public class Profile : INotifyPropertyChanged
{
    private string _name = "新配置";
    private string _hotkeyDisplay = "";
    private bool _isActive;
    private int _hotkeyKey;

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
    public int HotkeyKey
    {
        get => _hotkeyKey;
        set
        {
            if (SetProperty(ref _hotkeyKey, value))
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HotkeyVisibility)));
        }
    }

    public string HotkeyDisplay
    {
        get => _hotkeyDisplay;
        set
        {
            if (SetProperty(ref _hotkeyDisplay, value))
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HotkeyVisibility)));
        }
    }

    public bool HasHotkey => HotkeyKey != 0;

    /// <summary>该配置是否处于「已激活/已应用」状态（列表中开关打开）。n 选 1，由界面管理。</summary>
    public bool IsActive
    {
        get => _isActive;
        set => SetProperty(ref _isActive, value);
    }

    private bool _useDxgi = true;

    /// <summary>该配置使用的滤镜引擎：true=DXGI 逐像素着色器（HSL 调色可用），false=放大镜 API（HSL 不可用）。
    /// 每个配置记忆自己的 DXGI 开关，切换配置时恢复。</summary>
    public bool UseDxgi
    {
        get => _useDxgi;
        set
        {
            if (SetProperty(ref _useDxgi, value))
            {
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ApiText)));
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ApiVisibility)));
            }
        }
    }

    /// <summary>该配置使用的 API 徽标文字（DXGI / 放大镜），显示在配置列表快捷键左侧。</summary>
    [JsonIgnore]
    public string ApiText => UseDxgi ? "DXGI" : "放大镜";

    [JsonIgnore]
    public Visibility ApiVisibility => Visibility.Visible;

    /// <summary>是否显示快捷键徽标（未绑定快捷键时隐藏）。</summary>
    public Visibility HotkeyVisibility => HotkeyKey != 0 ? Visibility.Visible : Visibility.Collapsed;

    private bool SetProperty<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }
}
