// app_main.cpp — HScreenFilter 主程序（WebView2 桥接版，完整功能）
// 数据：profiles.json（与原版兼容）；引擎：FilterEngine 实时应用；
// 功能：配置 CRUD/导入导出、按应用切换（前台监听）、快捷键（捕获+注册）、自启、托盘。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio>
#include <cmath>
#include <memory>

#include "WebView2.h"
#include "common.h"
#include "log.h"
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
static void AppLog(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt); vswprintf(buf, 2048, fmt, ap); va_end(ap);
    // 与引擎日志写进同一个文件（%LocalAppData%\HScreenFilter\debug.log）：
    // 原来宿主写 exe 目录、引擎写 LocalAppData，界面「调试日志」按钮只打开前者，
    // 于是 LutEngine 的帧率/捕获记录根本不在用户看到的文件里。
    hsf::Log::Write(L"App", buf);
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
static UINT g_taskbarCreatedMsg = 0;   // explorer 重启通知（RegisterWindowMessage）
static constexpr UINT_PTR kTimerWatchdog = 3;   // 引擎看门狗定时器
static constexpr UINT_PTR kTimerFps = 4;        // 实时帧率推送（界面右上角）
static constexpr UINT_PTR kTimerApply = 5;      // WebView2 未就绪时的兜底：延迟启动滤镜
static constexpr UINT_PTR kTimerWebView = 6;    // WebView2 启动看门狗（白屏自愈）
static constexpr UINT kMsgShowWindow = WM_APP + 2;   // 第二个实例请求已有实例显示主窗口

// 自启进托盘：窗口必须先真正显示过一次再创建 WebView2 控制器 ——
// WebView2 在「从未显示过」的窗口上初始化时不会建立可见的合成表面，之后从托盘
// 还原只会得到一张纯白窗口（已复现）。做法：自启时先把窗口显示在屏幕外，
// 等页面 ready（或导航完成）后再隐藏并归位，用户看不到闪烁。
static bool g_startHidden = false;
static bool g_startupVisibilityDone = false;
static int g_normalX = 0, g_normalY = 0;
static bool g_webViewReady = false;
static bool g_initialApplyPending = false;
static bool g_webViewReloadTried = false;
static bool g_webViewFailed = false;
static std::thread g_shaderPrewarm;

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
static void FinishStartupVisibility();
static void RevealStartupWindow();
static void ShowWebViewError(const std::wstring &detail);
static void RequestInitialApply();

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
    // 范围与字段映射统一在 models.cpp 的 FilterParams 表里；未知键直接忽略而不是静默写错字段。
    if (const auto *def = FilterParams::Find(key)) FilterParams::Set(s, *def, v);
    else AppLog(L"[bridge] 未知基础参数 key=%s（已忽略）", key.c_str());
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
    AppLog(L"[store] saved profiles.json");
}

// ---------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------
static std::wstring EngineStatusText()
{
    switch (FilterEngine::Instance().Kind())
    {
    case EngineKind::PixelShader:
        // 覆盖层因拿不到画面而暂停时明确告知（独占全屏/显示模式切换中）
        if (FilterEngine::Instance().OverlayPaused())
            return L"滤镜引擎：LUT 逐像素引擎（覆盖层暂停：独占全屏或显示模式切换中，恢复后自动继续）";
        // HDR 下即便用户关着 LUT 开关也会自动走 LUT（放大镜在 HDR 下对 HDR 内容无效）
        if (!data_.UseDxgi && curDisplay_ >= 0 && curDisplay_ < (int)monitors_.size() &&
            QueryDisplayHdr(monitors_[curDisplay_].X, monitors_[curDisplay_].Y))
            return L"滤镜引擎：LUT 逐像素引擎（HDR 下自动启用）";
        return L"滤镜引擎：LUT 逐像素引擎";
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

    // 按应用命中某个配置时，引擎模式跟随该配置的 UseDxgi；未命中/「按当前设置」
    // 时回到 data_.UseDxgi。这里只切换引擎层，不改动 data_.UseDxgi，避免自动切换
    // 污染保存的“当前设置”模式（例如：手动当前是放大镜，绑定的配置却是 LUT）。
    bool dxgiNow = perAppProfile ? perAppProfile->UseDxgi : data_.UseDxgi;
    // HDR 显示器上放大镜/伽马引擎只能作用于 SDR 合成面：HDR 内容（HDR 壁纸/视频/独占全屏游戏）
    // 完全不受影响，实测表现为「滤镜失效」。因此只要启用的显示器里有 HDR，就自动走 LUT 引擎
    //（LUT 引擎用 DXGI 桌面复制取 SDR 画面、按像素处理），不改动用户保存的开关。
    bool hdrEnabled = false;
    for (int i = 0; i < (int)monitors_.size() && i < (int)data_.Displays.size(); i++)
    {
        if (!data_.Displays[i].IsEnabled) continue;
        if (QueryDisplayHdr(monitors_[i].X, monitors_[i].Y)) { hdrEnabled = true; break; }
    }
    if (hdrEnabled && !dxgiNow)
    {
        static bool warnedHdrAutoLut = false;
        if (!warnedHdrAutoLut)
        {
            warnedHdrAutoLut = true;
            AppLog(L"[engine] HDR 显示器：自动使用 LUT 引擎（放大镜/伽马只能作用于 SDR 画面，HDR 内容会失效；不写回开关）");
        }
        dxgiNow = true;
    }
    if (FilterEngine::Instance().UseDxgi() != dxgiNow)
    {
        FilterEngine::Instance().SetUseDxgi(dxgiNow);
        FilterEngine::Instance().Initialize();
    }

    bool anyEnabled = false, anyOk = false;
    for (int i = 0; i < (int)monitors_.size() && i < (int)data_.Displays.size(); i++)
    {
        auto &d = data_.Displays[i];
        bool on = globalOn && d.IsEnabled;
        if (!on) { FilterEngine::Instance().ResetDisplay(i); continue; }
        anyEnabled = true;
        FilterSettings s = perAppProfile ? perAppProfile->Settings
                                         : (dxgiNow ? d.Current : NeutralizeHsl(d.Current));
        if (dxgiNow) FilterEngine::Instance().SetVsync(i, d.UseVsync);
        // 实时帧率 / 详细帧率日志开关（界面「显示滤镜实时帧率」）
        FilterEngine::Instance().SetFpsDetail(i, data_.ShowFps);
        if (FilterEngine::Instance().Apply(i, monitors_[i], s)) anyOk = true;
        else AppLog(L"[engine] apply FAILED display %d: %s", i, FilterEngine::Instance().LastError().c_str());
    }
    if (!anyEnabled) FilterEngine::Instance().Reset();
    // 只在结果真的变化时记一条：拖动滑块时每秒会有上千次 apply，逐次写日志既刷屏又会
    // 把日志文件撑大（旧实现实测 650 行/秒）。
    {
        static int lastKind = -1, lastDxgi = -1, lastEn = -1, lastOk = -1;
        int k = (int)FilterEngine::Instance().Kind(), dx = dxgiNow ? 1 : 0, en = anyEnabled ? 1 : 0, ok = anyOk ? 1 : 0;
        if (k != lastKind || dx != lastDxgi || en != lastEn || ok != lastOk)
        {
            lastKind = k; lastDxgi = dx; lastEn = en; lastOk = ok;
            AppLog(L"[engine] apply done kind=%d useDxgi=%d anyEnabled=%d anyOk=%d", k, dx, en, ok);
        }
    }
    // HDR + 非 LUT 引擎：实测会过曝、文字抗锯齿被裁成纯白，记一条便于排查
    bool hdrHere = false;
    if (curDisplay_ >= 0 && curDisplay_ < (int)monitors_.size())
        hdrHere = QueryDisplayHdr(monitors_[curDisplay_].X, monitors_[curDisplay_].Y);
    if (anyEnabled && !dxgiNow && hdrHere)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            AppLog(L"[engine] 警告：显示器处于 HDR，当前引擎为放大镜/伽马（会过曝、文字发虚），建议开启 LUT 引擎");
        }
    }
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
    // 统一走 models.h 的 FilterSettings::Equals（包含 DarkSceneBoost 与全部 HSL 通道）。
    return a.Equals(b);
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
    auto setCh = [&p](const wchar_t* ch, double h, double s, double l) {
        HslChannel* c = p.FindChannel(ch);
        if (c) { c->Hue = h; c->Saturation = s; c->Lightness = l; }
    };
    // ---- 基础组（只用基础滑块，放大镜/伽马引擎同样可用）----
    if (name == L"eye")        { p.Brightness = -5;  p.Contrast = 95;  p.Saturation = 95;  p.Temperature = 25; }
    else if (name == L"night") { p.Brightness = -40; p.Contrast = 100; p.Saturation = 90;  p.Temperature = 45; }
    else if (name == L"vivid") { p.Contrast = 110;   p.Saturation = 150; }
    // 游戏暗场景提亮：抬黑位、略降对比避免糊，饱和微增保证颜色不发灰
    // 游戏暗场景提亮：直接用「暗场景优化」自适应曲线（暗部大幅提亮、保色相），再配一点阴影/饱和
    else if (name == L"game-bright") { p.Brightness = 4; p.Contrast = 98; p.Saturation = 105; p.Shadows = 20; p.Highlights = -5; p.DarkSceneBoost = 70; }
    // 竞技清晰：高对比 + 压黑位，突出轮廓；由 LUT 后处理补锐化
    else if (name == L"arena") { p.Contrast = 125; p.Saturation = 110; p.Shadows = -10; p.Highlights = 5; p.Sharpen = 10; p.Clarity = 15; }
    // 冷色办公：偏冷、低饱和、轻微提亮，长时间看文档不刺眼
    else if (name == L"office") { p.Brightness = 3; p.Contrast = 98; p.Saturation = 92; p.Temperature = -18; }
    // ---- LUT 组（分色系 HSL / 后处理，需 LUT 引擎；HDR 下会自动启用）----
    // HDR 色彩：略高对比 + 略高饱和 + 微提亮高光/压暗黑位，并加一点清晰度/锐化做出「HDR 观感」。
    // 注意：SDR 内容按屏幕实际白点输出后，画面已在 HDR 亮度上，预设不能再叠加强对比/高饱和，
    // 否则高光很快被推到 1.0 裁切（过曝）且颜色失真 —— 这里刻意取温和值。
    //（清晰度/锐化属 LUT 后处理，仅 LUT 引擎下生效，其它引擎下参数保留但不应用）
    else if (name == L"hdr")   { p.Contrast = 108;   p.Saturation = 115; p.Highlights = 8; p.Shadows = -5; p.Sharpen = 8; p.Clarity = 18; }
    // 电影青橙：暗部偏青、亮部偏橙，绿降饱和（经典电影配色的核心）
    else if (name == L"cinematic")
    {
        p.Contrast = 108; p.Saturation = 105; p.Highlights = 5; p.Shadows = -6; p.Temperature = 6; p.Clarity = 15;
        setCh(HslChannelNames::Cyan, -8, 135, -4);
        setCh(HslChannelNames::Blue, -6, 120, -6);
        setCh(HslChannelNames::Orange, 4, 145, 8);
        setCh(HslChannelNames::Yellow, 0, 115, 4);
        setCh(HslChannelNames::Red, 0, 120, 2);
        setCh(HslChannelNames::Green, 0, 80, 0);
    }
    // 风景浓郁：绿/青/蓝加饱和、抬暗部保留细节
    else if (name == L"landscape")
    {
        p.Contrast = 106; p.Saturation = 110; p.Highlights = -8; p.Shadows = 12; p.Clarity = 25; p.Sharpen = 8;
        setCh(HslChannelNames::Green, 0, 130, 3);
        setCh(HslChannelNames::Cyan, 0, 125, 0);
        setCh(HslChannelNames::Blue, -4, 125, 0);
        setCh(HslChannelNames::Yellow, 0, 120, 2);
    }
    // 人像柔肤：肤色（红/橙）微提亮、降噪柔化，避免过度锐化
    else if (name == L"portrait")
    {
        p.Brightness = 3; p.Contrast = 102; p.Highlights = 5; p.Shadows = 8; p.NoiseReduction = 25; p.Clarity = 8;
        setCh(HslChannelNames::Orange, 0, 105, 6);
        setCh(HslChannelNames::Red, 0, 105, 4);
    }
    // 赛博夜景：青蓝紫强化 + 高对比，霓虹感
    else if (name == L"cyber")
    {
        p.Contrast = 115; p.Saturation = 120; p.Shadows = -12; p.Highlights = 6; p.Clarity = 28; p.Sharpen = 10;
        setCh(HslChannelNames::Cyan, -10, 140, 0);
        setCh(HslChannelNames::Blue, -8, 145, -6);
        setCh(HslChannelNames::Purple, 6, 130, 0);
        setCh(HslChannelNames::Magenta, 0, 125, 0);
    }
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
    AppLog(L"[watcher] foreground %s -> hit=%d", proc.c_str(), hit);
}

static void UpdateWatcher()
{
    if (!watcher_) { AppLog(L"[watcher] UpdateWatcher: watcher null"); return; }
    watcher_->SetTargets(data_.AppBindings);
    // SetTargets 会立即执行一次 CheckNow，这里同步取最新命中值，
    // 避免随后的 ApplyCurrent 仍用旧的 activeBinding_（表现为“先关后开/不立即生效”）
    activeBinding_ = watcher_->CurrentHit();
    if (data_.PerAppEnabled)
    {
        AppLog(L"[watcher] UpdateWatcher: start (targets=%d)", (int)data_.AppBindings.size());
        watcher_->Start(500);
    }
    else { AppLog(L"[watcher] UpdateWatcher: stop"); watcher_->Stop(); activeBinding_ = -1; }
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
    AppLog(L"[bridge] activate profile %d (\"%s\")", index, data_.Profiles[index].Name.c_str());
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

// 标题栏与 UI 同色、并把系统标题文字设成同色（等于隐藏）→ 观感上标题栏与界面一体。
// 颜色取网页主题的 --bg / --border：深色 #171B22/#313B4B、浅色 #F3F5F9/#E1E7EF。
// 网页每次切换/解析主题都会发 {type:'theme', dark} 过来校正（'auto' 由页面解析后上报）。
static void ApplyTitleBarTheme(bool dark)
{
    if (!g_hwnd) return;
    BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof(d));
    COLORREF caption = dark ? RGB(0x17, 0x1B, 0x22) : RGB(0xF3, 0xF5, 0xF9);
    COLORREF border = dark ? RGB(0x31, 0x3B, 0x4B) : RGB(0xE1, 0xE7, 0xEF);
    DwmSetWindowAttribute(g_hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(g_hwnd, 36 /*DWMWA_TEXT_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(g_hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
}

static void HandleHotkeyCapture(int vk, int scanCode, bool extended)
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
    // 显示名带扫描码：小键盘 -/. / Enter 与主键区 M/./Enter 共用同一 VK，
    // 不区分就会出现「按小键盘 - 显示 VK6D」这种情况。
    std::wstring display = HotkeyText::FormatEx(mods, vk, scanCode, extended);
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
        HandleHotkeyCapture((int)kb->vkCode, (int)kb->scanCode,
                            (kb->flags & LLKHF_EXTENDED) != 0);
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
    AppLog(L"[hotkey] capturing (global=%d profile=%d)", global ? 1 : 0, profile);
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
    AppLog(L"[tray] added=%d", g_trayAdded ? 1 : 0);
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
    if (!show)
    {
        ShowWindow(g_hwnd, SW_HIDE);
        return;
    }

    // 可靠还原：旧实现只用 SW_SHOW —— 对最小化的窗口不会还原，且后台进程调用
    // SetForegroundWindow 常被前台锁拒绝，窗口会弹在其他窗口后面，看起来"双击没反应"。
    bool wasIconic = IsIconic(g_hwnd) != FALSE;
    bool wasVisible = IsWindowVisible(g_hwnd) != FALSE;
    if (wasIconic) ShowWindow(g_hwnd, SW_RESTORE);
    else if (!wasVisible) ShowWindow(g_hwnd, SW_SHOWNA);
    else ShowWindow(g_hwnd, SW_SHOW);
    SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (!SetForegroundWindow(g_hwnd))
    {
        // 前台锁拒绝：用 TOPMOST 抖动强行置顶，并闪任务栏提示，保证用户能看到窗口
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = g_hwnd;
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        FlashWindowEx(&fi);
    }
    AppLog(L"[tray] 显示主窗口: 之前最小化=%d 之前隐藏=%d 前台=%d",
        wasIconic ? 1 : 0, wasVisible ? 0 : 1, GetForegroundWindow() == g_hwnd ? 1 : 0);

    // WebView2 控制器在窗口隐藏期间可能停止绘制；还原后显式恢复可见性并重新下发
    // 一次 bounds，强制它按当前客户区重绘（否则自启/最小化到托盘后可能只看到白底）。
    if (g_controller)
    {
        g_controller->put_IsVisible(TRUE);
        ResizeWebView();
    }
}

// 自启窗口已经在屏幕外显示过（为了让 WebView2 建立合成表面），现在把它藏起来并归位。
// 只在页面 ready / 导航成功后调用一次；失败路径由 RevealStartupWindow 保证用户能看到错误。
static void FinishStartupVisibility()
{
    if (!g_hwnd || g_startupVisibilityDone) return;
    g_startupVisibilityDone = true;
    if (!g_startHidden)
    {
        // 手动启动本来就是显示状态；只需要确保没有遗留在屏幕外的坐标
        return;
    }
    ShowWindowAsync(g_hwnd, SW_HIDE);
    SetWindowPos(g_hwnd, HWND_TOP, g_normalX, g_normalY, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    AppLog(L"[init] 页面已就绪：自启窗口已隐藏并归位到 (%d,%d)，可从托盘恢复", g_normalX, g_normalY);
}

// 自启时如果 WebView2 初始化失败，必须把窗口挪回屏幕内并显示，否则用户只会看到
// 一个「什么都没有」的后台进程。错误对话框也挂在主窗口上，避免被前台窗口盖住。
static void RevealStartupWindow()
{
    if (!g_hwnd) return;
    g_startupVisibilityDone = true;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetWindowPos(g_hwnd, HWND_TOP, g_normalX, g_normalY, 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ResizeWebView();
    AppLog(L"[init] 启动失败：已把主窗口移回屏幕内显示");
}

static void ShowWebViewError(const std::wstring& detail)
{
    if (g_webViewFailed) return;
    g_webViewFailed = true;
    g_webViewReady = false;
    AppLog(L"[init] WebView2 启动失败：%s", detail.c_str());
    RevealStartupWindow();
    std::wstring msg = L"界面组件 WebView2 未能启动。\n\n" + detail +
        L"\n\n可尝试：\n"
        L"1. 安装或修复 Microsoft Edge WebView2 Runtime；\n"
        L"2. 确认程序目录下 WebView2Loader.dll 与 webui2\\ 完整；\n"
        L"3. 查看日志：" + hsf::Log::FilePath();
    MessageBoxW(g_hwnd, msg.c_str(), L"HScreenFilter 启动失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
    g_exitRequested = true;
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// 错峰启动：先把主窗口/WebView2 初始化完，再启动 LUT 覆盖层与渲染线程。
// 旧顺序是在 WebView2 冷启动的同一时刻就创建全屏覆盖层 + 180fps 渲染线程，
// 首启时两者抢 GPU/CPU，表现为「刚开滤镜大幅卡顿，重启几次（缓存热了）才稳」。
static void RequestInitialApply()
{
    if (!g_initialApplyPending) return;
    g_initialApplyPending = false;
    if (g_hwnd) KillTimer(g_hwnd, kTimerApply);
    AppLog(L"[init] 主窗口/页面已就绪，开始应用滤镜（错峰启动）");
    ApplyCurrent();
    SendState(L"sync");
}

// ---------------------------------------------------------------------------
// 状态 → 页面
// ---------------------------------------------------------------------------
static JsonValue SettingsToJson(const FilterSettings &s)
{
    // 基础参数只从元数据表生成，新增参数不需要再改这里。
    JsonValue base = JsonValue::ObjectValue();
    for (const auto &def : FilterParams::All())
        base.Set(def.Key, JsonValue::NumValue(FilterParams::Get(s, def)));
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
    // HDR 显示器上强制性走 LUT（放大镜/伽马作用在合成面上，对 HDR 桌面等于滤镜失效）：
    // 开关显示为「开」（effective）并置 lutForced，界面据此锁住开关。
    // 注意：捕获方式固定是 DXGI 桌面复制（SDR 画面），HDR 只影响「该不该用 LUT 引擎」。
    bool hdrNowPayload = false;
    if (curDisplay_ >= 0 && curDisplay_ < (int)monitors_.size())
        hdrNowPayload = QueryDisplayHdr(monitors_[curDisplay_].X, monitors_[curDisplay_].Y);
    j.Set(L"lut", JsonValue::BoolValue(data_.UseDxgi || hdrNowPayload));
    j.Set(L"lutForced", JsonValue::BoolValue(hdrNowPayload));
    j.Set(L"vsync", JsonValue::BoolValue(CurDisplay().UseVsync));
    // 滤镜层强制垂直同步（窗口模式 Present(1)，由 DWM 对齐 vblank）：界面开关显示为「开」并锁住。
    j.Set(L"vsyncForced", JsonValue::BoolValue(true));
    {
        bool hdrNow = false;
        if (curDisplay_ >= 0 && curDisplay_ < (int)monitors_.size())
            hdrNow = QueryDisplayHdr(monitors_[curDisplay_].X, monitors_[curDisplay_].Y);
        j.Set(L"hdr", JsonValue::BoolValue(hdrNow));
    }
    // LUT 引擎是否真的在跑（高级调整以此决定可否调节，而不是只看开关）
    j.Set(L"lutActive", JsonValue::BoolValue(FilterEngine::Instance().Kind() == EngineKind::PixelShader));
    j.Set(L"perapp", JsonValue::BoolValue(data_.PerAppEnabled));
    j.Set(L"capture", JsonValue::BoolValue(data_.Captureable));
    j.Set(L"showFps", JsonValue::BoolValue(data_.ShowFps));
    // 界面主题（auto/light/dark）：页面据此恢复上次选择；临时文件/旧配置里的
    // "default" 一律按 auto 处理（models 的默认值仍是 auto）。
    std::wstring theme = data_.Theme;
    if (theme != L"light" && theme != L"dark") theme = L"auto";
    j.Set(L"theme", JsonValue::StrValue(theme));
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

    // 参数元数据：网页据此生成滑块、决定范围/默认值/分组，避免两端范围表漂移。
    JsonValue paramDefs = JsonValue::ArrayValue();
    for (const auto &def : FilterParams::All())
    {
        JsonValue d = JsonValue::ObjectValue();
        d.Set(L"key", JsonValue::StrValue(def.Key));
        d.Set(L"label", JsonValue::StrValue(def.Label));
        d.Set(L"jsonKey", JsonValue::StrValue(def.JsonKey));
        d.Set(L"min", JsonValue::NumValue(def.Min));
        d.Set(L"max", JsonValue::NumValue(def.Max));
        d.Set(L"default", JsonValue::NumValue(def.Default));
        d.Set(L"advanced", JsonValue::BoolValue(def.Advanced));
        paramDefs.arr.push_back(std::move(d));
    }
    j.Set(L"params", std::move(paramDefs));
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

// 实时帧率推送：只在用户开启「显示滤镜实时帧率」时发送，每 500ms 一次。帧率由 LUT 引擎
// 在渲染线程里统计（1 秒窗口），这里只做转发 —— 界面右上角的数字 + 覆盖层右上角的 OSD。
// fps < 0 表示当前没有在跑的 LUT 引擎（显示「—」）。
static void PushPresentFps()
{
    if (!g_webview || !data_.ShowFps) return;
    double fps = FilterEngine::Instance().PresentFps(curDisplay_);
    JsonValue j = JsonValue::ObjectValue();
    j.Set(L"type", JsonValue::StrValue(L"fps"));
    j.Set(L"fps", JsonValue::NumValue(fps));
    g_webview->PostWebMessageAsJson(j.SerializeCompact().c_str());
    // 覆盖层右上角的帧率 OSD（每台在跑的显示器各显示自己的帧率）
    FilterEngine::Instance().UpdateFpsOsd();
}

// ---------------------------------------------------------------------------
// 页面消息处理
// ---------------------------------------------------------------------------
static void HandleMessage(JsonValue &msg)
{
    std::wstring type = msg.GetString(L"type", L"");
    if (type == L"ready")
    {
        AppLog(L"[web] ready w=%d h=%d", (int)msg.GetNumber(L"w", 0), (int)msg.GetNumber(L"h", 0));
        g_webViewReady = true;
        if (g_hwnd) KillTimer(g_hwnd, kTimerWebView);
        SendState(L"init");
        // 顺序很重要：先让自启窗口隐藏/归位，再启动全屏覆盖层，避免自启时两者同时改
        // 合成状态（也是首启卡顿的一个来源）。
        FinishStartupVisibility();
        RequestInitialApply();
#ifdef HSF_DEBUG
        SetTimer(g_hwnd, 1, 2500, nullptr);
        SetTimer(g_hwnd, 2, 5000, nullptr);
#endif
        return;
    }
    if (type == L"applied")
    {
        AppLog(L"[web] applied: displays=%d profiles=%d master=%d lut=%d",
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
    // 网页上报实际生效主题：标题栏配色跟着 UI 走（'auto' 时页面已按系统解析）
    if (type == L"theme")
    {
        ApplyTitleBarTheme(msg.GetBool(L"dark", false));
        return;
    }
    // 界面主题是用户偏好：立即落盘，重启/自启后保持（与「显示实时帧率」一致）
    if (type == L"theme-save")
    {
        std::wstring theme = msg.GetString(L"theme", L"auto");
        if (theme != L"auto" && theme != L"light" && theme != L"dark") theme = L"auto";
        data_.Theme = theme;
        SaveState();
        AppLog(L"[bridge] 界面主题 = %s", theme.c_str());
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
            // 总开关是全局状态，不属于「当前配置」的草稿；立即落盘，重启后保持。
            SaveState();
        }
        else if (id == L"display" || id == L"displayEnable") { CurDisplay().IsEnabled = v; ApplyCurrent(); SaveState(); }
          else if (id == L"lut")
          {
              // HDR 下 LUT 是强制的（放大镜/伽马对 HDR 内容无效），忽略关闭请求
              bool hdrHere = curDisplay_ >= 0 && curDisplay_ < (int)monitors_.size() &&
                             QueryDisplayHdr(monitors_[curDisplay_].X, monitors_[curDisplay_].Y);
              if (hdrHere && !v)
              {
                  SendState(L"sync");
                  return;
              }
              int ai = CurDisplay().ActiveProfileIndex;
            if (ai >= 0 && ai < (int)data_.Profiles.size()) data_.Profiles[ai].UseDxgi = v;
            ApplyLutMode(v);
            ApplyCurrent();
            SendDirtyState();
            SendState(L"sync");
        }
        else if (id == L"vsync") { CurDisplay().UseVsync = v; if (data_.UseDxgi && CurDisplay().IsEnabled) FilterEngine::Instance().SetVsync(curDisplay_, v); SendDirtyState(); }
        else if (id == L"perapp") { data_.PerAppEnabled = v; UpdateWatcher(); ApplyCurrent(); SaveState(); }
        else if (id == L"capture")
        {
            // 固定关闭：覆盖层可捕获 = 被自己的桌面复制抓到 = 全黑反馈。
            if (v) SendStatus(L"hint", L"LUT 覆盖层必须排除自身捕获，「允许捕获」已禁用");
            data_.Captureable = false;
            FilterEngine::Instance().SetOverlayCapturable(false);
            SaveState();
        }
        else if (id == L"showfps")
        {
            // 显示滤镜实时帧率：右上角显示 + 日志每秒一条帧率明细（抓游戏内卡顿用）
            data_.ShowFps = v;
            for (int i = 0; i < (int)data_.Displays.size(); i++)
                FilterEngine::Instance().SetFpsDetail(i, v);
            if (g_hwnd)
            {
                if (v) { SetTimer(g_hwnd, kTimerFps, 500, nullptr); PushPresentFps(); }
                else KillTimer(g_hwnd, kTimerFps);
            }
            // 显示偏好立即落盘：用户不用为了这个开关去点「保存」，重启后也还在
            SaveState();
            AppLog(L"[bridge] 实时帧率显示 = %d（日志详细帧率%s）", v ? 1 : 0, v ? L"已开启" : L"已关闭");
        }
        else if (id == L"autostart")
        {
            // 注册表写入可能失败（组策略/权限）：以注册表实际状态为准回写 UI，
            // 并把结果落盘，避免「界面显示已开、实际没写进去」或重启后状态回退。
            AutoStart::Set(v);
            data_.AutoStart = AutoStart::IsEnabled();
            SaveState();
            if (data_.AutoStart != v)
            {
                SendStatus(L"hint", v ? L"开机自启设置失败：注册表写入被拒绝" : L"开机自启关闭失败：注册表项无法删除");
                SendState(L"sync");   // 把开关拨回注册表的真实状态，避免界面显示与实际不一致
            }
        }
        else if (id == L"autotray") { data_.MinimizeToTray = v; SaveState(); }
        AppLog(L"[bridge] switch %s = %d", id.c_str(), v ? 1 : 0);
        SendStatus(L"perapp", PerAppStatusText());
        return;
    }
    if (type == L"display")
    {
        int idx = (int)msg.GetNumber(L"index", 0);
        if (idx >= 0 && idx < (int)data_.Displays.size())
        {
            curDisplay_ = idx;
            // 切换显示器后，脏状态比较基准必须同步到这台显示器的当前值，
            // 否则「改了 A 屏再切到 B 屏」会误报脏、取消时还会把 A 屏的快照写回 B 屏。
            savedSnapshot_ = CurDisplay().Current.Clone();
            savedUseDxgi_ = data_.UseDxgi;
            savedUseVsync_ = CurDisplay().UseVsync;
            ApplyCurrent();
            SendDirtyState();
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
        // 垂直同步同样回滚（否则取消后开关不还原、IsDirty 仍为 true）
        CurDisplay().UseVsync = savedUseVsync_;
        ApplyCurrent();
        SendDirtyState();
        SendState(L"sync");
        return;
    }
    if (type == L"log")
    {
        // 打开完整日志（宿主 + 滤镜引擎同一个文件）
        ShellExecuteW(nullptr, L"open", hsf::Log::FilePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    AppLog(L"[web] unhandled: %s", type.c_str());
}


// ---- 开发测试钩子：读取 exe 目录 app-test.json 并按序喂给 HandleMessage ----
#ifdef HSF_DEBUG
static void RunTestScript()
{
    FILE *f = _wfopen((ExeDir() + L"\\app-test.json").c_str(), L"rb");
    if (!f) { AppLog(L"[test] no test script"); return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    std::string bytes;
    if (len > 0 && len < 4 * 1024 * 1024) { bytes.resize((size_t)len); fread(&bytes[0], 1, (size_t)len, f); }
    fclose(f);
    JsonValue root;
    if (!JsonValue::Parse(Utf8ToWide(bytes), root) || !root.IsArray()) { AppLog(L"[test] bad test script"); return; }
    AppLog(L"[test] executing %d messages", (int)root.arr.size());
    for (auto &m : root.arr) HandleMessage(m);
    AppLog(L"[test] done");
}
#endif // HSF_DEBUG

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
        AppLog(L"[nav] success=%d", ok ? 1 : 0);
        if (g_hwnd) SetWindowTextW(g_hwnd, (Format(L"HScreenFilter %s", kVersionString)).c_str());
        if (ok)
        {
            // JS 的 ready 是首选信号；导航成功作为兜底，保证自启窗口不会一直停在屏幕外。
            FinishStartupVisibility();
        }
        else
        {
            // 页面加载失败一次时先重载（磁盘/Defender 偶发占用、WebView2 冷启动竞争），
            // 第二次仍失败才报明确的错误 —— 不再留一张纯白窗口让用户猜。
            if (!g_webViewReloadTried && sender)
            {
                g_webViewReloadTried = true;
                AppLog(L"[nav] 首次导航失败，立即重载一次");
                sender->Reload();
            }
            else
            {
                ShowWebViewError(L"页面导航失败：" + g_url +
                                 L"\n请确认 webui2\\index.html 存在且可读。");
            }
        }
        return S_OK;
    }
};

#ifdef HSF_DEBUG
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
        AppLog(L"[preview] CapturePreview result=0x%08X", (unsigned)result);
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
#endif // HSF_DEBUG

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
            ShowWebViewError(Format(L"创建 WebView2 控制器失败 (0x%08X)。\n"
                                    L"常见原因：WebView2 Runtime 未安装/版本过旧，或用户数据目录被其它进程占用。",
                                    (unsigned)result));
            return S_OK;
        }
        ICoreWebView2 *webview = nullptr;
        if (FAILED(controller->get_CoreWebView2(&webview)) || !webview)
        {
            ShowWebViewError(L"无法从控制器取得 CoreWebView2 接口。");
            return S_OK;
        }
        g_controller = controller; g_controller->AddRef();
        g_webview = webview;
        // 关键：显式恢复可见性。自启时窗口曾在屏幕外显示过，若控制器沿用了
        // 「不可见」状态，从托盘恢复后就会是一张白底窗口（WebView2 不绘制）。
        g_controller->put_IsVisible(TRUE);
        // 页面加载完成前先铺与主题一致的颜色，避免白屏闪光；不支持该接口的老运行时忽略。
        {
            ComPtr<ICoreWebView2Controller2> c2;
            if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2,
                                                     (void**)c2.GetAddressOf())) && c2)
            {
                bool dark = (data_.Theme == L"dark");
                if (data_.Theme != L"light" && data_.Theme != L"dark")
                {
                    DWORD appsUseLight = 1, size = sizeof(appsUseLight);
                    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLight, &size);
                    dark = (appsUseLight == 0);
                }
                COREWEBVIEW2_COLOR bg{};
                bg.A = 255;
                if (dark) { bg.R = 0x17; bg.G = 0x1B; bg.B = 0x22; }
                else      { bg.R = 0xF3; bg.G = 0xF5; bg.B = 0xF9; }
                c2->put_DefaultBackgroundColor(bg);
            }
        }
        AppLog(L"[init] controller created（窗口可见=%d）", IsWindowVisible(g_hwnd) ? 1 : 0);
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
        g_webViewReady = false;
        g_webViewReloadTried = false;
        // 启动看门狗：12 秒还没收到页面 ready 就重载一次，再不行就给出错误提示。
        if (g_hwnd) SetTimer(g_hwnd, kTimerWebView, 12000, nullptr);
        AppLog(L"[init] navigate: %s", g_url.c_str());
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
            ShowWebViewError(Format(L"创建 WebView2 环境失败 (0x%08X)。\n"
                                    L"请确认系统已安装 Microsoft Edge WebView2 Runtime，且用户数据目录可写。",
                                    (unsigned)result));
            return S_OK;
        }
        g_env = env; g_env->AddRef();
        AppLog(L"[init] environment created");
        env->CreateCoreWebView2Controller(g_hwnd, new ControllerCreatedHandler());
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------
// 后台预热 LUT 着色器：d3dcompiler 的编译是纯 CPU 工作，第一次启用滤镜时
// LutEngine::CreatePipeline 要付 100ms+，正好落在用户点开关的瞬间（UI 线程阻塞）。
// 进程启动后就把 4 个着色器编译进进程级缓存，等页面 ready 再启滤镜时通常已编译完，
// 用户感知到的启动停顿明显缩短。低优先级，不抢 WebView2 冷启动的 CPU。
static void PrewarmLutShaders()
{
    if (FilterEngine::Instance().Kind() != EngineKind::PixelShader) return;
    g_shaderPrewarm = std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        DWORD t0 = GetTickCount();
        std::wstring err;
        ComPtr<ID3DBlob> blob;
        bool ok = CompileShader(g_vsSource, "main", "vs_4_0", blob, err);
        ok = ok && CompileShader(g_psLutSource, "main", "ps_4_0", blob, err);
        ok = ok && CompileShader(g_psPassthroughSource, "main", "ps_4_0", blob, err);
        ok = ok && CompileShader(g_csLutSource, "CSMain", "cs_5_0", blob, err);
        AppLog(L"[init] LUT 着色器预热%s，耗时 %ums%s%s",
               ok ? L"完成" : L"失败", (unsigned)(GetTickCount() - t0),
               ok ? L"" : L"：", ok ? L"" : err.c_str());
    });
}

static void InitWebView2(const std::wstring &userDataDir)
{
    if (!g_createEnv) return;
    LPWSTR ver = nullptr;
    if (g_getVersion && SUCCEEDED(g_getVersion(nullptr, &ver)) && ver)
    {
        AppLog(L"[init] WebView2 runtime: %s", ver);
        CoTaskMemFree(ver);
    }
    g_createEnv(nullptr, userDataDir.c_str(), nullptr, new EnvCreatedHandler());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_APP + 1: DrainUiQueue(); return 0;
    case kMsgShowWindow:
        // 第二个实例启动时请求显示已有实例：必须走 ShowWindowApp 的完整还原路径
        // （SW_RESTORE/SW_SHOWNA + put_IsVisible + ResizeWebView），否则自启托盘实例
        // 被直接 ShowWindow 后 WebView2 可能仍是白底。
        ShowWindowApp(true);
        return 0;
    case WM_DISPLAYCHANGE:
        // 分辨率 / DSR / HDR 切换后重新枚举显示器并重新应用：覆盖层几何与交换链
        // 跟着新分辨率走，避免滤镜作用在错误的区域或旧尺寸上。
        AppLog(L"[display] WM_DISPLAYCHANGE %dx%d", (int)LOWORD(lp), (int)HIWORD(lp));
        InitializeDisplays();
        // 重新枚举后 curDisplay_ 可能回到主显示器；保存快照也要跟着切，
        // 否则新显示器的当前值会和旧快照比较，误报「有未保存修改」。
        savedSnapshot_ = CurDisplay().Current.Clone();
        savedUseDxgi_ = data_.UseDxgi;
        savedUseVsync_ = CurDisplay().UseVsync;
        ApplyCurrent();
        SendState(L"sync");
        return 0;
    case WM_NCHITTEST:
    {
        // 标题栏已延伸进客户区（DwmExtendFrameIntoClientArea）：顶部一条算标题栏，
        // 可拖动/双击最大化；右侧留出系统按钮宽度，其余仍按客户区处理，网页才能点。
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit != HTCLIENT) return hit;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int captionH = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
        int buttonsW = 150;   // Win10/11 三个标题栏按钮的宽度（含留白）
        if (pt.y >= 0 && pt.y < captionH && pt.x < rc.right - buttonsW)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && data_.MinimizeToTray)
        {
            ShowWindow(hwnd, SW_HIDE);
            ShowTrayBalloon(kAppName, L"已最小化到系统托盘，双击图标可恢复。");
        }
        else ResizeWebView();
        return 0;
    case WM_TIMER:
        if (wp == kTimerFps) { PushPresentFps(); return 0; }
        if (wp == kTimerApply)
        {
            // 页面 6 秒都没 ready：不无限等，先把滤镜按保存的状态应用上；
            // WebView2 后续恢复时 ready 仍会走正常路径（RequestInitialApply 幂等）。
            if (g_initialApplyPending)
            {
                AppLog(L"[init] 页面 ready 超时，兜底启动滤镜");
                RequestInitialApply();
            }
            else KillTimer(hwnd, kTimerApply);
            return 0;
        }
        if (wp == kTimerWebView)
        {
            KillTimer(hwnd, kTimerWebView);
            if (g_webViewReady || g_webViewFailed) return 0;
            if (!g_webview)
            {
                // 环境/控制器回调一直没来：不能无限等，给用户明确提示并退出。
                ShowWebViewError(L"WebView2 初始化超时（12 秒内没有创建出控制器）。\n"
                                 L"常见原因：WebView2 Runtime 安装损坏、用户数据目录被其它进程占用。");
                return 0;
            }
            if (!g_webViewReloadTried)
            {
                g_webViewReloadTried = true;
                AppLog(L"[web] 12 秒仍未收到 ready，重载页面一次");
                g_webview->Reload();
                SetTimer(hwnd, kTimerWebView, 12000, nullptr);
            }
            else
            {
                ShowWebViewError(L"页面在 24 秒内没有完成加载（未收到 ready）。\n"
                                 L"请检查 webui2\\index.html 与 app.js 是否完整。");
            }
            return 0;
        }
        if (wp == kTimerWatchdog)
        {
            // 引擎看门狗：LUT 引擎静默死亡 / 伽马表被系统重置时自动恢复
            if (FilterEngine::Instance().Watchdog())
            {
                AppLog(L"[watchdog] 检测到引擎失效，已自动重建");
                SendState(L"sync");
            }
            return 0;
        }
#ifdef HSF_DEBUG
        if (wp == 1) { KillTimer(hwnd, 1); SavePreview(); }
        else if (wp == 2) { KillTimer(hwnd, 2); RunTestScript(); }
#endif
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
#ifdef HSF_DEBUG
    cand = L"F:\\code\\HScreenFilter\\webui2\\index.html";
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) { outPath = cand; outUrl = ToFileUrl(cand); return; }
#endif
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

    AppLog(L"=== HScreenFilter %s start (build %s) ===",
        kVersionString, Utf8ToWide(__DATE__ " " __TIME__).c_str());

    // 单实例：桥接版此前没有互斥（原生版有），同时开两个实例会有两条 topmost 覆盖层
    // 各自做桌面复制、互相抢帧 —— 实测表现为 HDR 下捕获帧格式来回跳、画面闪烁/黑屏。
    // 已有实例：把它的主窗口叫到前面然后退出。
    {
        HANDLE onceMutex = CreateMutexW(nullptr, TRUE, L"HScreenFilter_SingleInstance");
        DWORD mutexErr = GetLastError();
        if (!onceMutex || mutexErr == ERROR_ALREADY_EXISTS)
        {
            HWND existing = FindWindowW(L"HScreenFilterMainWindow", nullptr);
            AppLog(L"[init] 已有实例在运行（hwnd=%p），本次启动退出", existing);
            if (existing)
            {
                // 交给已有实例自己走 ShowWindowApp：它会同时恢复 WebView2 可见性/尺寸。
                PostMessageW(existing, kMsgShowWindow, 0, 0);
            }
            if (onceMutex) CloseHandle(onceMutex);
            // 直接终止：此路径没有任何需要清理的资源，避免异步日志的静态析构 join
            // 因日志文件被另一个实例占用而把「第二个实例」卡在退出阶段。
            ExitProcess(0);
        }
    }

    // 命令行开关：--capturable / --no-capturable 临时覆盖"允许捕获"（不写回配置）。
    // 用途：排查 NVIDIA 录制（ShadowPlay/即时重放）与覆盖层捕获亲和性的相互影响。
    bool cliCapturable = false, cliNoCapturable = false;
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            for (int i = 1; i < argc; i++)
            {
                if (_wcsicmp(argv[i], L"--capturable") == 0) cliCapturable = true;
                else if (_wcsicmp(argv[i], L"--no-capturable") == 0) cliNoCapturable = true;
            }
            LocalFree(argv);
        }
    }

    // 数据 + 引擎
    data_ = g_store.Load();
    // 自启状态以注册表为准（旧版本可能只写了注册表没写配置，或用户手动删除了项）
    data_.AutoStart = AutoStart::IsEnabled();
    if (data_.Theme != L"light" && data_.Theme != L"dark") data_.Theme = L"auto";
    // Captureable 是旧配置/命令行字段，保留 JSON 兼容，但 LUT 覆盖层不允许参与捕获：
    // 覆盖层一旦对 DXGI 桌面复制可见，滤镜就会捕获自己的输出并迅速全黑。
    // 这里强制关闭并保留一条日志，避免旧 profiles.json 里 Captureable=true 的用户继续黑屏。
    bool captureRequested = data_.Captureable || cliCapturable;
    if (captureRequested)
        AppLog(L"[engine] 已忽略「允许捕获」：LUT 覆盖层必须排除自身捕获，否则会自反馈全黑");
    data_.Captureable = false;
    (void)cliNoCapturable;
    InitializeDisplays();
    savedSnapshot_ = CurDisplay().Current.Clone();
    savedUseDxgi_ = data_.UseDxgi;
    savedUseVsync_ = CurDisplay().UseVsync;

    FilterEngine::Instance().SetUseDxgi(data_.UseDxgi);
    FilterEngine::Instance().Initialize();
    FilterEngine::Instance().SetOverlayCapturable(data_.Captureable);
    AppLog(L"[data] profiles=%d bindings=%d displays=%d useDxgi=%d engine=%d capturable=%d%s",
        (int)data_.Profiles.size(), (int)data_.AppBindings.size(), (int)data_.Displays.size(),
        data_.UseDxgi ? 1 : 0, (int)FilterEngine::Instance().Kind(),
        data_.Captureable ? 1 : 0,
        (cliCapturable || cliNoCapturable) ? L"（命令行覆盖）" : L"");
    // 不在进程刚起来时就 ApplyCurrent：此时 WebView2/主窗口还在冷启动，
    // 同时创建全屏覆盖层 + 180fps 渲染线程会互相抢 GPU/CPU，首启表现为大幅卡顿。
    // 改为等页面 ready 后由 RequestInitialApply() 错峰启动；6 秒兜底。
    g_initialApplyPending = true;

    // 后台消息窗口 + 热键
    msgWindow_.Create();
    hotkeys_ = std::make_unique<HotkeyService>(msgWindow_);
    for (int i = 0; i < (int)data_.Profiles.size(); i++) RegisterProfileHotkey(i);
    RegisterGlobalToggle();

    // 前台监听
    watcher_ = std::make_unique<ForegroundAppWatcher>();
    watcher_->MatchChanged = [](int hit, const std::wstring &proc, const std::wstring &title) {
        AppLog(L"[watcher] callback fired hit=%d proc=%s", hit, proc.c_str());
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
        else if (g_taskbarCreatedMsg != 0 && m == g_taskbarCreatedMsg)
        {
            // explorer 重启后托盘图标会被系统清掉且不会自动恢复 —— 不重新添加的话，
            // 之后点托盘（含双击）永远没有反应。
            AppLog(L"[tray] 收到 TaskbarCreated，重新添加托盘图标");
            RemoveTray();
            AddTray();
        }
    });

    if (!LoadWebView2Loader())
    {
        AppLog(L"[init] WebView2Loader.dll 未找到");
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
            AppLog(L"[init] window scaled to fit work area (factor=%.2f) -> %dx%d phys", f, physW, physH);
        }
    }

    int w = physW, h = physH;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    g_wndW = rc.right - rc.left; g_wndH = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    std::wstring wndTitle = Format(L"HScreenFilter %s", kVersionString);
    // 自启进托盘：窗口仍需「先真正显示一次」再初始化 WebView2，否则控制器不会建立
    // 可见合成表面，之后从托盘恢复只有白底（已复现）。这里先显示在屏幕外，
    // 页面 ready 后再隐藏并归位，用户看不到闪烁。
    g_startHidden = AutoStartLaunched() && data_.MinimizeToTray;
    g_normalX = (sx - g_wndW) / 2;
    g_normalY = (sy - g_wndH) / 3;
    int createX = g_startHidden ? (-g_wndW - 64) : g_normalX;
    int createY = g_normalY;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, wndTitle.c_str(),
                                style, createX, createY, g_wndW, g_wndH,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { AppLog(L"[init] CreateWindow failed: %lu", GetLastError()); return 1; }
    g_hwnd = hwnd;
    AppLog(L"[init] 主窗口已创建：startHidden=%d 位置=(%d,%d) 归位=(%d,%d)",
           g_startHidden ? 1 : 0, createX, createY, g_normalX, g_normalY);

    // 标题栏与 UI 融为一体（WinUI 观感）：把窗口框架延伸到客户区，
    // 标题栏区域变成透明、由网页内容填充；标题栏高度内的拖动交给 WM_NCHITTEST
    // 返回 HTCAPTION 处理（见 WndProc），右侧留给系统的最小化/最大化/关闭按钮。
    {
        MARGINS margins{ -1, -1, -1, -1 };
        HRESULT hr = DwmExtendFrameIntoClientArea(hwnd, &margins);
        if (FAILED(hr)) AppLog(L"[ui] DwmExtendFrameIntoClientArea 失败 0x%08X", (unsigned)hr);

        // Win11 圆角（老系统忽略失败）
        DWORD corner = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));

        // 先用系统主题给标题栏上色；网页加载后会按「UI 实际生效主题」再校正一次
        DWORD appsUseLight = 1, size = sizeof(appsUseLight);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLight, &size);
        ApplyTitleBarTheme(appsUseLight == 0);
    }

    std::wstring pagePath, pageUrl;
    ResolveWebUi(pagePath, pageUrl);
    g_url = pageUrl;
    AppLog(L"[init] webui: %s", pagePath.c_str());

    wchar_t la[512];
    std::wstring userData;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, 512))
        userData = std::wstring(la) + L"\\HScreenFilter\\Browser";

    // 无论是否自启进托盘，创建 WebView2 控制器前窗口必须处于「已显示」状态。
    // 自启时用 SW_SHOWNOACTIVATE 显示在屏幕外，不抢焦点；就绪后 FinishStartupVisibility 隐藏并归位。
    ShowWindow(hwnd, g_startHidden ? SW_SHOWNOACTIVATE : SW_SHOW);
    UpdateWindow(hwnd);

    // explorer 重启通知：注册后才能在托盘图标被系统清掉时重新添加
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // 着色器预热与 WebView2 初始化并行：两者都在后台/异步，不阻塞创建窗口。
    PrewarmLutShaders();
    InitWebView2(userData);
    AddTray();
    // 环境/控制器创建本身也可能卡住（WebView2 Runtime 异常、用户数据目录被锁）：
    // 这里先起 12 秒看门狗，controller 回调成功后会重置它。
    SetTimer(hwnd, kTimerWebView, 12000, nullptr);

    // 页面 ready 是首选启动时机；这里 6 秒兜底，避免 WebView2 异常时滤镜永远不启动。
    SetTimer(hwnd, kTimerApply, 6000, nullptr);

    // 引擎看门狗：每 5 秒检查一次 LUT 引擎/伽马曲线是否被系统或驱动重置
    SetTimer(hwnd, kTimerWatchdog, 5000, nullptr);
    // 实时帧率推送（仅在用户开启「显示滤镜实时帧率」时发送，见 PushPresentFps）
    if (data_.ShowFps)
    {
        for (int i = 0; i < (int)data_.Displays.size(); i++)
            FilterEngine::Instance().SetFpsDetail(i, true);
        SetTimer(hwnd, kTimerFps, 500, nullptr);
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    if (g_shaderPrewarm.joinable()) g_shaderPrewarm.join();
    AppLog(L"=== HScreenFilter exit ===");
    if (SUCCEEDED(coInit)) CoUninitialize();
    return (int)m.wParam;
}
