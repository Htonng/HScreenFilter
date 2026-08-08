using System;
using System.Text.Json.Serialization;

namespace HScreenFilter.Models;

/// <summary>按应用切换：一个要检测的前台进程与可选配置的绑定。</summary>
public class AppBinding
{
    /// <summary>进程名（不含 .exe，例如 notepad）。</summary>
    public string ProcessName { get; set; } = "";

    /// <summary>窗口标题包含的文字（可选，留空匹配任意标题）。</summary>
    public string WindowTitle { get; set; } = "";

    /// <summary>该进程在前台时自动应用配置在 <see cref="ProfileData.Profiles"/> 中的索引；-1 表示不切换配置（按当前设置）。</summary>
    public int ProfileIndex { get; set; } = -1;

    /// <summary>绑定配置名（仅用于 UI 显示，不持久化）。</summary>
    [JsonIgnore]
    public string ProfileName { get; set; } = "";

    public string DisplayName => string.IsNullOrWhiteSpace(WindowTitle)
        ? ProcessName
        : $"{ProcessName}（标题含「{WindowTitle}」）";

    public string ProfileDisplay => string.IsNullOrEmpty(ProfileName) ? "按当前设置" : $"自动切换配置「{ProfileName}」";
}
