namespace HScreenFilter.Models;

/// <summary>持久化到磁盘的完整状态。</summary>
public class ProfileData
{
    public FilterSettings Current { get; set; } = new();
    public bool IsEnabled { get; set; }
    public List<Profile> Profiles { get; set; } = new();
    public int SelectedProfileIndex { get; set; } = -1;

    public int GlobalModifiers { get; set; }
    public int GlobalKey { get; set; }
    public string GlobalDisplay { get; set; } = "";

    public bool AutoStart { get; set; }
    public bool MinimizeToTray { get; set; } = true;

    // 按前台应用自动切换滤镜
    public bool PerAppEnabled { get; set; }
    public string PerAppProcess { get; set; } = "";
    public string PerAppTitle { get; set; } = "";
}
