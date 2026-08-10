namespace HScreenFilter.Models;

/// <summary>持久化到磁盘的完整状态。</summary>
public class ProfileData
{
    public FilterSettings Current { get; set; } = new();
    public bool IsEnabled { get; set; }
    public List<Profile> Profiles { get; set; } = new();
    public int SelectedProfileIndex { get; set; } = -1;

    /// <summary>当前处于激活状态（开关打开）的配置索引；-1 表示没有激活（此时改动保存至临时配置，下次启动恢复默认）。</summary>
    public int ActiveProfileIndex { get; set; } = -1;

    public int GlobalModifiers { get; set; }
    public int GlobalKey { get; set; }
    public string GlobalDisplay { get; set; } = "";

    public bool AutoStart { get; set; }
    public bool MinimizeToTray { get; set; } = true;

    /// <summary>是否启用 DXGI 捕获引擎（开启时 HSL 调色可用，关闭时用放大镜 API）。</summary>
    public bool UseDxgi { get; set; }

    /// <summary>每显示器独立状态（开关 + 激活配置 + 临时设置）。键由显示器索引关联；索引与枚举顺序对应。</summary>
    public System.Collections.Generic.List<DisplayState> Displays { get; set; } = new();

    /// <summary>界面主题：default（无毛玻璃，兼容老设备）/ mica（毛玻璃）。默认 default。</summary>
    public string Theme { get; set; } = "default";

    /// <summary>UI 与滤镜层是否可被屏幕捕获（OBS 等）。开启后两者设 WDA_MONITOR：
    /// 从 DXGI Desktop Duplication 排除（覆盖层自捕获看不到自己 = 防自反馈），
    /// 但 OBS（WGC 显示器捕获 / 窗口捕获 BitBlt）仍可见。</summary>
    public bool Captureable { get; set; }

    // 按前台应用自动切换滤镜（进程列表 + 绑定配置）
    public bool PerAppEnabled { get; set; }
    public System.Collections.Generic.List<AppBinding> AppBindings { get; set; } = new();
}
