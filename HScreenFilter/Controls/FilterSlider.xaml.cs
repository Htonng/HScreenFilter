using System;
using System.Collections.Generic;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;
using Windows.UI;

namespace HScreenFilter.Controls;

/// <summary>
/// 带标题和实时数值的滑块。
/// 用户拖动时触发 <see cref="ValueChangedExternal"/>；
/// 程序设置值请使用 <see cref="SetValueSilently"/>（不会触发外部事件）。
/// 设置 <see cref="Spectrum"/>（逗号分隔的十六进制颜色）可在滑块轨道上渲染 Photoshop 式彩色渐变。
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

    /// <summary>
    /// 逗号分隔的十六进制颜色（如 "#FF0000,#FFFF00,#00FF00"），均匀分布在滑块轨道上形成渐变。
    /// 为空或无法解析时不显示彩色轨道。
    /// </summary>
    public static readonly DependencyProperty SpectrumProperty =
        DependencyProperty.Register(nameof(Spectrum), typeof(string), typeof(FilterSlider), new PropertyMetadata(string.Empty, OnSpectrumPropertyChanged));

    public string Spectrum
    {
        get => (string)GetValue(SpectrumProperty);
        set => SetValue(SpectrumProperty, value);
    }

    private static void OnValuePropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (FilterSlider)d;
        c.Slider.Value = (double)e.NewValue;
        if (c.ValueText != null)
            c.ValueText.Text = Math.Round((double)e.NewValue).ToString();
    }

    private static void OnSpectrumPropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (FilterSlider)d;
        c.ApplySpectrum((string)e.NewValue);
    }

    public FilterSlider()
    {
        InitializeComponent();
        ApplySpectrum(Spectrum);
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

    private void ApplySpectrum(string? spectrum)
    {
        if (SpectrumBar == null) return;

        var stops = new List<GradientStop>();
        if (!string.IsNullOrWhiteSpace(spectrum))
        {
            var parts = spectrum.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            int count = parts.Length;
            for (int i = 0; i < count; i++)
            {
                if (TryParseColor(parts[i], out Color color))
                {
                    double offset = count > 1 ? i / (double)(count - 1) : 0.0;
                    stops.Add(new GradientStop { Color = color, Offset = offset });
                }
            }
        }

        if (stops.Count >= 2)
        {
            var brush = new LinearGradientBrush { StartPoint = new Point(0, 0.5), EndPoint = new Point(1, 0.5) };
            foreach (var stop in stops)
                brush.GradientStops.Add(stop);
            SpectrumBar.Background = brush;
            SpectrumBar.Visibility = Visibility.Visible;
        }
        else
        {
            SpectrumBar.Background = null;
            SpectrumBar.Visibility = Visibility.Collapsed;
        }
    }

    private static bool TryParseColor(string text, out Color color)
    {
        color = default;
        text = text.Trim();
        if (text.StartsWith("#", StringComparison.Ordinal)) text = text.Substring(1);
        if (text.Length != 6) return false;
        if (!uint.TryParse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint rgb)) return false;
        color = Color.FromArgb(0xFF, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);
        return true;
    }
}
