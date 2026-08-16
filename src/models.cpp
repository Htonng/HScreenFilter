#include "models.h"

namespace hsf {

// ---------------- FilterSettings ----------------
JsonValue FilterSettings::ToJson() const
{
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"Brightness", JsonValue::NumValue(Brightness));
    j.Set(L"Contrast", JsonValue::NumValue(Contrast));
    j.Set(L"Saturation", JsonValue::NumValue(Saturation));
    j.Set(L"Highlights", JsonValue::NumValue(Highlights));
    j.Set(L"Shadows", JsonValue::NumValue(Shadows));
    j.Set(L"Temperature", JsonValue::NumValue(Temperature));
    j.Set(L"ActiveHslChannel", JsonValue::StrValue(ActiveHslChannel));
    j.Set(L"Hue", JsonValue::NumValue(Hue));
    j.Set(L"HslSaturation", JsonValue::NumValue(HslSaturation));
    j.Set(L"Lightness", JsonValue::NumValue(Lightness));

    JsonValue arr = JsonValue::ArrayValue();
    for (const auto& c : HslChannels)
    {
        JsonValue cj = JsonValue::ObjectValue();
        cj.Set(L"Name", JsonValue::StrValue(c.Name));
        cj.Set(L"Hue", JsonValue::NumValue(c.Hue));
        cj.Set(L"Saturation", JsonValue::NumValue(c.Saturation));
        cj.Set(L"Lightness", JsonValue::NumValue(c.Lightness));
        arr.arr.push_back(std::move(cj));
    }
    j.Set(L"HslChannels", std::move(arr));
    return j;
}

void FilterSettings::FromJson(const JsonValue& j)
{
    Brightness = j.GetNumber(L"Brightness", 0);
    Contrast = j.GetNumber(L"Contrast", 100);
    Saturation = j.GetNumber(L"Saturation", 100);
    Highlights = j.GetNumber(L"Highlights", 0);
    Shadows = j.GetNumber(L"Shadows", 0);
    Temperature = j.GetNumber(L"Temperature", 0);
    ActiveHslChannel = j.GetString(L"ActiveHslChannel", HslChannelNames::Master);
    Hue = j.GetNumber(L"Hue", 0);
    HslSaturation = j.GetNumber(L"HslSaturation", 100);
    Lightness = j.GetNumber(L"Lightness", 0);

    ResetChannels();
    const JsonValue* arr = j.Get(L"HslChannels");
    if (arr && arr->IsArray() && !arr->arr.empty())
    {
        HslChannels.clear();
        for (const auto& cj : arr->arr)
        {
            if (!cj.IsObject()) continue;
            HslChannel c;
            c.Name = cj.GetString(L"Name", HslChannelNames::Master);
            c.Hue = cj.GetNumber(L"Hue", 0);
            c.Saturation = cj.GetNumber(L"Saturation", 100);
            c.Lightness = cj.GetNumber(L"Lightness", 0);
            HslChannels.push_back(c);
        }
        // 保证八个色系齐全（旧数据可能缺项）
        for (const wchar_t* name : HslChannelNames::ColorNames)
        {
            bool found = false;
            for (const auto& c : HslChannels)
                if (c.Name == name) { found = true; break; }
            if (!found) HslChannels.push_back(HslChannel{ name, 0.0, 100.0, 0.0 });
        }
    }
}

// ---------------- DisplayState ----------------
JsonValue DisplayState::ToJson() const
{
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"Index", JsonValue::NumValue((double)Index));
    j.Set(L"IsEnabled", JsonValue::BoolValue(IsEnabled));
    j.Set(L"ActiveProfileIndex", JsonValue::NumValue((double)ActiveProfileIndex));
    j.Set(L"Current", Current.ToJson());
    j.Set(L"UseVsync", JsonValue::BoolValue(UseVsync));
    return j;
}

void DisplayState::FromJson(const JsonValue& j)
{
    Index = (int)j.GetNumber(L"Index", 0);
    IsEnabled = j.GetBool(L"IsEnabled", false);
    ActiveProfileIndex = (int)j.GetNumber(L"ActiveProfileIndex", -1);
    UseVsync = j.GetBool(L"UseVsync", false);
    const JsonValue* cur = j.Get(L"Current");
    if (cur && cur->IsObject()) Current.FromJson(*cur);
}

// ---------------- Profile ----------------
JsonValue Profile::ToJson() const
{
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"Name", JsonValue::StrValue(Name));
    j.Set(L"Settings", Settings.ToJson());
    j.Set(L"HotkeyModifiers", JsonValue::NumValue((double)HotkeyModifiers));
    j.Set(L"HotkeyKey", JsonValue::NumValue((double)HotkeyKey));
    j.Set(L"HotkeyDisplay", JsonValue::StrValue(HotkeyDisplay));
    j.Set(L"IsActive", JsonValue::BoolValue(IsActive));
    j.Set(L"UseDxgi", JsonValue::BoolValue(UseDxgi));
    return j;
}

void Profile::FromJson(const JsonValue& j)
{
    Name = j.GetString(L"Name", L"新配置");
    HotkeyModifiers = (int)j.GetNumber(L"HotkeyModifiers", 0);
    HotkeyKey = (int)j.GetNumber(L"HotkeyKey", 0);
    HotkeyDisplay = j.GetString(L"HotkeyDisplay", L"");
    IsActive = j.GetBool(L"IsActive", false);
    UseDxgi = j.GetBool(L"UseDxgi", true);
    const JsonValue* s = j.Get(L"Settings");
    if (s && s->IsObject()) Settings.FromJson(*s);
}

// ---------------- AppBinding ----------------
JsonValue AppBinding::ToJson() const
{
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"ProcessName", JsonValue::StrValue(ProcessName));
    j.Set(L"WindowTitle", JsonValue::StrValue(WindowTitle));
    j.Set(L"ProfileIndex", JsonValue::NumValue((double)ProfileIndex));
    return j;
}

void AppBinding::FromJson(const JsonValue& j)
{
    ProcessName = j.GetString(L"ProcessName", L"");
    WindowTitle = j.GetString(L"WindowTitle", L"");
    ProfileIndex = (int)j.GetNumber(L"ProfileIndex", -1);
}

// ---------------- ProfileData ----------------
JsonValue ProfileData::ToJson() const
{
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"Current", Current.ToJson());
    j.Set(L"IsEnabled", JsonValue::BoolValue(IsEnabled));

    JsonValue profiles = JsonValue::ArrayValue();
    for (const auto& p : Profiles) profiles.arr.push_back(p.ToJson());
    j.Set(L"Profiles", std::move(profiles));

    j.Set(L"SelectedProfileIndex", JsonValue::NumValue((double)SelectedProfileIndex));
    j.Set(L"ActiveProfileIndex", JsonValue::NumValue((double)ActiveProfileIndex));
    j.Set(L"GlobalModifiers", JsonValue::NumValue((double)GlobalModifiers));
    j.Set(L"GlobalKey", JsonValue::NumValue((double)GlobalKey));
    j.Set(L"GlobalDisplay", JsonValue::StrValue(GlobalDisplay));
    j.Set(L"AutoStart", JsonValue::BoolValue(AutoStart));
    j.Set(L"MinimizeToTray", JsonValue::BoolValue(MinimizeToTray));
    j.Set(L"UseDxgi", JsonValue::BoolValue(UseDxgi));

    JsonValue displays = JsonValue::ArrayValue();
    for (const auto& d : Displays) displays.arr.push_back(d.ToJson());
    j.Set(L"Displays", std::move(displays));

    j.Set(L"Theme", JsonValue::StrValue(Theme));
    j.Set(L"Captureable", JsonValue::BoolValue(Captureable));
    j.Set(L"PerAppEnabled", JsonValue::BoolValue(PerAppEnabled));

    JsonValue bindings = JsonValue::ArrayValue();
    for (const auto& b : AppBindings) bindings.arr.push_back(b.ToJson());
    j.Set(L"AppBindings", std::move(bindings));
    return j;
}

void ProfileData::FromJson(const JsonValue& j)
{
    const JsonValue* cur = j.Get(L"Current");
    if (cur && cur->IsObject()) Current.FromJson(*cur);
    IsEnabled = j.GetBool(L"IsEnabled", false);

    const JsonValue* profiles = j.Get(L"Profiles");
    if (profiles && profiles->IsArray())
    {
        Profiles.clear();
        for (const auto& pj : profiles->arr)
        {
            if (!pj.IsObject()) continue;
            Profile p;
            p.FromJson(pj);
            Profiles.push_back(std::move(p));
        }
    }

    SelectedProfileIndex = (int)j.GetNumber(L"SelectedProfileIndex", -1);
    ActiveProfileIndex = (int)j.GetNumber(L"ActiveProfileIndex", -1);
    GlobalModifiers = (int)j.GetNumber(L"GlobalModifiers", 0);
    GlobalKey = (int)j.GetNumber(L"GlobalKey", 0);
    GlobalDisplay = j.GetString(L"GlobalDisplay", L"");
    AutoStart = j.GetBool(L"AutoStart", false);
    MinimizeToTray = j.GetBool(L"MinimizeToTray", true);
    UseDxgi = j.GetBool(L"UseDxgi", false);
    Theme = j.GetString(L"Theme", L"default");
    Captureable = j.GetBool(L"Captureable", false);
    PerAppEnabled = j.GetBool(L"PerAppEnabled", false);

    const JsonValue* displays = j.Get(L"Displays");
    if (displays && displays->IsArray())
    {
        Displays.clear();
        for (const auto& dj : displays->arr)
        {
            if (!dj.IsObject()) continue;
            DisplayState d;
            d.FromJson(dj);
            Displays.push_back(std::move(d));
        }
    }

    const JsonValue* bindings = j.Get(L"AppBindings");
    if (bindings && bindings->IsArray())
    {
        AppBindings.clear();
        for (const auto& bj : bindings->arr)
        {
            if (!bj.IsObject()) continue;
            AppBinding b;
            b.FromJson(bj);
            AppBindings.push_back(std::move(b));
        }
    }
}

// ---------------- HotkeyText ----------------
std::wstring HotkeyText::KeyName(int vk)
{
    // 数字键
    if (vk >= 0x30 && vk <= 0x39) return NumToStr((double)(vk - 0x30));
    // 小键盘数字
    if (vk >= 0x60 && vk <= 0x69) return L"Num" + NumToStr((double)(vk - 0x60));
    // 字母
    if (vk >= 0x41 && vk <= 0x5A)
    {
        wchar_t c = (wchar_t)(L'A' + (vk - 0x41));
        return std::wstring(1, c);
    }
    // F1..F24
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + NumToStr((double)(vk - VK_F1 + 1));
    switch (vk)
    {
    case VK_SPACE:  return L"Space";
    case VK_TAB:    return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_BACK:   return L"Backspace";
    case VK_HOME:   return L"Home";
    case VK_END:    return L"End";
    case VK_PRIOR:  return L"PageUp";
    case VK_NEXT:   return L"PageDown";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_LEFT:   return L"←";
    case VK_RIGHT:  return L"→";
    case VK_UP:     return L"↑";
    case VK_DOWN:   return L"↓";
    default:
        return hsf::Format(L"VK%02X", vk);
    }
}

std::wstring HotkeyText::Format(int modifiers, int key)
{
    if (key == 0) return L"";
    std::wstring parts;
    auto add = [&](const wchar_t* s) {
        if (!parts.empty()) parts += L"+";
        parts += s;
    };
    if ((modifiers & MOD_WIN) != 0) add(L"Win");
    if ((modifiers & MOD_CONTROL) != 0) add(L"Ctrl");
    if ((modifiers & MOD_SHIFT) != 0) add(L"Shift");
    if ((modifiers & MOD_ALT) != 0) add(L"Alt");
    parts += KeyName(key);
    return parts;
}

} // namespace hsf
