// models.h — 数据模型（与旧版 C# HScreenFilter 的 ProfileData/FilterSettings 等一一对应）。
#pragma once
#include "common.h"
#include "json.h"

namespace hsf {

// ---------------- HSL 色系名称 ----------------
namespace HslChannelNames {
inline const wchar_t* Master   = L"全部";
inline const wchar_t* Red      = L"红";
inline const wchar_t* Orange   = L"橙";
inline const wchar_t* Yellow   = L"黄";
inline const wchar_t* Green    = L"绿";
inline const wchar_t* Cyan     = L"青";
inline const wchar_t* Blue     = L"蓝";
inline const wchar_t* Purple   = L"紫";
inline const wchar_t* Magenta  = L"品红";

// 八个具体色系（顺序与旧版一致）
inline const wchar_t* ColorNames[] = { Red, Orange, Yellow, Green, Cyan, Blue, Purple, Magenta };
constexpr int ColorCount = 8;

// 按名称取重心色相（度）
inline double ReferenceHue(const std::wstring& name)
{
    if (name == Red) return 0;
    if (name == Orange) return 30;
    if (name == Yellow) return 60;
    if (name == Green) return 120;
    if (name == Cyan) return 180;
    if (name == Blue) return 240;
    if (name == Purple) return 270;
    if (name == Magenta) return 300;
    return 0;
}
} // namespace HslChannelNames

// ---------------- 单个色系 ----------------
struct HslChannel
{
    std::wstring Name = HslChannelNames::Master;
    double Hue = 0.0;          // -180..180
    double Saturation = 100.0; // 0..200，100 中性
    double Lightness = 0.0;    // -100..100

    bool IsNeutral() const { return Hue == 0 && Saturation == 100 && Lightness == 0; }
    HslChannel Clone() const { return *this; }
};

// ---------------- 一组滤镜参数 ----------------
struct FilterSettings
{
    double Brightness = 0.0;   // -100..100
    double Contrast = 100.0;   // 0..200
    double Saturation = 100.0; // 0..200
    double Highlights = 0.0;   // -100..100
    double Shadows = 0.0;      // -100..100
    double Temperature = 0.0;  // -100..100

    std::wstring ActiveHslChannel = HslChannelNames::Master;

    double Hue = 0.0;             // 主色系（全部）
    double HslSaturation = 100.0;
    double Lightness = 0.0;

    std::vector<HslChannel> HslChannels; // 八个具体色系

    FilterSettings() { ResetChannels(); }

    void ResetChannels()
    {
        HslChannels.clear();
        for (const wchar_t* name : HslChannelNames::ColorNames)
            HslChannels.push_back(HslChannel{ name, 0.0, 100.0, 0.0 });
    }

    HslChannel* FindChannel(const std::wstring& name)
    {
        for (auto& c : HslChannels)
            if (c.Name == name) return &c;
        return nullptr;
    }
    const HslChannel* FindChannel(const std::wstring& name) const
    {
        for (const auto& c : HslChannels)
            if (c.Name == name) return &c;
        return nullptr;
    }

    FilterSettings Clone() const
    {
        FilterSettings c = *this;
        c.HslChannels = HslChannels; // vector 深拷贝
        return c;
    }

    bool IsNeutral() const
    {
        if (Brightness != 0 || Contrast != 100 || Saturation != 100 ||
            Highlights != 0 || Shadows != 0 || Temperature != 0) return false;
        if (Hue != 0 || HslSaturation != 100 || Lightness != 0) return false;
        for (const auto& c : HslChannels)
            if (!c.IsNeutral()) return false;
        return true;
    }

    // 旧版 IsDefaultSettings 语义（用于迁移判断，只查基础项）
    bool IsDefaultBasic() const
    {
        return Hue == 0 && HslSaturation == 100 && Lightness == 0 &&
               Saturation == 100 && Contrast == 100 && Brightness == 0 &&
               Temperature == 0 && Highlights == 0 && Shadows == 0;
    }

    JsonValue ToJson() const;
    void FromJson(const JsonValue& j);
};

// ---------------- 显示器 ----------------
struct DisplayMonitor
{
    std::wstring DeviceName;
    int X = 0, Y = 0, Width = 0, Height = 0;
    bool IsPrimary = false;

    std::wstring Label() const
    {
        return IsPrimary
            ? Format(L"主显示器 (%d×%d)", Width, Height)
            : Format(L"显示器 %d×%d", Width, Height);
    }
};

// ---------------- 每显示器状态 ----------------
struct DisplayState
{
    int Index = 0;
    bool IsEnabled = false;
    int ActiveProfileIndex = -1;
    FilterSettings Current;
    bool UseVsync = false;

    JsonValue ToJson() const;
    void FromJson(const JsonValue& j);
};

// ---------------- 配置 ----------------
struct Profile
{
    std::wstring Name = L"新配置";
    FilterSettings Settings;
    int HotkeyModifiers = 0; // 1=Alt 2=Ctrl 4=Shift 8=Win
    int HotkeyKey = 0;       // 虚拟键码，0=未设置
    std::wstring HotkeyDisplay;
    bool IsActive = false;
    bool UseDxgi = true;

    bool HasHotkey() const { return HotkeyKey != 0; }
    // UI 徽标文字：按当前实现命名为 LUT（3D LUT 逐像素引擎）；UseDxgi 字段名保持兼容旧数据
    std::wstring ApiText() const { return UseDxgi ? L"LUT" : L"放大镜"; }

    JsonValue ToJson() const;
    void FromJson(const JsonValue& j);
};

// ---------------- 按应用绑定 ----------------
struct AppBinding
{
    std::wstring ProcessName;
    std::wstring WindowTitle;
    int ProfileIndex = -1;

    std::wstring DisplayName() const
    {
        if (WindowTitle.empty())
            return ProcessName;
        return ProcessName + L"（标题含「" + WindowTitle + L"」）";
    }
    std::wstring ProfileDisplay(const std::wstring& profileName) const
    {
        return profileName.empty() ? L"按当前设置" : L"自动切换配置「" + profileName + L"」";
    }

    JsonValue ToJson() const;
    void FromJson(const JsonValue& j);
};

// ---------------- 持久化根数据 ----------------
struct ProfileData
{
    FilterSettings Current;
    bool IsEnabled = false;
    std::vector<Profile> Profiles;
    int SelectedProfileIndex = -1;
    int ActiveProfileIndex = -1;
    int GlobalModifiers = 0;
    int GlobalKey = 0;
    std::wstring GlobalDisplay;
    bool AutoStart = false;
    bool MinimizeToTray = true;
    bool UseDxgi = false;
    std::vector<DisplayState> Displays;
    std::wstring Theme = L"default";
    bool Captureable = false;
    bool PerAppEnabled = false;
    std::vector<AppBinding> AppBindings;

    JsonValue ToJson() const;
    void FromJson(const JsonValue& j);
};

// ---------------- 快捷键文本 ----------------
namespace HotkeyText {
// 修饰键掩码（MOD_ALT/MOD_CONTROL/MOD_SHIFT 与系统宏一致，这里用别名避免宏冲突）
inline constexpr int ModAlt = 0x1;
inline constexpr int ModCtrl = 0x2;
inline constexpr int ModShift = 0x4;
inline constexpr int ModWin = 0x8;

std::wstring KeyName(int vk);
std::wstring Format(int modifiers, int key);
} // namespace HotkeyText

} // namespace hsf
