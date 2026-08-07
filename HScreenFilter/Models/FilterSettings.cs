namespace HScreenFilter.Models;

/// <summary>
/// 一组滤镜参数。
/// 范围说明：
///   Brightness      -100..100（0 为中性）
///   Contrast        0..200（100 为中性）
///   Saturation      0..200（100 为中性）
///   Highlights     -100..100（0 为中性，正值提亮高光）
///   Shadows        -100..100（0 为中性，正值提亮阴影）
///   Temperature    -100..100（0 为中性，负值偏冷、正值偏暖）
///   Hue             -180..180（0 为中性，单位：度，色相旋转）
///   HslSaturation   0..200（100 为中性）
///   Lightness      -100..100（0 为中性，正值偏白、负值偏黑）
/// </summary>
public class FilterSettings
{
    public double Brightness { get; set; }
    public double Contrast { get; set; } = 100;
    public double Saturation { get; set; } = 100;
    public double Highlights { get; set; }
    public double Shadows { get; set; }
    public double Temperature { get; set; }
    public double Hue { get; set; }
    public double HslSaturation { get; set; } = 100;
    public double Lightness { get; set; }

    public FilterSettings Clone() => (FilterSettings)MemberwiseClone();

    /// <summary>是否为全中性（不产生任何效果）。</summary>
    public bool IsNeutral =>
        Brightness == 0 && Contrast == 100 && Saturation == 100 &&
        Highlights == 0 && Shadows == 0 && Temperature == 0 &&
        Hue == 0 && HslSaturation == 100 && Lightness == 0;
}
