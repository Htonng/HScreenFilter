using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace HScreenFilter.Models;

/// <summary>
/// 一块显示器在 HScreenFilter 中的独立状态（每显示器一份）。
/// 每块显示器可以：独立开关、独立激活配置（n 选 1 随显示器）、独立临时设置。
/// </summary>
public class DisplayState : INotifyPropertyChanged
{
    /// <summary>该显示器在枚举列表中的索引（用于与当前枚举结果匹配；显示器热插拔时可能变化）。</summary>
    public int Index { get; set; }

    /// <summary>该显示器是否应用滤镜（每显示器独立开关）。</summary>
    private bool _isEnabled;
    public bool IsEnabled
    {
        get => _isEnabled;
        set { if (SetProperty(ref _isEnabled, value)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsEnabled))); }
    }

    /// <summary>该显示器当前激活的配置索引；-1 表示无激活（用临时设置）。</summary>
    public int ActiveProfileIndex { get; set; } = -1;

    /// <summary>该显示器无激活配置时使用的临时设置。</summary>
    public FilterSettings Current { get; set; } = new();

    public event PropertyChangedEventHandler? PropertyChanged;

    private bool SetProperty<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        return true;
    }
}
