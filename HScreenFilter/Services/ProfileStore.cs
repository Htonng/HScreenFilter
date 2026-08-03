using System;
using System.IO;
using System.Text.Json;
using HScreenFilter.Models;

namespace HScreenFilter.Services;

/// <summary>把用户状态（当前参数、配置列表、快捷键）保存为 %LocalAppData%\HScreenFilter\profiles.json。</summary>
public sealed class ProfileStore
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };
    private readonly string _dir;
    private readonly string _file;

    public ProfileStore()
    {
        _dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HScreenFilter");
        Directory.CreateDirectory(_dir);
        _file = Path.Combine(_dir, "profiles.json");
    }

    public string FilePath => _file;

    public ProfileData Load()
    {
        try
        {
            if (File.Exists(_file))
            {
                var data = JsonSerializer.Deserialize<ProfileData>(File.ReadAllText(_file), Options);
                if (data != null) return data;
            }
        }
        catch { /* 损坏则回退到默认值 */ }
        return new ProfileData();
    }

    public void Save(ProfileData data)
    {
        try
        {
            File.WriteAllText(_file, JsonSerializer.Serialize(data, Options));
        }
        catch { }
    }
}
