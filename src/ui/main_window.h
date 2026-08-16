// main_window.h — 主窗口：导航式原生控件 UI（类系统设置/Android 设置）。
// 左侧导航 + 右侧内容页（基础调节 / HSL 调色盘 / 按应用切换 / 配置与快捷键），
// 无整体滚动；全部使用系统标准控件（comctl32 v6 主题），由系统处理 DPI/命中/绘制。
#pragma once
#include "common.h"
#include "models.h"
#include "store.h"
#include "msgwindow.h"
#include "hotkeys.h"
#include "tray.h"
#include "fgwatcher.h"
#include "engines/filter_engine.h"
#include <commctrl.h>
#include <map>
#include <set>

namespace hsf {

// 页面
enum PageId
{
    PAGE_BASE = 0,
    PAGE_HSL,
    PAGE_PERAPP,
    PAGE_PROFILES,
    PAGE_COUNT,
};

// 控件 ID
enum CtrlId
{
    IDC_STATUS = 1,
    IDC_DEBUG_LOG,
    IDC_NAV_LIST,
    IDC_DISPLAY_COMBO,
    IDC_DISPLAY_ENABLE,
    IDC_ENABLE_FILTER,
    IDC_HOTKEY_SET_GLOBAL,
    IDC_HOTKEY_CLEAR_GLOBAL,
    IDC_GLOBAL_HINT,

    IDC_SLIDER_BASE_FIRST = 100, // 基础滑块 100..105
    IDC_EDIT_BASE_FIRST = 200,   // 数值框 200..205
    IDC_PRESET_DEFAULT,
    IDC_PRESET_EYE,
    IDC_PRESET_NIGHT,
    IDC_PRESET_VIVID,

    IDC_LUT_SWITCH,              // LUT（原 DXGI）引擎开关
    IDC_VSYNC_SWITCH,
    IDC_HSL_TAB,
    IDC_SLIDER_HSL_FIRST = 300,  // HSL 滑块 300..326
    IDC_EDIT_HSL_FIRST = 400,
    IDC_HSL_RESET,
    IDC_HSL_HINT,

    IDC_PERAPP_SWITCH,
    IDC_BINDING_LIST,
    IDC_BINDING_ADD,
    IDC_BINDING_PICK,
    IDC_BINDING_EDIT,
    IDC_BINDING_DELETE,
    IDC_PERAPP_STATUS,

    IDC_PROFILE_LIST,
    IDC_PROFILE_NEW,
    IDC_PROFILE_RENAME,
    IDC_PROFILE_DELETE,
    IDC_PROFILE_IMPORT,
    IDC_PROFILE_EXPORT,
    IDC_PROFILE_UP,
    IDC_PROFILE_DOWN,
    IDC_HOTKEY_SET,
    IDC_HOTKEY_CLEAR,
    IDC_HOTKEY_HINT,
    IDC_THEME_COMBO,
    IDC_CAPTURE_SWITCH,
    IDC_AUTOSTART_SWITCH,
    IDC_AUTOTRAY_SWITCH,

    IDC_SAVEBAR_TEXT,
    IDC_SAVEBAR_SAVE,
    IDC_SAVEBAR_CANCEL,
};

enum TimerId
{
    TimerApply = 1,
    TimerSave = 2,
    TimerHint = 3,
};

// 滑块定义（HSL）
struct SliderSpec
{
    const wchar_t* label;
    double min, max;
    double displayBase;         // NaN = 显示原值
    std::vector<DWORD> spectrum; // 0xRRGGBB
    int field = 0;              // 0 色相 / 1 饱和度 / 2 明度
    int channel = -1;           // 0..8
};

class MainWindow
{
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInst);
    void Show();
    void Hide();
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    void InvokeUi(std::function<void()> fn);

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnCommand(int id, HWND hwndCtl, int code);
    void OnHScroll(int sliderId, int pos);
    void OnNotify(LPNMHDR nm);
    void OnTimer(UINT_PTR id);

    // 布局与页面
    void BuildChrome();          // 标题栏 + 头部 + 导航（只建一次）
    void ShowPage(int page);     // 切换页面（销毁旧页控件并重建）
    void DestroyPage();
    void BuildPageBase();
    void BuildPageHsl();
    void BuildPagePerApp();
    void BuildPageProfiles();
    void ApplyPagePositions();   // 页面控件按当前 DPI 重新定位（DPI 变化时）

    // 控件工厂（创建后若 buildingPage_ 则登记到 pageControls_）
    HWND AddStatic(int id, const wchar_t* text, int x, int y, int w, int h,
                   int fontSize, bool bold = false, DWORD align = SS_LEFT);
    HWND AddButton(int id, const wchar_t* text, int x, int y, int w, int h);
    HWND AddCheck(int id, const wchar_t* text, int x, int y, int w, int h);
    HWND AddTrack(int id, int x, int y, int w, int h, double min, double max);
    HWND AddEdit(int id, int x, int y, int w, int h);
    HWND AddCombo(int id, int x, int y, int w, int h);
    HWND AddList(int id, int x, int y, int w, int h, const wchar_t* col1,
                 const wchar_t* col2, const wchar_t* col3, bool checkboxes);
    HWND AddTab(int id, int x, int y, int w, int h, const std::vector<const wchar_t*>& tabs);
    int Dip(int v) const { return (int)(v * scale_ + 0.5f); }
    void SetFont(HWND ctl, int fontSizeDip, bool bold = false);

    void BuildHslSpecs();
    void RebuildHslPage(int page);
    void UpdateSliderUi(int sliderId, double value, bool updateEdit);
    void ApplySliderValue(int sliderId, double value);
    void OnDpiChanged(UINT newDpi);

    // 应用逻辑
    DisplayState& CurrentDisplay();
    Profile* ActiveProfile();
    int ActiveProfileIndex();
    void InitializeDisplays();
    void SwitchDisplay(int index);
    void UpdateDisplayEnableStatus();
    void ScheduleApply();
    void ApplyCurrent();
    void SaveState();
    void LoadSettingsIntoUi(const FilterSettings& s);
    void LoadHslSliders(FilterSettings& s);
    void SetHslValue(FilterSettings& s, int channel, int field, double value);
    double ChannelValue(const FilterSettings& s, int channel, int field) const;
    void HslReset();
    void UpdateEngineStatus();
    void UpdateHslHint();
    void ApplyLutMode(bool useLut);
    void ApplyLutModeNoDialog(bool useLut);
    void ApplyProfileLut(int profileIndex);
    void OnLutSwitchToggled();
    void UpdateGlobalHotkeyBadge();
    void RefreshProfileList();
    void RefreshBindingList();
    void UpdatePerAppStatus();
    void StartPerAppWatching();
    void StopPerAppWatching();
    void ForegroundMatchChanged(int hitBindingIndex, const std::wstring& proc, const std::wstring& title);
    void SetActiveProfileAndApply(int index);
    void ActivateProfile(int index) { SetActiveProfileAndApply(index); }
    void DeactivateProfile();
    void SyncAppliedProfileSwitch(int profileIndex);
    void RegisterProfileHotkey(int index);
    void RegisterGlobalToggle();
    void ToggleGlobal();
    void ToggleProfileHotkey(int index);
    void TryEnsureUiTopmost();
    void ApplyWindowTheme();
    void HandleHotkeyCapture(int vk);
    void ShutdownApp();
    bool AutoStartLaunched();
    void MarkFilterChanged();
    void ApplyPreset(const FilterSettings& preset);
    void ScheduleSave();

    // 对话框动作
    void OnNewProfile();
    void OnRenameProfile();
    void OnDeleteProfile();
    void OnMoveProfile(int delta);
    void OnImportProfile();
    void OnExportProfile();
    void OnAddBinding();
    void OnEditBinding();
    void OnDeleteBinding();
    void OnPickForeground();
    void OnCaptureHotkey();
    void OnCaptureGlobalHotkey();
    void OnClearHotkey();
    void OnClearGlobalHotkey();
    void OnOpenDebugLog();

    // 保存条
    void ShowSaveBar();
    void HideSaveBar();
    void SaveBarSave();
    void SaveBarCancel();
    void PositionSaveBar();
    static LRESULT CALLBACK SaveBarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND h, UINT m, WPARAM wp, LPARAM lp);

    // 成员
    Store store_;
    ProfileData data_;
    std::vector<DisplayMonitor> monitors_;
    int currentDisplayIndex_ = 0;

    MessageWindow msgWindow_;
    std::unique_ptr<HotkeyService> hotkeys_;
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<ForegroundAppWatcher> watcher_;

    std::map<int, int> profileHotkeyIds_;
    int globalHotkeyId_ = 0;

    bool uiInit_ = false, displayInit_ = false, perAppInit_ = false, profileToggleInit_ = false;
    bool lutInit_ = false, themeInit_ = false, pendingEdit_ = false;
    bool closingToTray_ = true, shutdownStarted_ = false, destroying_ = false;
    bool startToTray_ = false, trayHideDone_ = false, sizeApplied_ = false;
    int capturingFor_ = -1;
    bool capturingGlobal_ = false;
    int activeBinding_ = -1;
    FilterSettings savedSnapshot_;

    // 布局
    float scale_ = 1.f;
    int cx0_ = 160;   // 内容区左缘（DIP）
    int cx1_ = 664;   // 内容区右缘
    int cy0_ = 116;   // 内容区顶（DIP）

    // 控件
    HWND hwnd_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    HWND hStatus = nullptr, hNav = nullptr, hDisplayCombo = nullptr, hDisplayEnable = nullptr,
         hEnableFilter = nullptr, hGlobalHint = nullptr;

    int currentPage_ = -1;
    bool buildingPage_ = false;
    std::vector<HWND> pageControls_;

    HWND pgSliderBase[6] = {}, pgEditBase[6] = {};
    HWND pgLutSwitch = nullptr, pgVsyncSwitch = nullptr, pgHslTab = nullptr, pgHslHint = nullptr;
    HWND pgSliderHsl[27] = {}, pgEditHsl[27] = {};
    HWND pgPerAppSwitch = nullptr, pgBindingList = nullptr, pgPerAppStatus = nullptr;
    HWND pgProfileList = nullptr, pgHotkeyHint = nullptr, pgThemeCombo = nullptr;
    HWND pgCaptureSwitch = nullptr, pgAutoStartSwitch = nullptr, pgAutoTraySwitch = nullptr;

    std::vector<SliderSpec> hslSpecs_;
    int hslPage_ = 0;

    // 滑块行矩形（px，渐变条背景装饰）
    struct SliderRow { HWND track; RECT rc; std::vector<DWORD> spectrum; };
    std::vector<SliderRow> sliderRows_;

    HWND hSaveBar = nullptr;
    HBRUSH bgBrush_ = nullptr;
};

} // namespace hsf
