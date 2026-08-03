using System;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace HScreenFilter.Controls;

/// <summary>
/// 带标题和实时数值的滑块。
/// 用户拖动时触发 <see cref="ValueChangedExternal"/>；
/// 程序设置值请使用 <see cref="SetValueSilently"/>（不会触发外部事件）。
/// </summary>
public sealed partial class FilterSlider : UserControl
{
    public event EventHandler<double>? ValueChangedExternal;

    private bool _internal;

    public static readonly DependencyProperty HeaderProperty =
        DependencyProperty.Register(nameof(Header), typeof(string), typeof(FilterSlider), new PropertyMetadata(string.Empty));

    public string Header
    {
        get => (string)GetValue(HeaderProperty);
        set => SetValue(HeaderProperty, value);
    }

    public static readonly DependencyProperty MinimumProperty =
        DependencyProperty.Register(nameof(Minimum), typeof(double), typeof(FilterSlider), new PropertyMetadata(0.0));

    public double Minimum
    {
        get => (double)GetValue(MinimumProperty);
        set => SetValue(MinimumProperty, value);
    }

    public static readonly DependencyProperty MaximumProperty =
        DependencyProperty.Register(nameof(Maximum), typeof(double), typeof(FilterSlider), new PropertyMetadata(100.0));

    public double Maximum
    {
        get => (double)GetValue(MaximumProperty);
        set => SetValue(MaximumProperty, value);
    }

    public static readonly DependencyProperty ValueProperty =
        DependencyProperty.Register(nameof(Value), typeof(double), typeof(FilterSlider), new PropertyMetadata(0.0, OnValuePropertyChanged));

    public double Value
    {
        get => (double)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    private static void OnValuePropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (FilterSlider)d;
        c.Slider.Value = (double)e.NewValue;
        if (c.ValueText != null)
            c.ValueText.Text = Math.Round((double)e.NewValue).ToString();
    }

    public FilterSlider()
    {
        InitializeComponent();
    }

    /// <summary>程序设置值：不触发 ValueChangedExternal，避免回环。</summary>
    public void SetValueSilently(double value)
    {
        _internal = true;
        Value = value;
        _internal = false;
    }

    private void Slider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_internal) return;
        if (ValueText != null)
            ValueText.Text = Math.Round(e.NewValue).ToString();
        Value = e.NewValue;
        ValueChangedExternal?.Invoke(this, e.NewValue);
    }
}
