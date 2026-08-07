namespace HScreenFilter.Models;

/// <summary>单个色系（全部/红/黄/绿/青/蓝/品红）的 HSL 调整参数。</summary>
public class HslChannel
{
    /// <summary>色系名：全部/红/黄/绿/青/蓝/品红。</summary>
    public string Name { get; set; } = "全部";

    /// <summary>该色系的色相旋转量（度，-180..180，0 为中性）。</summary>
    public double Hue { get; set; }

    /// <summary>该色系的饱和度（0..200，100 为中性）。</summary>
    public double Saturation { get; set; } = 100;

    /// <summary>该色系的明度（-100..100，0 为中性）。</summary>
    public double Lightness { get; set; }

    public bool IsNeutral => Hue == 0 && Saturation == 100 && Lightness == 0;

    public HslChannel Clone() => (HslChannel)MemberwiseClone();
}

/// <summary>
/// 一组滤镜参数。
/// 范围说明：
///   Brightness      -100..100（0 为中性）
///   Contrast        0..200（100 为中性）
///   Saturation      0..200（100 为中性）
///   Highlights     -100..100（0 为中性，正值提亮高光）
///   Shadows        -100..100（0 为中性，正值提亮阴影）
///   Temperature    -100..100（0 为中性，负值偏冷、正值偏暖）
///   HSL（主色系=全部，另有 红/黄/绿/青/蓝/品红 六个可单独微调）
/// </summary>
public class FilterSettings
{
    public double Brightness { get; set; }
    public double Contrast { get; set; } = 100;
    public double Saturation { get; set; } = 100;
    public double Highlights { get; set; }
    public double Shadows { get; set; }
    public double Temperature { get; set; }

    /// <summary>当前在面板中选中的色系（全部/红/黄/绿/青/蓝/品红）。</summary>
    public string ActiveHslChannel { get; set; } = HslChannelNames.Master;

    /// <summary>“全部(主)”色系的色相/饱和度/明度。</summary>
    public double Hue { get; set; }
    public double HslSaturation { get; set; } = 100;
    public double Lightness { get; set; }

    /// <summary>六个具体色系（红/黄/绿/青/蓝/品红）各自的 HSL 调整。</summary>
    public List<HslChannel> HslChannels { get; set; } = CreateDefaultChannels();

    public FilterSettings Clone()
    {
        var clone = (FilterSettings)MemberwiseClone();
        clone.HslChannels = new List<HslChannel>(HslChannels.Count);
        foreach (var c in HslChannels)
            clone.HslChannels.Add(c.Clone());
        return clone;
    }

    /// <summary>是否为全中性（不产生任何效果）。</summary>
    public bool IsNeutral =>
        Brightness == 0 && Contrast == 100 && Saturation == 100 &&
        Highlights == 0 && Shadows == 0 && Temperature == 0 &&
        Hue == 0 && HslSaturation == 100 && Lightness == 0 &&
        HslChannels.TrueForAll(c => c.IsNeutral);

    private static List<HslChannel> CreateDefaultChannels()
    {
        var list = new List<HslChannel>();
        foreach (var name in HslChannelNames.ColorNames)
            list.Add(new HslChannel { Name = name });
        return list;
    }
}

/// <summary>HSL 色系名称与各色系的重心色相（度），用于近似分色系偏置。</summary>
public static class HslChannelNames
{
    public const string Master = "全部";
    public const string Red = "红";
    public const string Yellow = "黄";
    public const string Green = "绿";
    public const string Cyan = "青";
    public const string Blue = "蓝";
    public const string Magenta = "品红";

    /// <summary>六个可选的具体色系名称。</summary>
    public static readonly string[] ColorNames = { Red, Yellow, Green, Cyan, Blue, Magenta };

    /// <summary>按名称取重心色相（度）。"全部"返回 0。</summary>
    public static double ReferenceHue(string name) => name switch
    {
        Red => 0,
        Yellow => 60,
        Green => 120,
        Cyan => 180,
        Blue => 240,
        Magenta => 300,
        _ => 0,
    };
}
