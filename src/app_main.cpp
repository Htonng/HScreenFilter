// app_main.cpp — HScreenFilter 主程序（WebView2 桥接版，完整功能）
// 数据：profiles.json（与原版兼容）；引擎：FilterEngine 实时应用；
// 功能：配置 CRUD/导入导出、按应用切换（前台监听）、快捷键（捕获+注册）、自启、托盘。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio>
#include <cmath>
#include <memory>

#include "WebView2.h"
#include "common.h"
#include "models.h"
#include "store.h"
#include "monitors.h"
#include "autostart.h"
#include "msgwindow.h"
#include "hotkeys.h"
#include "fgwatcher.h"
#include "engines/filter_engine.h"

using namespace hsf;

// ---------------------------------------------------------------------------
// 动态加载 WebView2Loader.dll
// ---------------------------------------------------------------------------
typedef HRESULT(STDAPICALLTYPE *PFN_CreateEnvWithOptions)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
typedef HRESULT(STDAPICALLTYPE *PFN_GetBrowserVersion)(PCWSTR, LPWSTR *);
static PFN_CreateEnvWithOptions g_createEnv = nullptr;
static PFN_GetBrowserVersion   g_getVersion = nullptr;

static bool LoadWebView2Loader()
{
    HMODULE mod = LoadLibraryW(L"WebView2Loader.dll");
    if (!mod) return false;
    g_createEnv  = (PFN_CreateEnvWithOptions)GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions");
    g_getVersion = (PFN_GetBrowserVersion)GetProcAddress(mod, "GetAvailableCoreWebView2BrowserVersionString");
    return g_createEnv != nullptr;
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
static std::wstring g_logPath;
static void Log(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt); vswprintf(buf, 2048, fmt, ap); va_end(ap);
    FILE *f = _wfopen(g_logPath.c_str(), L"a, ccs=UTF-8");
    if (f) { fwprintf(f, L"%s\n", buf); fclose(f); }
    OutputDebugStringW(buf);
}

// ---------------------------------------------------------------------------
// 全局
// ---------------------------------------------------------------------------
static HWND g_hwnd = nullptr;
static ICoreWebView2Environment *g_env = nullptr;
static ICoreWebView2Controller *g_controller = nullptr;
static ICoreWebView2 *g_webview = nullptr;
static int g_wndW = 0, g_wndH = 0;
static std::wstring g_url;
static Store g_store;

static ProfileData data_;
static std::vector<DisplayMonitor> monitors_;
static int curDisplay_ = 0;
static FilterSettings savedSnapshot_;
static bool savedUseDxgi_ = false;
static bool savedUseVsync_ = false;

static MessageWindow msgWindow_;
static std::unique_ptr<HotkeyService> hotkeys_;
static std::map<int, int> profileHotkeyIds_;
static int globalHotkeyId_ = 0;
static std::unique_ptr<ForegroundAppWatcher> watcher_;
static int activeBinding_ = -1;

static bool capturing_ = false;
static bool captureGlobal_ = false;
static int captureProfile_ = -1;
static HHOOK g_captureHook = nullptr;

static bool g_trayAdded = false;
static bool g_exitRequested = false;

static std::vector<std::function<void()>> g_uiQueue;
static std::mutex g_uiMutex;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
static void ResizeWebView();
static void SendState(const wchar_t *type);
static void SendStatus(const wchar_t *id, const std::wstring &text);
static void ApplyCurrent();
static void SaveState();
static void UpdateWatcher();
static void ShowWindowApp(bool show);

static DisplayState &CurDisplay() { return data_.Displays[curDisplay_]; }

static void InvokeUi(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(g_uiMutex);
        g_uiQueue.push_back(std::move(fn));
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 1, 0, 0);
}

static void DrainUiQueue()
{
    for (;;)
    {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(g_uiMutex);
            if (g_uiQueue.empty()) break;
            fn = std::move(g_uiQueue.front());
            g_uiQueue.erase(g_uiQueue.begin());
        }
        if (fn) fn();
    }
}

static void SetBaseValue(FilterSettings &s, const std::wstring &key, double v)
{
    if (key == L"brightness") s.Brightness = v;
    else if (key == L"contrast") s.Contrast = v;
    else if (key == L"saturation") s.Saturation = v;
    else if (key == L"highlights") s.Highlights = v;
    else if (key == L"shadows") s.Shadows = v;
    else if (key == L"temperature") s.Temperature = v;
}

static void SetHslValue(FilterSettings &s, int channel, int field, double value)
{
    if (channel == 0)
    {
        if (field == 0) s.Hue = value;
        else if (field == 1) s.HslSaturation = value;
        else s.Lightness = value;
        return;
    }
    const wchar_t *name = HslChannelNames::ColorNames[channel - 1];
    HslChannel *ch = s.FindChannel(name);
    if (!ch) { s.HslChannels.push_back(HslChannel{ name, 0.0, 100.0, 0.0 }); ch = &s.HslChannels.back(); }
    if (field == 0) ch->Hue = value;
    else if (field == 1) ch->Saturation = value;
    else ch->Lightness = value;
}

static void ResetHsl(FilterSettings &s)
{
    s.Hue = 0; s.HslSaturation = 100; s.Lightness = 0;
    for (auto &c : s.HslChannels) { c.Hue = 0; c.Saturation = 100; c.Lightness = 0; }
}

static FilterSettings NeutralizeHsl(const FilterSettings &s)
{
    FilterSettings c = s.Clone();
    ResetHsl(c);
    return c;
}

static void InitializeDisplays()
{
    monitors_ = Monitors::Enumerate();
    if (monitors_.empty()) monitors_.push_back(DisplayMonitor{ L"", 0, 0, 1920, 1080, true });
    bool wasEmpty = data_.Displays.empty();
    while ((int)data_.Displays.size() < (int)monitors_.size())
        data_.Displays.push_back(DisplayState{});
    for (int i = 0; i < (int)data_.Displays.size() && i < (int)monitors_.size(); i++)
        data_.Displays[i].Index = i;
    if (wasEmpty && !data_.Displays.empty())
    {
        auto &primary = data_.Displays[0];
        if (!data_.Current.IsDefaultBasic()) primary.Current = data_.Current;
        primary.ActiveProfileIndex = data_.ActiveProfileIndex;
        primary.IsEnabled = data_.IsEnabled;
    }
    curDisplay_ = 0;
    for (int i = 0; i < (int)monitors_.size(); i++)
        if (monitors_[i].IsPrimary) { curDisplay_ = i; break; }
}

static void SaveState()
{
    g_store.Save(data_);
    Log(L"[store] saved profiles.json");
}

// ---------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------
static std::wstring EngineStatusText()
{
    switch (FilterEngine::Instance().Kind())
    {
    case EngineKind::PixelShader: return L"滤镜引擎：LUT 逐像素引擎";
    case EngineKind::FullScreenColorEffect: return L"滤镜引擎：全屏颜色效果";
    case EngineKind::GammaRamp: return L"滤镜引擎：显卡伽马曲线";
    default: return L"滤镜引擎不可用：" + FilterEngine::Instance().LastError();
    }
}

static void ApplyCurrent()
{
    bool perAppRequiresHit = data_.PerAppEnabled && !data_.AppBindings.empty();
    bool globalOn = data_.IsEnabled && (!perAppRequiresHit || activeBinding_ != -1);
    Profile *perAppProfile = nullptr;
    if (perAppRequiresHit && activeBinding_ >= 0 && activeBinding_ < (int)data_.AppBindings.size())
    {
        int pi = data_.AppBindings[activeBinding_].ProfileIndex;
        if (pi >= 0 && pi < (int)data_.Profiles.size())
            perAppProfile = &data_.Profiles[pi];
    }
    bool anyEnabled = false, anyOk = false;
    for (int i = 0; i < (int)monitors_.size() && i < (int)data_.Displays.size(); i++)
    {
        auto &d = data_.Displays[i];
        bool on = globalOn && d.IsEnabled;
        if (!on) { FilterEngine::Instance().ResetDisplay(i); continue; }
        anyEnabled = true;
        FilterSettings s = perAppProfile ? perAppProfile->Settings
                                         : (data_.UseDxgi ? d.Current : NeutralizeHsl(d.Current));
        if (data_.UseDxgi) FilterEngine::Instance().SetVsync(i, d.UseVsync);
        if (FilterEngine::Instance().Apply(i, monitors_[i], s)) anyOk = true;
        else Log(L"[engine] apply FAILED display %d: %s", i, FilterEngine::Instance().LastError().c_str());
    }
    if (!anyEnabled) FilterEngine::Instance().Reset();
    Log(L"[engine] apply done kind=%d useDxgi=%d anyEnabled=%d anyOk=%d",
        (int)FilterEngine::Instance().Kind(), data_.UseDxgi ? 1 : 0, anyEnabled ? 1 : 0, anyOk ? 1 : 0);
}

static void ApplyLutMode(bool useLut)
{
    data_.UseDxgi = useLut;
    FilterEngine::Instance().SetUseDxgi(useLut);
    FilterEngine::Instance().Initialize();
    if (useLut && CurDisplay().IsEnabled)
        FilterEngine::Instance().SetVsync(curDisplay_, CurDisplay().UseVsync);
}

static bool SettingsEqual(const FilterSettings &a, const FilterSettings &b)
{
    if (a.Brightness != b.Brightness || a.Contrast != b.Contrast || a.Saturation != b.Saturation ||
        a.Highlights != b.Highlights || a.Shadows != b.Shadows || a.Temperature != b.Temperature) return false;
    if (a.Hue != b.Hue || a.HslSaturation != b.HslSaturation || a.Lightness != b.Lightness) return false;
    if (a.HslChannels.size() != b.HslChannels.size()) return false;
    for (size_t i = 0; i < a.HslChannels.size(); i++)
    {
        const auto &x = a.HslChannels[i], &y = b.HslChannels[i];
        if (x.Name != y.Name || x.Hue != y.Hue || x.Saturation != y.Saturation || x.Lightness != y.Lightness) return false;
    }
    return true;
}
static std::wstring ActiveProfileName()
{
    int ai = CurDisplay().ActiveProfileIndex;
    if (ai >= 0 && ai < (int)data_.Profiles.size()) return data_.Profiles[ai].Name;
    return L"当前设置";
}
static bool IsDirty()
{
    if (!SettingsEqual(CurDisplay().Current, savedSnapshot_)) return true;
    if (data_.UseDxgi != savedUseDxgi_) return true;
    if (CurDisplay().UseVsync != savedUseVsync_) return true;
    return false;
}
static void SendDirtyState()
{
    if (!g_webview) return;
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"type", JsonValue::StrValue(L"dirty"));
    j.Set(L"dirty", JsonValue::BoolValue(IsDirty()));
    j.Set(L"target", JsonValue::StrValue(ActiveProfileName()));
    g_webview->PostWebMessageAsJson(j.SerializeCompact().c_str());
}

static void ApplyPresetByName(const std::wstring &name)
{
    FilterSettings p;
    if (name == L"eye")        { p.Brightness = -5;  p.Contrast = 95;  p.Saturation = 95;  p.Temperature = 25; }
    else if (name == L"night") { p.Brightness = -40; p.Contrast = 100; p.Saturation = 90;  p.Temperature = 45; }
    else if (name == L"vivid") { p.Contrast = 110;   p.Saturation = 150; }
    CurDisplay().Current = p.Clone();
    ApplyCurrent();
    SendDirtyState();
    SendState(L"sync");
}

// ---------------------------------------------------------------------------
// 按应用切换
// ---------------------------------------------------------------------------
static std::wstring PerAppStatusText()
{
    if (!data_.PerAppEnabled) return L"○ 按应用切换未启用";
    if (data_.AppBindings.empty()) return L"○ 尚未添加要检测的应用（请点击「添加应用…」）";
    if (activeBinding_ < 0)
        return Format(L"○ 列表内无进程在前台，滤镜已自动关闭（共 %d 个检测目标）", (int)data_.AppBindings.size());
    const AppBinding &b = data_.AppBindings[activeBinding_];
    std::wstring cfg = L"按当前设置";
    if (b.ProfileIndex >= 0 && b.ProfileIndex < (int)data_.Profiles.size())
        cfg = L"自动切换配置「" + data_.Profiles[b.ProfileIndex].Name + L"」";
    return L"● " + b.ProcessName + L" 在前台，滤镜已启用（" + cfg + L"）";
}

static void OnForegroundMatch(int hit, const std::wstring &proc, const std::wstring &title)
{
    activeBinding_ = hit;
    SendStatus(L"perapp", PerAppStatusText());
    ApplyCurrent();
    Log(L"[watcher] foreground %s -> hit=%d", proc.c_str(), hit);
}

static void UpdateWatcher()
{
    if (!watcher_) { Log(L"[watcher] UpdateWatcher: watcher null"); return; }
    watcher_->SetTargets(data_.AppBindings);
    // SetTargets 会立即执行一次 CheckNow，这里同步取最新命中值，
    // 避免随后的 ApplyCurrent 仍用旧的 activeBinding_（表现为“先关后开/不立即生效”）
    activeBinding_ = watcher_->CurrentHit();
    if (data_.PerAppEnabled)
    {
        Log(L"[watcher] UpdateWatcher: start (targets=%d)", (int)data_.AppBindings.size());
        watcher_->Start(500);
    }
    else { Log(L"[watcher] UpdateWatcher: stop"); watcher_->Stop(); activeBinding_ = -1; }
}

// ---------------------------------------------------------------------------
// 配置 CRUD
// ---------------------------------------------------------------------------
static void ProfileNew(const std::wstring &name)
{
    Profile p;
    p.Name = name.empty() ? Format(L"配置 %d", (int)data_.Profiles.size() + 1) : name;
    p.Settings = CurDisplay().Current.Clone();
    p.UseDxgi = data_.UseDxgi;
    data_.Profiles.push_back(std::move(p));
    SaveState();
    SendState(L"sync");
    SendStatus(L"hint", Format(L"已新建配置「%s」", p.Name.c_str()));
}

static void ProfileRename(int idx, const std::wstring &name)
{
    if (idx < 0 || idx >= (int)data_.Profiles.size() || name.empty()) return;
    data_.Profiles[idx].Name = name;
    SaveState();
    SendState(L"sync");
}

static void ProfileDelete(int idx)
{
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    auto it = profileHotkeyIds_.find(idx);
    if (it != profileHotkeyIds_.end()) { hotkeys_->Unregister(it->second); profileHotkeyIds_.erase(it); }
    bool wasActive = (CurDisplay().ActiveProfileIndex == idx);
    data_.Profiles.erase(data_.Profiles.begin() + idx);
    for (auto &b : data_.AppBindings)
    {
        if (b.ProfileIndex == idx) b.ProfileIndex = -1;
        else if (b.ProfileIndex > idx) b.ProfileIndex--;
    }
    if (wasActive)
    {
        for (auto &p : data_.Profiles) p.IsActive = false;
        for (auto &d : data_.Displays) d.ActiveProfileIndex = -1;
        CurDisplay().Current = FilterSettings();
        savedSnapshot_ = CurDisplay().Current.Clone();
    }
    else
    {
        for (auto &d : data_.Displays)
            if (d.ActiveProfileIndex > idx) d.ActiveProfileIndex--;
    }
    std::map<int, int> remapped;
    for (auto &kv : profileHotkeyIds_) if (kv.first > idx) remapped[kv.first - 1] = kv.second;
    profileHotkeyIds_ = std::move(remapped);
    UpdateWatcher();
    ApplyCurrent();
    SaveState();
    SendState(L"sync");
}

static void ProfileMove(int idx, int delta)
{
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    int target = idx + delta;
    if (target < 0 || target >= (int)data_.Profiles.size()) return;
    auto p = data_.Profiles[idx];
    data_.Profiles.erase(data_.Profiles.begin() + idx);
    data_.Profiles.insert(data_.Profiles.begin() + target, p);
    int activeIdx = -1;
    for (int i = 0; i < (int)data_.Profiles.size(); i++)
        if (data_.Profiles[i].IsActive) { activeIdx = i; break; }
    for (auto &d : data_.Displays) d.ActiveProfileIndex = activeIdx;
    std::map<int, int> remapped;
    for (auto &kv : profileHotkeyIds_)
    {
        int from = kv.first;
        if (from == idx) remapped[target] = kv.second;
        else if (from > idx && from <= target) remapped[from - 1] = kv.second;
        else if (from < idx && from >= target) remapped[from + 1] = kv.second;
        else remapped[from] = kv.second;
    }
    profileHotkeyIds_ = std::move(remapped);
    UpdateWatcher();
    SaveState();
    SendState(L"sync");
}

static void ProfileImport()
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"HScreenFilter 配置 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    FILE *f = _wfopen(file, L"rb");
    if (!f) { SendStatus(L"hint", L"导入失败：无法读取文件"); return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    std::string bytes;
    if (len > 0 && len < 8 * 1024 * 1024) { bytes.resize((size_t)len); fread(&bytes[0], 1, (size_t)len, f); }
    fclose(f);
    JsonValue root;
    if (!JsonValue::Parse(Utf8ToWide(bytes), root) || !root.IsObject())
    {
        SendStatus(L"hint", L"导入失败：文件内容为空或格式不正确");
        return;
    }
    FilterSettings settings;
    settings.FromJson(root);
    std::wstring name = file;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);
    Profile p; p.Name = name; p.Settings = settings; p.UseDxgi = data_.UseDxgi;
    data_.Profiles.push_back(std::move(p));
    SaveState();
    SendState(L"sync");
    SendStatus(L"hint", Format(L"已导入配置「%s」", name.c_str()));
}

static void ProfileExport(int idx)
{
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    auto &p = data_.Profiles[idx];
    std::wstring json = p.Settings.ToJson().Serialize(true);
    std::wstring file = p.Name + L".json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"HScreenFilter 配置 (*.json)\0*.json\0";
    ofn.lpstrFile = &file[0];
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    FILE *f = _wfopen(file.c_str(), L"wb");
    if (!f) { SendStatus(L"hint", L"导出失败：无法写入文件"); return; }
    std::string bytes = WideToUtf8(json);
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    SendStatus(L"hint", Format(L"已导出配置「%s」", p.Name.c_str()));
}

static void SetActiveProfileAndApply(int index)
{
    if (index < 0 || index >= (int)data_.Profiles.size()) return;
    for (int i = 0; i < (int)data_.Profiles.size(); i++)
        data_.Profiles[i].IsActive = (i == index);
    CurDisplay().ActiveProfileIndex = index;
    CurDisplay().Current = data_.Profiles[index].Settings.Clone();
    savedSnapshot_ = CurDisplay().Current.Clone();
    if (data_.Profiles[index].UseDxgi != data_.UseDxgi) ApplyLutMode(data_.Profiles[index].UseDxgi);
    savedUseDxgi_ = data_.UseDxgi;
    ApplyCurrent();
    SaveState();
    SendDirtyState();
    SendState(L"sync");
    Log(L"[bridge] activate profile %d (\"%s\")", index, data_.Profiles[index].Name.c_str());
}

static void DeactivateProfile()
{
    for (auto &p : data_.Profiles) p.IsActive = false;
    CurDisplay().ActiveProfileIndex = -1;
    CurDisplay().Current = FilterSettings();
    savedSnapshot_ = CurDisplay().Current.Clone();
    ApplyCurrent();
    SaveState();
    SendState(L"sync");
}

// ---------------------------------------------------------------------------
// 快捷键
// ---------------------------------------------------------------------------
static void ToggleGlobal()
{
    data_.IsEnabled = !data_.IsEnabled;
    ApplyCurrent();
    SaveState();
    SendState(L"sync");
}

static void ToggleProfileHotkey(int index)
{
    if (CurDisplay().ActiveProfileIndex == index) DeactivateProfile();
    else SetActiveProfileAndApply(index);
}

static void RegisterProfileHotkey(int index)
{
    if (!hotkeys_ || index < 0 || index >= (int)data_.Profiles.size()) return;
    auto &p = data_.Profiles[index];
    auto it = profileHotkeyIds_.find(index);
    if (it != profileHotkeyIds_.end()) { hotkeys_->Unregister(it->second); profileHotkeyIds_.erase(it); }
    if (!p.HasHotkey()) return;
    int id = 0;
    if (hotkeys_->Register(p.HotkeyModifiers, p.HotkeyKey,
                           [index]() { InvokeUi([index]() { ToggleProfileHotkey(index); }); }, id))
        profileHotkeyIds_[index] = id;
}

static void RegisterGlobalToggle()
{
    if (!hotkeys_) return;
    if (globalHotkeyId_ != 0) { hotkeys_->Unregister(globalHotkeyId_); globalHotkeyId_ = 0; }
    if (data_.GlobalKey == 0) return;
    int id = 0;
    if (hotkeys_->Register(data_.GlobalModifiers, data_.GlobalKey,
                           []() { InvokeUi([]() { ToggleGlobal(); }); }, id))
        globalHotkeyId_ = id;
}

static void HandleHotkeyCapture(int vk)
{
    if (vk == VK_ESCAPE)
    {
        capturing_ = false; captureGlobal_ = false; captureProfile_ = -1;
        SendStatus(L"hint", L"已取消");
        return;
    }
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
        vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN)
        return;
    int mods = 0;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) mods |= MOD_CONTROL;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) mods |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) mods |= MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) mods |= 0x8;
    std::wstring display = HotkeyText::Format(mods, vk);
    bool wasGlobal = captureGlobal_;
    int wasProfile = captureProfile_;
    capturing_ = false; captureGlobal_ = false; captureProfile_ = -1;
    if (wasGlobal)
    {
        data_.GlobalModifiers = mods; data_.GlobalKey = vk; data_.GlobalDisplay = display;
        RegisterGlobalToggle();
        SendStatus(L"hint", L"全局开关快捷键已设置为 " + display);
    }
    else if (wasProfile >= 0 && wasProfile < (int)data_.Profiles.size())
    {
        auto &p = data_.Profiles[wasProfile];
        p.HotkeyModifiers = mods; p.HotkeyKey = vk; p.HotkeyDisplay = display;
        RegisterProfileHotkey(wasProfile);
        SendStatus(L"hint", Format(L"已为「%s」设置快捷键 %s", p.Name.c_str(), display.c_str()));
    }
    SaveState();
    SendState(L"sync");
}

static LRESULT CALLBACK CaptureHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_KEYDOWN && capturing_)
    {
        auto *kb = (KBDLLHOOKSTRUCT *)lParam;
        HandleHotkeyCapture((int)kb->vkCode);
        if (g_captureHook) { UnhookWindowsHookEx(g_captureHook); g_captureHook = nullptr; }
        return 1; // 消费按键
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static void BeginCapture(bool global, int profile)
{
    if (g_captureHook) { UnhookWindowsHookEx(g_captureHook); g_captureHook = nullptr; }
    capturing_ = true;
    captureGlobal_ = global;
    captureProfile_ = global ? -1 : profile;
    g_captureHook = SetWindowsHookExW(WH_KEYBOARD_LL, CaptureHookProc, GetModuleHandleW(nullptr), 0);
    Log(L"[hotkey] capturing (global=%d profile=%d)", global ? 1 : 0, profile);
}

static void ClearHotkey(bool global, int profile)
{
    if (global)
    {
        data_.GlobalModifiers = 0; data_.GlobalKey = 0; data_.GlobalDisplay = L"";
        RegisterGlobalToggle();
        SendStatus(L"hint", L"已清除全局开关快捷键");
    }
    else if (profile >= 0 && profile < (int)data_.Profiles.size())
    {
        auto &p = data_.Profiles[profile];
        p.HotkeyModifiers = 0; p.HotkeyKey = 0; p.HotkeyDisplay = L"";
        RegisterProfileHotkey(profile);
        SendStatus(L"hint", L"已清除该配置的快捷键");
    }
    SaveState();
    SendState(L"sync");
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------
static void AddTray()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msgWindow_.Handle();
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 9;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), L"APPICON");
    swprintf(nid.szTip, 128, L"HScreenFilter %s", kVersionString);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid);
    Log(L"[tray] added=%d", g_trayAdded ? 1 : 0);
}

static void RemoveTray()
{
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msgWindow_.Handle();
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

static void ShowTrayBalloon(const std::wstring &title, const std::wstring &text)
{
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msgWindow_.Handle();
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    wcscpy(nid.szInfoTitle, title.c_str());
    wcscpy(nid.szInfo, text.c_str());
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void ShowTrayMenu()
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (data_.IsEnabled ? MF_CHECKED : 0), 100, L"启用滤镜");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const int profileBase = 200;
    int n = (int)data_.Profiles.size();
    if (n > 0)
    {
        for (int i = 0; i < n; i++)
            AppendMenuW(menu, MF_STRING | (data_.Profiles[i].IsActive ? MF_CHECKED : 0), profileBase + i, data_.Profiles[i].Name.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, 1, L"显示主窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"退出");
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) ShowWindowApp(true);
    else if (cmd == 2) { g_exitRequested = true; PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
    else if (cmd == 100)
    {
        data_.IsEnabled = !data_.IsEnabled;
        ApplyCurrent();
        SaveState();
        SendState(L"sync");
    }
    else if (cmd >= profileBase && cmd < profileBase + n)
    {
        SetActiveProfileAndApply(cmd - profileBase);
    }
}

static void ShowWindowApp(bool show)
{
    ShowWindow(g_hwnd, show ? SW_SHOW : SW_HIDE);
    if (show) { SetForegroundWindow(g_hwnd); }
}

// ---------------------------------------------------------------------------
// 状态 → 页面
// ---------------------------------------------------------------------------
static JsonValue SettingsToJson(const FilterSettings &s)
{
    JsonValue base = JsonValue::ObjectValue();
    base.Set(L"brightness", JsonValue::NumValue(s.Brightness));
    base.Set(L"contrast", JsonValue::NumValue(s.Contrast));
    base.Set(L"saturation", JsonValue::NumValue(s.Saturation));
    base.Set(L"highlights", JsonValue::NumValue(s.Highlights));
    base.Set(L"shadows", JsonValue::NumValue(s.Shadows));
    base.Set(L"temperature", JsonValue::NumValue(s.Temperature));
    JsonValue master = JsonValue::ObjectValue();
    master.Set(L"h", JsonValue::NumValue(s.Hue));
    master.Set(L"s", JsonValue::NumValue(s.HslSaturation));
    master.Set(L"l", JsonValue::NumValue(s.Lightness));
    JsonValue channels = JsonValue::ArrayValue();
    for (const auto &c : s.HslChannels)
    {
        JsonValue cj = JsonValue::ObjectValue();
        cj.Set(L"name", JsonValue::StrValue(c.Name));
        cj.Set(L"h", JsonValue::NumValue(c.Hue));
        cj.Set(L"s", JsonValue::NumValue(c.Saturation));
        cj.Set(L"l", JsonValue::NumValue(c.Lightness));
        channels.arr.push_back(std::move(cj));
    }
    JsonValue hsl = JsonValue::ObjectValue();
    hsl.Set(L"master", std::move(master));
    hsl.Set(L"channels", std::move(channels));
    JsonValue settings = JsonValue::ObjectValue();
    settings.Set(L"base", std::move(base));
    settings.Set(L"hsl", std::move(hsl));
    return settings;
}

static JsonValue BuildStatePayload(const wchar_t *type)
{
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"type", JsonValue::StrValue(type));
    JsonValue displays = JsonValue::ArrayValue();
    for (int i = 0; i < (int)monitors_.size(); i++)
    {
        JsonValue d = JsonValue::ObjectValue();
        d.Set(L"index", JsonValue::NumValue((double)i));
        d.Set(L"label", JsonValue::StrValue(monitors_[i].Label()));
        bool en = i < (int)data_.Displays.size() ? data_.Displays[i].IsEnabled : false;
        d.Set(L"enabled", JsonValue::BoolValue(en));
        displays.arr.push_back(std::move(d));
    }
    j.Set(L"displays", std::move(displays));
    j.Set(L"displayIndex", JsonValue::NumValue((double)curDisplay_));
    j.Set(L"master", JsonValue::BoolValue(data_.IsEnabled));
    j.Set(L"lut", JsonValue::BoolValue(data_.UseDxgi));
    j.Set(L"vsync", JsonValue::BoolValue(CurDisplay().UseVsync));
    j.Set(L"perapp", JsonValue::BoolValue(data_.PerAppEnabled));
    j.Set(L"capture", JsonValue::BoolValue(data_.Captureable));
    j.Set(L"autostart", JsonValue::BoolValue(data_.AutoStart));
    j.Set(L"autotray", JsonValue::BoolValue(data_.MinimizeToTray));
    j.Set(L"globalHotkey", JsonValue::StrValue(data_.GlobalDisplay));
    j.Set(L"engine", JsonValue::StrValue(EngineStatusText()));

    JsonValue profiles = JsonValue::ArrayValue();
    for (const auto &p : data_.Profiles)
    {
        JsonValue pj = JsonValue::ObjectValue();
        pj.Set(L"name", JsonValue::StrValue(p.Name));
        pj.Set(L"engine", JsonValue::StrValue(p.ApiText()));
        pj.Set(L"hotkey", JsonValue::StrValue(p.HasHotkey() ? p.HotkeyDisplay : L""));
        pj.Set(L"active", JsonValue::BoolValue(p.IsActive));
        profiles.arr.push_back(std::move(pj));
    }
    j.Set(L"profiles", std::move(profiles));
    j.Set(L"activeProfile", JsonValue::NumValue((double)CurDisplay().ActiveProfileIndex));

    JsonValue bindings = JsonValue::ArrayValue();
    for (const auto &b : data_.AppBindings)
    {
        std::wstring profileName;
        if (b.ProfileIndex >= 0 && b.ProfileIndex < (int)data_.Profiles.size())
            profileName = data_.Profiles[b.ProfileIndex].Name;
        JsonValue bj = JsonValue::ObjectValue();
        bj.Set(L"process", JsonValue::StrValue(b.ProcessName));
        bj.Set(L"title", JsonValue::StrValue(b.WindowTitle));
        bj.Set(L"profile", JsonValue::NumValue((double)b.ProfileIndex));
        bj.Set(L"name", JsonValue::StrValue(b.DisplayName()));
        bj.Set(L"target", JsonValue::StrValue(b.ProfileDisplay(profileName)));
        bindings.arr.push_back(std::move(bj));
    }
    j.Set(L"bindings", std::move(bindings));
    j.Set(L"settings", SettingsToJson(CurDisplay().Current));
    return j;
}

static void SendState(const wchar_t *type)
{
    if (!g_webview) return;
    JsonValue payload = BuildStatePayload(type);
    g_webview->PostWebMessageAsJson(payload.SerializeCompact().c_str());
}

static void SendStatus(const wchar_t *id, const std::wstring &text)
{
    if (!g_webview) return;
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"type", JsonValue::StrValue(L"status"));
    j.Set(L"id", JsonValue::StrValue(id));
    j.Set(L"text", JsonValue::StrValue(text));
    g_webview->PostWebMessageAsJson(j.SerializeCompact().c_str());
}

// ---------------------------------------------------------------------------
// 页面消息处理
// ---------------------------------------------------------------------------
static void HandleMessage(JsonValue &msg)
{
    std::wstring type = msg.GetString(L"type", L"");
    if (type == L"ready")
    {
        Log(L"[web] ready w=%d h=%d", (int)msg.GetNumber(L"w", 0), (int)msg.GetNumber(L"h", 0));
        SendState(L"init");
        SetTimer(g_hwnd, 1, 2500, nullptr);
        SetTimer(g_hwnd, 2, 5000, nullptr);
        return;
    }
    if (type == L"applied")
    {
        Log(L"[web] applied: displays=%d profiles=%d master=%d lut=%d",
            (int)msg.GetNumber(L"displays", -1), (int)msg.GetNumber(L"profiles", -1),
            msg.GetBool(L"master", false) ? 1 : 0, msg.GetBool(L"lut", false) ? 1 : 0);
        return;
    }
    if (type == L"base")
    {
        SetBaseValue(CurDisplay().Current, msg.GetString(L"key", L""), msg.GetNumber(L"value", 0));
        ApplyCurrent();
        SendDirtyState();
        return;
    }
    if (type == L"hsl")
    {
        int channel = (int)msg.GetNumber(L"channel", 0);
        std::wstring f = msg.GetString(L"field", L"h");
        int field = (f == L"s") ? 1 : (f == L"l") ? 2 : 0;
        SetHslValue(CurDisplay().Current, channel, field, msg.GetNumber(L"value", 0));
        ApplyCurrent();
        SendDirtyState();
        return;
    }
    if (type == L"preset") { ApplyPresetByName(msg.GetString(L"name", L"default")); return; }
    if (type == L"hsl-reset")
    {
        ResetHsl(CurDisplay().Current);
        ApplyCurrent();
        SendDirtyState();
        SendState(L"sync");
        return;
    }
    if (type == L"switch")
    {
        std::wstring id = msg.GetString(L"id", L"");
        bool v = msg.GetBool(L"value", false);
        if (id == L"master")
        {
            data_.IsEnabled = v;
            if (v) { bool allOff = true; for (auto &d : data_.Displays) if (d.IsEnabled) { allOff = false; break; }
                     if (allOff) for (auto &d : data_.Displays) d.IsEnabled = true; }
            ApplyCurrent();
        }
        else if (id == L"display" || id == L"displayEnable") { CurDisplay().IsEnabled = v; ApplyCurrent(); }
        else if (id == L"lut")
        {
            int ai = CurDisplay().ActiveProfileIndex;
            if (ai >= 0 && ai < (int)data_.Profiles.size()) data_.Profiles[ai].UseDxgi = v;
            ApplyLutMode(v);
            ApplyCurrent();
            SendDirtyState();
            SendState(L"sync");
        }
        else if (id == L"vsync") { CurDisplay().UseVsync = v; if (data_.UseDxgi && CurDisplay().IsEnabled) FilterEngine::Instance().SetVsync(curDisplay_, v); SendDirtyState(); }
        else if (id == L"perapp") { data_.PerAppEnabled = v; UpdateWatcher(); ApplyCurrent(); }
        else if (id == L"capture") { data_.Captureable = v; FilterEngine::Instance().SetOverlayCapturable(v); }
        else if (id == L"autostart") { data_.AutoStart = v; AutoStart::Set(v); }
        else if (id == L"autotray") { data_.MinimizeToTray = v; }
        Log(L"[bridge] switch %s = %d", id.c_str(), v ? 1 : 0);
        SendStatus(L"perapp", PerAppStatusText());
        return;
    }
    if (type == L"display")
    {
        int idx = (int)msg.GetNumber(L"index", 0);
        if (idx >= 0 && idx < (int)data_.Displays.size())
        {
            curDisplay_ = idx;
            savedSnapshot_ = CurDisplay().Current.Clone();
            ApplyCurrent();
            SendState(L"sync");
        }
        return;
    }
    if (type == L"profile-activate")
    {
        SetActiveProfileAndApply((int)msg.GetNumber(L"index", -1));
        return;
    }
    if (type == L"profile")
    {
        std::wstring action = msg.GetString(L"action", L"");
        if (action == L"new") ProfileNew(msg.GetString(L"name", L""));
        else if (action == L"rename") ProfileRename((int)msg.GetNumber(L"index", -1), msg.GetString(L"name", L""));
        else if (action == L"delete") ProfileDelete((int)msg.GetNumber(L"index", -1));
        else if (action == L"move") ProfileMove((int)msg.GetNumber(L"index", -1), (int)msg.GetNumber(L"delta", 0));
        else if (action == L"import") ProfileImport();
        else if (action == L"export") ProfileExport((int)msg.GetNumber(L"index", -1));
        return;
    }
    if (type == L"binding")
    {
        std::wstring action = msg.GetString(L"action", L"");
        int idx = (int)msg.GetNumber(L"index", -1);
        if (action == L"add")
        {
            AppBinding b;
            b.ProcessName = msg.GetString(L"process", L"");
            b.WindowTitle = msg.GetString(L"title", L"");
            b.ProfileIndex = (int)msg.GetNumber(L"profile", -1);
            if (b.ProcessName.empty()) { SendStatus(L"perapp", L"进程名不能为空"); return; }
            data_.AppBindings.push_back(b);
            UpdateWatcher(); ApplyCurrent(); SaveState();
            SendState(L"sync");
            SendStatus(L"perapp", PerAppStatusText());
        }
        else if (action == L"edit" && idx >= 0 && idx < (int)data_.AppBindings.size())
        {
            auto &b = data_.AppBindings[idx];
            b.ProcessName = msg.GetString(L"process", L"");
            b.WindowTitle = msg.GetString(L"title", L"");
            b.ProfileIndex = (int)msg.GetNumber(L"profile", -1);
            UpdateWatcher(); ApplyCurrent(); SaveState();
            SendState(L"sync");
            SendStatus(L"perapp", PerAppStatusText());
        }
        else if (action == L"delete" && idx >= 0 && idx < (int)data_.AppBindings.size())
        {
            data_.AppBindings.erase(data_.AppBindings.begin() + idx);
            if (activeBinding_ == idx) activeBinding_ = -1;
            else if (activeBinding_ > idx) activeBinding_--;
            UpdateWatcher(); ApplyCurrent(); SaveState();
            SendState(L"sync");
            SendStatus(L"perapp", PerAppStatusText());
        }
        else if (action == L"pick")
        {
            HWND fg = ForegroundAppWatcher::GetForegroundWindowForPicker();
            std::wstring proc, title;
            if (!fg || !ForegroundAppWatcher::GetForegroundInfo(fg, proc, title) || proc.empty())
            {
                SendStatus(L"perapp", L"未能读取当前前台应用");
                return;
            }
            JsonValue j = JsonValue::ObjectValue();
            j.Set(L"type", JsonValue::StrValue(L"picked"));
            j.Set(L"process", JsonValue::StrValue(proc));
            j.Set(L"title", JsonValue::StrValue(title));
            if (g_webview) g_webview->PostWebMessageAsJson(j.SerializeCompact().c_str());
        }
        return;
    }
    if (type == L"hotkey")
    {
        std::wstring action = msg.GetString(L"action", L"");
        std::wstring scope = msg.GetString(L"scope", L"profile");
        int profile = (int)msg.GetNumber(L"profile", -1);
        if (action == L"capture")
        {
            JsonValue j = JsonValue::ObjectValue();
            j.Set(L"type", JsonValue::StrValue(L"capturing"));
            j.Set(L"scope", JsonValue::StrValue(scope));
            j.Set(L"profile", JsonValue::NumValue((double)profile));
            if (g_webview) g_webview->PostWebMessageAsJson(j.SerializeCompact().c_str());
            BeginCapture(scope == L"global", profile);
        }
        else if (action == L"clear")
        {
            ClearHotkey(scope == L"global", profile);
        }
        return;
    }
    if (type == L"save")
    {
        int ai = CurDisplay().ActiveProfileIndex;
        if (ai >= 0 && ai < (int)data_.Profiles.size())
        {
            data_.Profiles[ai].Settings = CurDisplay().Current.Clone();
            data_.Profiles[ai].UseDxgi = data_.UseDxgi;
        }
        savedSnapshot_ = CurDisplay().Current.Clone();
        savedUseDxgi_ = data_.UseDxgi;
        savedUseVsync_ = CurDisplay().UseVsync;
        SaveState();
        SendDirtyState();
        SendState(L"sync");
        JsonValue j = JsonValue::ObjectValue();
        j.Set(L"type", JsonValue::StrValue(L"saved"));
        if (g_webview) g_webview->PostWebMessageAsJson(j.SerializeCompact().c_str());
        return;
    }
    if (type == L"cancel")
    {
        CurDisplay().Current = savedSnapshot_.Clone();
        if (data_.UseDxgi != savedUseDxgi_)
        {
            int ai = CurDisplay().ActiveProfileIndex;
            if (ai >= 0 && ai < (int)data_.Profiles.size()) data_.Profiles[ai].UseDxgi = savedUseDxgi_;
            ApplyLutMode(savedUseDxgi_);
        }
        ApplyCurrent();
        SendDirtyState();
        SendState(L"sync");
        return;
    }
    if (type == L"log")
    {
        ShellExecuteW(nullptr, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    Log(L"[web] unhandled: %s", type.c_str());
}


// ---- 开发测试钩子：读取 exe 目录 app-test.json 并按序喂给 HandleMessage ----
static void RunTestScript()
{
    FILE *f = _wfopen((ExeDir() + L"\\app-test.json").c_str(), L"rb");
    if (!f) { Log(L"[test] no test script"); return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    std::string bytes;
    if (len > 0 && len < 4 * 1024 * 1024) { bytes.resize((size_t)len); fread(&bytes[0], 1, (size_t)len, f); }
    fclose(f);
    JsonValue root;
    if (!JsonValue::Parse(Utf8ToWide(bytes), root) || !root.IsArray()) { Log(L"[test] bad test script"); return; }
    Log(L"[test] executing %d messages", (int)root.arr.size());
    for (auto &m : root.arr) HandleMessage(m);
    Log(L"[test] done");
}

// ---------------------------------------------------------------------------
// COM 回调
// ---------------------------------------------------------------------------
class MsgReceivedHandler : public ICoreWebView2WebMessageReceivedEventHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER; *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2WebMessageReceivedEventHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }
    STDMETHODIMP Invoke(ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) override
    {
        LPWSTR json = nullptr;
        if (args && SUCCEEDED(args->get_WebMessageAsJson(&json)) && json)
        {
            JsonValue msg;
            if (JsonValue::Parse(json, msg)) HandleMessage(msg);
            CoTaskMemFree(json);
        }
        return S_OK;
    }
};

class NavCompletedHandler : public ICoreWebView2NavigationCompletedEventHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER; *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2NavigationCompletedEventHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }
    STDMETHODIMP Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) override
    {
        BOOL ok = FALSE; if (args) args->get_IsSuccess(&ok);
        Log(L"[nav] success=%d", ok ? 1 : 0);
        if (g_hwnd) SetWindowTextW(g_hwnd, (Format(L"HScreenFilter %s", kVersionString)).c_str());
        return S_OK;
    }
};

class PreviewDoneHandler : public ICoreWebView2CapturePreviewCompletedHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER; *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CapturePreviewCompletedHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }
    STDMETHODIMP Invoke(HRESULT result) override
    {
        Log(L"[preview] CapturePreview result=0x%08X", (unsigned)result);
        return S_OK;
    }
};

static void SavePreview()
{
    if (!g_webview) return;
    std::wstring outPath = ExeDir() + L"\\HScreenFilter-preview.png";
    IStream *stream = nullptr;
    if (FAILED(SHCreateStreamOnFileW(outPath.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, &stream))) return;
    g_webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, new PreviewDoneHandler());
    stream->Release();
}

class ControllerCreatedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER; *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Controller *controller) override
    {
        if (FAILED(result) || !controller)
        {
            Log(L"[init] controller failed: 0x%08X", (unsigned)result);
            if (g_hwnd) DestroyWindow(g_hwnd);
            return S_OK;
        }
        ICoreWebView2 *webview = nullptr;
        if (FAILED(controller->get_CoreWebView2(&webview)) || !webview)
        {
            Log(L"[init] get_CoreWebView2 failed");
            if (g_hwnd) DestroyWindow(g_hwnd);
            return S_OK;
        }
        g_controller = controller; g_controller->AddRef();
        g_webview = webview;
        Log(L"[init] controller created");
        ResizeWebView();
        ICoreWebView2Settings *settings = nullptr;
        if (SUCCEEDED(webview->get_Settings(&settings)) && settings)
        {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->Release();
        }
        webview->add_NavigationCompleted(new NavCompletedHandler(), nullptr);
        webview->add_WebMessageReceived(new MsgReceivedHandler(), nullptr);
        Log(L"[init] navigate: %s", g_url.c_str());
        webview->Navigate(g_url.c_str());
        return S_OK;
    }
};

class EnvCreatedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER; *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref_; if (r == 0) delete this; return r; }
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Environment *env) override
    {
        if (FAILED(result) || !env)
        {
            Log(L"[init] environment failed: 0x%08X", (unsigned)result);
            if (g_hwnd) DestroyWindow(g_hwnd);
            return S_OK;
        }
        g_env = env; g_env->AddRef();
        Log(L"[init] environment created");
        env->CreateCoreWebView2Controller(g_hwnd, new ControllerCreatedHandler());
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------
static void InitWebView2(const std::wstring &userDataDir)
{
    if (!g_createEnv) return;
    LPWSTR ver = nullptr;
    if (g_getVersion && SUCCEEDED(g_getVersion(nullptr, &ver)) && ver)
    {
        Log(L"[init] WebView2 runtime: %s", ver);
        CoTaskMemFree(ver);
    }
    g_createEnv(nullptr, userDataDir.c_str(), nullptr, new EnvCreatedHandler());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_APP + 1: DrainUiQueue(); return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && data_.MinimizeToTray)
        {
            ShowWindow(hwnd, SW_HIDE);
            ShowTrayBalloon(kAppName, L"已最小化到系统托盘，双击图标可恢复。");
        }
        else ResizeWebView();
        return 0;
    case WM_TIMER:
        if (wp == 1) { KillTimer(hwnd, 1); SavePreview(); }
        else if (wp == 2) { KillTimer(hwnd, 2); RunTestScript(); }
        return 0;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = g_wndW; mmi->ptMinTrackSize.y = g_wndH;   // 最小 = 默认 680x860 DIP
        mmi->ptMaxTrackSize.x = 32767; mmi->ptMaxTrackSize.y = 32767;
        return 0;
    }
    case WM_CLOSE:
        if (g_exitRequested) { DestroyWindow(hwnd); }
        else { ShowWindow(hwnd, SW_HIDE); ShowTrayBalloon(kAppName, L"已最小化到系统托盘。"); }
        return 0;
    case WM_DESTROY:
        if (watcher_) watcher_->Stop();
        if (g_captureHook) { UnhookWindowsHookEx(g_captureHook); g_captureHook = nullptr; }
        if (hotkeys_) hotkeys_->UnregisterAll();
        RemoveTray();
        FilterEngine::Instance().Reset();
        if (g_webview) { g_webview->Release(); g_webview = nullptr; }
        if (g_controller) { g_controller->Release(); g_controller = nullptr; }
        if (g_env) { g_env->Release(); g_env = nullptr; }
        FilterEngine::Instance().Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ResizeWebView()
{
    if (!g_controller || !g_hwnd) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

static std::wstring ToFileUrl(const std::wstring &path)
{
    std::wstring p = path;
    for (auto &c : p) if (c == L'\\') c = L'/';
    return L"file:///" + p;
}

static void ResolveWebUi(std::wstring &outPath, std::wstring &outUrl)
{
    std::wstring cand = ExeDir() + L"\\webui2\\index.html";
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) { outPath = cand; outUrl = ToFileUrl(cand); return; }
    cand = L"F:\\code\\HScreenFilter\\webui2\\index.html";
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) { outPath = cand; outUrl = ToFileUrl(cand); return; }
    outPath = L"<not found>"; outUrl = L"about:blank";
}

static bool AutoStartLaunched()
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    if (argv)
    {
        for (int i = 1; i < argc; i++)
            if (_wcsicmp(argv[i], L"--autostart") == 0) { found = true; break; }
        LocalFree(argv);
    }
    return found;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI *PFN_SetProcessDpiAwarenessContext)(void *);
    auto setDpi = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (setDpi) setDpi((void *)-4);
    else SetProcessDPIAware();

    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_logPath = ExeDir() + L"\\HScreenFilter.log";
    Log(L"=== HScreenFilter %s start ===", kVersionString);

    // 数据 + 引擎
    data_ = g_store.Load();
    InitializeDisplays();
    savedSnapshot_ = CurDisplay().Current.Clone();
    savedUseDxgi_ = data_.UseDxgi;
    savedUseVsync_ = CurDisplay().UseVsync;

    FilterEngine::Instance().SetUseDxgi(data_.UseDxgi);
    FilterEngine::Instance().Initialize();
    FilterEngine::Instance().SetOverlayCapturable(data_.Captureable);
    Log(L"[data] profiles=%d bindings=%d displays=%d useDxgi=%d engine=%d",
        (int)data_.Profiles.size(), (int)data_.AppBindings.size(), (int)data_.Displays.size(),
        data_.UseDxgi ? 1 : 0, (int)FilterEngine::Instance().Kind());
    ApplyCurrent();

    // 后台消息窗口 + 热键
    msgWindow_.Create();
    hotkeys_ = std::make_unique<HotkeyService>(msgWindow_);
    for (int i = 0; i < (int)data_.Profiles.size(); i++) RegisterProfileHotkey(i);
    RegisterGlobalToggle();

    // 前台监听
    watcher_ = std::make_unique<ForegroundAppWatcher>();
    watcher_->MatchChanged = [](int hit, const std::wstring &proc, const std::wstring &title) {
        Log(L"[watcher] callback fired hit=%d proc=%s", hit, proc.c_str());
        InvokeUi([hit, proc, title]() { OnForegroundMatch(hit, proc, title); });
    };
    UpdateWatcher();

    // 托盘消息
    msgWindow_.AddMessageHandler([](uint32_t m, WPARAM wp, LPARAM lp) {
        if (m == WM_APP + 9)
        {
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == NIN_SELECT)
                InvokeUi([]() { ShowTrayMenu(); });
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
                InvokeUi([]() { ShowWindowApp(true); });
        }
    });

    if (!LoadWebView2Loader())
    {
        Log(L"[init] WebView2Loader.dll 未找到");
        MessageBoxW(nullptr, L"未找到 WebView2Loader.dll，请将其放在本程序同目录。", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(hInst, L"APPICON");
    wc.hIconSm = LoadIconW(hInst, L"APPICON");
    wc.lpszClassName = L"HScreenFilterMainWindow";
    RegisterClassExW(&wc);

    HDC hdc = GetDC(nullptr);
    UINT dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    if (dpi < 96) dpi = 96;

    // 设计基准 680x860 DIP，按系统缩放换算物理像素；若超过工作区约 90% 则等比缩小（避免 1080p 上窗口过大）
    int physW = MulDiv(680, (int)dpi, 96);
    int physH = MulDiv(860, (int)dpi, 96);
    RECT wa{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
    {
        int waW = wa.right - wa.left;
        int waH = wa.bottom - wa.top;
        float f = 1.0f;
        if (waW > 0 && (float)physW > waW * 0.90f) f = (waW * 0.90f) / (float)physW;
        if (waH > 0 && (float)physH > waH * 0.90f) { float fh = (waH * 0.90f) / (float)physH; if (fh < f) f = fh; }
        if (f < 1.0f)
        {
            physW = (int)(physW * f);
            physH = (int)(physH * f);
            Log(L"[init] window scaled to fit work area (factor=%.2f) -> %dx%d phys", f, physW, physH);
        }
    }

    int w = physW, h = physH;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    g_wndW = rc.right - rc.left; g_wndH = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    std::wstring wndTitle = Format(L"HScreenFilter %s", kVersionString);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, wndTitle.c_str(),
                                style, (sx - g_wndW) / 2, (sy - g_wndH) / 3, g_wndW, g_wndH,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { Log(L"[init] CreateWindow failed: %lu", GetLastError()); return 1; }
    g_hwnd = hwnd;

    std::wstring pagePath, pageUrl;
    ResolveWebUi(pagePath, pageUrl);
    g_url = pageUrl;
    Log(L"[init] webui: %s", pagePath.c_str());

    wchar_t la[512];
    std::wstring userData;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, 512))
        userData = std::wstring(la) + L"\\HScreenFilter\\Browser";

    bool startHidden = AutoStartLaunched() && data_.MinimizeToTray;
    if (!startHidden) ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    InitWebView2(userData);
    AddTray();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    Log(L"=== HScreenFilter exit ===");
    if (SUCCEEDED(coInit)) CoUninitialize();
    return (int)m.wParam;
}
