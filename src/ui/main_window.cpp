// main_window.cpp — 导航式原生控件 UI。
// 左侧导航 + 右侧内容页，无整体滚动；全部系统标准控件（comctl32 v6 现代主题）。
#include "main_window.h"
#include "dialogs.h"
#include "log.h"
#include "autostart.h"
#include "monitors.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace hsf {

namespace {
constexpr int kSliderRowH = 46;
constexpr int kLabelW = 170;          // 滑块标题宽
constexpr int kEditW = 56;            // 数值框宽
constexpr int kTrackGap = 8;
}

// ---------------- 构造/析构 ----------------

MainWindow::MainWindow() = default;

MainWindow::~MainWindow()
{
    Destroy();
}

void MainWindow::InvokeUi(std::function<void()> fn)
{
    if (!hwnd_) { fn(); return; }
    {
        static std::mutex qm;
        static std::vector<std::function<void()>> q;
        std::lock_guard<std::mutex> lock(qm);
        q.push_back(std::move(fn));
    }
    PostMessageW(hwnd_, WM_APP + 1, 0, 0);
}

// ---------------- 窗口创建 ----------------

bool MainWindow::Create(HINSTANCE hInst)
{
    hInst_ = hInst;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, L"APPICON", IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"HScreenFilterMainWindow";
    RegisterClassExW(&wc);

    HDC hdc = GetDC(nullptr);
    scale_ = GetDeviceCaps(hdc, LOGPIXELSX) / 96.f;
    ReleaseDC(nullptr, hdc);
    if (scale_ < 1.f) scale_ = 1.f;

    // 以“客户区 680x860 DIP”为准计算窗口尺寸（含边框）。
    // 固定尺寸：不允许缩放，避免控件随窗口变化产生重叠/裁切。
    DWORD wndStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, Dip(680), Dip(860) };
    AdjustWindowRectEx(&rc, wndStyle, FALSE, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    int x = (sx - w) / 2, y = (sy - h) / 3;

    hwnd_ = CreateWindowExW(0, L"HScreenFilterMainWindow", L"HScreenFilter",
                            wndStyle, x, y, w, h, nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    bgBrush_ = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));

    ApplyWindowTheme();

    // ---- 初始化应用逻辑 ----
    data_ = store_.Load();
    InitializeDisplays();

    bool startupUseLut = data_.UseDxgi;
    if (CurrentDisplay().ActiveProfileIndex >= 0 && CurrentDisplay().ActiveProfileIndex < (int)data_.Profiles.size())
        startupUseLut = data_.Profiles[CurrentDisplay().ActiveProfileIndex].UseDxgi;
    FilterEngine::Instance().SetUseDxgi(startupUseLut);
    data_.UseDxgi = startupUseLut;
    FilterEngine::Instance().Initialize();
    UpdateEngineStatus();

    if (CurrentDisplay().ActiveProfileIndex >= 0 && CurrentDisplay().ActiveProfileIndex < (int)data_.Profiles.size())
    {
        profileToggleInit_ = true;
        data_.Profiles[CurrentDisplay().ActiveProfileIndex].IsActive = true;
        profileToggleInit_ = false;
        CurrentDisplay().Current = data_.Profiles[CurrentDisplay().ActiveProfileIndex].Settings.Clone();
    }
    else
    {
        CurrentDisplay().Current = FilterSettings();
    }
    savedSnapshot_ = CurrentDisplay().Current.Clone();

    watcher_ = std::make_unique<ForegroundAppWatcher>();
    watcher_->MatchChanged = [this](int hit, const std::wstring& proc, const std::wstring& title) {
        InvokeUi([this, hit, proc, title]() { ForegroundMatchChanged(hit, proc, title); });
    };

    msgWindow_.Create();
    hotkeys_ = std::make_unique<HotkeyService>(msgWindow_);
    for (int i = 0; i < (int)data_.Profiles.size(); i++)
        RegisterProfileHotkey(i);
    RegisterGlobalToggle();

    tray_ = std::make_unique<TrayIcon>(msgWindow_);
    tray_->OnShow = [this]() { InvokeUi([this]() { Show(); }); };
    tray_->OnExit = [this]() { InvokeUi([this]() { closingToTray_ = false; ShutdownApp(); }); };
    tray_->ProfilesProvider = [this]() {
        std::vector<std::pair<std::wstring, bool>> items;
        for (auto& p : data_.Profiles) items.emplace_back(p.Name, p.IsActive);
        return items;
    };
    tray_->ProfileSelected = [this](int index) {
        InvokeUi([this, index]() { if (index >= 0 && index < (int)data_.Profiles.size()) ActivateProfile(index); });
    };
    tray_->Show();

    BuildChrome();
    currentPage_ = -1;
    ShowPage(PAGE_BASE);
    RefreshBindingList();
    UpdatePerAppStatus();

    startToTray_ = AutoStartLaunched() && data_.MinimizeToTray;

    Log::Write(L"UI", L"MainWindow 初始化完成（导航式原生控件）");
    return true;
}

void MainWindow::Show()
{
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void MainWindow::Hide()
{
    ShowWindow(hwnd_, SW_HIDE);
}

void MainWindow::Destroy()
{
    if (!hwnd_) return;
    watcher_.reset();
    tray_.reset();
    hotkeys_.reset();
    msgWindow_.Destroy();
    FilterEngine::Instance().Shutdown();
    if (bgBrush_) { DeleteObject(bgBrush_); bgBrush_ = nullptr; }
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

// ---------------- 控件工厂 ----------------

void MainWindow::SetFont(HWND ctl, int fontSizeDip, bool bold)
{
    if (!ctl) return;
    static std::map<int, HFONT> fonts;
    int key = (fontSizeDip << 1) | (bold ? 1 : 0);
    auto it = fonts.find(key);
    if (it == fonts.end())
    {
        int px = Dip(fontSizeDip);
        HFONT f = CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        fonts[key] = f;
        it = fonts.find(key);
    }
    SendMessageW(ctl, WM_SETFONT, (WPARAM)it->second, TRUE);
}

static HWND MakeChild(HWND parent, HINSTANCE inst, const wchar_t* cls, const wchar_t* text,
                      DWORD style, int id, int x, int y, int w, int h, int ex = 0)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, inst, nullptr);
}

HWND MainWindow::AddStatic(int id, const wchar_t* text, int x, int y, int w, int h,
                           int fontSize, bool bold, DWORD align)
{
    HWND c = MakeChild(hwnd_, hInst_, L"STATIC", text, align, id, Dip(x), Dip(y), Dip(w), Dip(h));
    SetFont(c, fontSize, bold);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

HWND MainWindow::AddButton(int id, const wchar_t* text, int x, int y, int w, int h)
{
    HWND c = MakeChild(hwnd_, hInst_, L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, id, Dip(x), Dip(y), Dip(w), Dip(h));
    SetFont(c, 13);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

HWND MainWindow::AddCheck(int id, const wchar_t* text, int x, int y, int w, int h)
{
    HWND c = MakeChild(hwnd_, hInst_, L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, Dip(x), Dip(y), Dip(w), Dip(h));
    SetFont(c, 13);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

HWND MainWindow::AddTrack(int id, int x, int y, int w, int h, double min, double max)
{
    HWND c = MakeChild(hwnd_, hInst_, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                       id, Dip(x), Dip(y), Dip(w), Dip(h));
    SendMessageW(c, TBM_SETRANGEMIN, TRUE, (int)min);
    SendMessageW(c, TBM_SETRANGEMAX, TRUE, (int)max);
    SendMessageW(c, TBM_CLEARTICS, TRUE, 0);
    SetFont(c, 13);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

HWND MainWindow::AddEdit(int id, int x, int y, int w, int h)
{
    HWND c = MakeChild(hwnd_, hInst_, L"EDIT", L"0", WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL,
                       id, Dip(x), Dip(y), Dip(w), Dip(h), WS_EX_CLIENTEDGE);
    SetFont(c, 12);
    SetWindowLongPtrW(c, GWLP_USERDATA, (LONG_PTR)(INT_PTR)id);
    SetWindowLongPtrW(c, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

LRESULT CALLBACK MainWindow::EditSubclassProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    int editId = (int)(INT_PTR)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_KEYDOWN)
    {
        if (wp == VK_RETURN) { PostMessageW(GetParent(h), WM_APP + 2, (WPARAM)(INT_PTR)editId, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(GetParent(h), WM_APP + 3, 0, 0); return 0; }
    }
    if (m == WM_KILLFOCUS) { PostMessageW(GetParent(h), WM_APP + 2, (WPARAM)(INT_PTR)editId, 0); return 0; }
    return DefWindowProcW(h, m, wp, lp);
}

HWND MainWindow::AddCombo(int id, int x, int y, int w, int h)
{
    HWND c = MakeChild(hwnd_, hInst_, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                       id, Dip(x), Dip(y), Dip(w), Dip(200));
    SetFont(c, 12);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

HWND MainWindow::AddList(int id, int x, int y, int w, int h, const wchar_t* col1,
                         const wchar_t* col2, const wchar_t* col3, bool checkboxes)
{
    HWND c = MakeChild(hwnd_, hInst_, WC_LISTVIEWW, L"",
                       WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                       id, Dip(x), Dip(y), Dip(w), Dip(h), WS_EX_CLIENTEDGE);
    DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
    if (checkboxes) ex |= LVS_EX_CHECKBOXES;
    ListView_SetExtendedListViewStyle(c, ex);
    LVCOLUMNW lc{};
    lc.mask = LVCF_TEXT | LVCF_WIDTH;
    if (col1) { lc.cx = Dip(180); lc.pszText = (LPWSTR)col1; ListView_InsertColumn(c, 0, &lc); }
    if (col2) { lc.cx = Dip(80); lc.pszText = (LPWSTR)col2; ListView_InsertColumn(c, 1, &lc); }
    if (col3) { lc.cx = Dip(120); lc.pszText = (LPWSTR)col3; ListView_InsertColumn(c, 2, &lc); }
    SetFont(c, 12);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

HWND MainWindow::AddTab(int id, int x, int y, int w, int h, const std::vector<const wchar_t*>& tabs)
{
    HWND c = MakeChild(hwnd_, hInst_, WC_TABCONTROLW, L"", WS_TABSTOP | TCS_TABS,
                       id, Dip(x), Dip(y), Dip(w), Dip(h));
    for (size_t i = 0; i < tabs.size(); i++)
    {
        TCITEMW ti{ TCIF_TEXT, 0, 0, (LPWSTR)tabs[i] };
        TabCtrl_InsertItem(c, (int)i, &ti);
    }
    SetFont(c, 12);
    if (buildingPage_) pageControls_.push_back(c);
    return c;
}

// ---------------- 布局构建 ----------------

void MainWindow::BuildChrome()
{
    buildingPage_ = false;
    // 标题栏
    AddStatic(0, L"屏幕滤镜", 16, 6, 90, 28, 16, true);
    hStatus = AddStatic(IDC_STATUS, L"正在初始化…", 116, 10, 460, 22, 12);
    AddButton(IDC_DEBUG_LOG, L"调试日志", 584, 4, 80, 28);
    // 头部
    AddStatic(0, L"显示器", 16, 44, 60, 24, 12);
    hDisplayCombo = AddCombo(IDC_DISPLAY_COMBO, 76, 42, 210, 200);
    for (auto& m : monitors_)
        SendMessageW(hDisplayCombo, CB_ADDSTRING, 0, (LPARAM)m.Label().c_str());
    SendMessageW(hDisplayCombo, CB_SETCURSEL, 0, 0);
    hDisplayEnable = AddCheck(IDC_DISPLAY_ENABLE, L"启用", 296, 44, 60, 24);
    hEnableFilter = AddCheck(IDC_ENABLE_FILTER, L"启用滤镜", 366, 44, 110, 24);
    AddButton(IDC_HOTKEY_SET_GLOBAL, L"全局开关快捷键…", 486, 42, 178, 28);
    hGlobalHint = AddStatic(IDC_GLOBAL_HINT, L"", 16, 72, 640, 20, 11);

    // 导航
    hNav = MakeChild(hwnd_, hInst_, WC_LISTVIEWW, L"",
                     WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
                     IDC_NAV_LIST, Dip(16), Dip(110), Dip(132), Dip(700), WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(hNav, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW lc{};
    lc.mask = LVCF_TEXT | LVCF_WIDTH;
    lc.cx = Dip(118);
    lc.pszText = (LPWSTR)L"";
    ListView_InsertColumn(hNav, 0, &lc);
    SetFont(hNav, 13);
    static const wchar_t* navItems[PAGE_COUNT] = {
        L"基础调节", L"HSL 调色盘", L"按应用切换", L"配置与快捷键",
    };
    for (int i = 0; i < PAGE_COUNT; i++)
    {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)navItems[i];
        ListView_InsertItem(hNav, &it);
    }

    CheckDlgButton(hwnd_, IDC_DISPLAY_ENABLE, CurrentDisplay().IsEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_ENABLE_FILTER, data_.IsEnabled ? BST_CHECKED : BST_UNCHECKED);
    UpdateDisplayEnableStatus();
    UpdateGlobalHotkeyBadge();
}

void MainWindow::DestroyPage()
{
    for (HWND c : pageControls_)
        if (c) DestroyWindow(c);
    pageControls_.clear();
    for (auto& s : pgSliderBase) s = nullptr;
    for (auto& e : pgEditBase) e = nullptr;
    pgLutSwitch = pgVsyncSwitch = pgHslTab = pgHslHint = nullptr;
    for (auto& s : pgSliderHsl) s = nullptr;
    for (auto& e : pgEditHsl) e = nullptr;
    pgPerAppSwitch = pgBindingList = pgPerAppStatus = nullptr;
    pgProfileList = pgHotkeyHint = pgThemeCombo = nullptr;
    pgCaptureSwitch = pgAutoStartSwitch = pgAutoTraySwitch = nullptr;
    sliderRows_.clear();
}

void MainWindow::ShowPage(int page)
{
    if (page < 0 || page >= PAGE_COUNT) page = 0;
    if (page == currentPage_ && !pageControls_.empty()) return;
    DestroyPage();
    currentPage_ = page;
    buildingPage_ = true;
    switch (page)
    {
    case PAGE_BASE: BuildPageBase(); break;
    case PAGE_HSL: BuildPageHsl(); break;
    case PAGE_PERAPP: BuildPagePerApp(); break;
    case PAGE_PROFILES: BuildPageProfiles(); break;
    }
    buildingPage_ = false;
    LoadSettingsIntoUi(CurrentDisplay().Current);
    UpdateEngineStatus();
    RefreshBindingList();
    UpdatePerAppStatus();
    RefreshProfileList();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::BuildPageBase()
{
    int x = cx0_, w = cx1_ - cx0_;
    int y = cy0_ + 4;
    AddStatic(0, L"基础调节", x, y, 150, 26, 15, true);
    AddStatic(0, L"拖动滑块即可实时调整滤镜的亮度、对比度、色彩和阴影表现。", x + 156, y + 2, w - 156, 20, 11);
    static const wchar_t* names[6] = { L"亮度 Brightness", L"对比度 Contrast", L"鲜艳度 Saturation",
                                       L"亮部 Highlights", L"暗部 Shadows", L"色温（冷 ← → 暖）" };
    static const double mins[6] = { -100, 0, 0, -100, -100, -100 };
    static const double maxs[6] = { 100, 200, 200, 100, 100, 100 };
    y += 38;
    for (int i = 0; i < 6; i++)
    {
        AddStatic(0, names[i], x, y, kLabelW, 22, 12);
        pgSliderBase[i] = AddTrack(100 + i, x + kLabelW + kTrackGap, y - 2, w - kLabelW - kTrackGap - kEditW - kTrackGap, 26, mins[i], maxs[i]);
        pgEditBase[i] = AddEdit(200 + i, x + w - kEditW, y, kEditW, 24);
        y += kSliderRowH;
    }
    AddStatic(0, L"快捷预设", x, y + 4, 120, 24, 13, true); y += 34;
    int bx = x;
    AddButton(IDC_PRESET_DEFAULT, L"默认值", bx, y, 80, 28); bx += 88;
    AddButton(IDC_PRESET_EYE, L"护眼", bx, y, 80, 28); bx += 88;
    AddButton(IDC_PRESET_NIGHT, L"夜间", bx, y, 80, 28); bx += 88;
    AddButton(IDC_PRESET_VIVID, L"鲜艳", bx, y, 80, 28);
}

void MainWindow::BuildPageHsl()
{
    int x = cx0_, w = cx1_ - cx0_;
    int y = cy0_ + 4;
    AddStatic(0, L"HSL 调色盘（3D LUT）", x, y, 260, 26, 15, true);
    AddStatic(0, L"按色系分别精细调整色相、饱和度、明亮度，各色系互不干扰。", x + 270, y + 2, w - 270, 20, 11);
    pgLutSwitch = AddCheck(IDC_LUT_SWITCH, L"LUT 引擎开关（开启 = 3D LUT 逐像素引擎，支持 HSL）", x, y + 38, w, 26);
    pgVsyncSwitch = AddCheck(IDC_VSYNC_SWITCH, L"垂直同步 V-Sync（防撕裂）", x, y + 70, w, 26);
    pgHslTab = AddTab(IDC_HSL_TAB, x, y + 106, w, 28, { L"色相", L"饱和度", L"明亮度" });
    CheckDlgButton(hwnd_, IDC_LUT_SWITCH, data_.UseDxgi ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_VSYNC_SWITCH, CurrentDisplay().UseVsync ? BST_CHECKED : BST_UNCHECKED);
    pgHslHint = AddStatic(IDC_HSL_HINT, L"", x, y + 566, w, 20, 11);
    AddButton(IDC_HSL_RESET, L"默认值", x, y + 602, 80, 28);
    BuildHslSpecs();
    RebuildHslPage(hslPage_);
    UpdateHslHint();
}

void MainWindow::BuildPagePerApp()
{
    int x = cx0_, w = cx1_ - cx0_;
    int y = cy0_ + 4;
    AddStatic(0, L"按应用切换滤镜", x, y, 200, 26, 15, true);
    AddStatic(0, L"让滤镜只在指定应用前台时启用，切到其它窗口时自动关闭。", x + 210, y + 2, w - 210, 20, 11);
    pgPerAppSwitch = AddCheck(IDC_PERAPP_SWITCH, L"启用按应用切换", x, y + 38, 200, 26);
    pgBindingList = AddList(IDC_BINDING_LIST, x, y + 74, w, 200, L"检测应用", L"绑定配置", nullptr, false);
    int by = y + 286;
    int bx = x;
    AddButton(IDC_BINDING_ADD, L"添加应用…", bx, by, 100, 26); bx += 108;
    AddButton(IDC_BINDING_PICK, L"用当前前台应用添加", bx, by, 150, 26); bx += 158;
    AddButton(IDC_BINDING_EDIT, L"编辑所选", bx, by, 80, 26); bx += 88;
    AddButton(IDC_BINDING_DELETE, L"删除所选", bx, by, 80, 26);
    pgPerAppStatus = AddStatic(IDC_PERAPP_STATUS, L"", x, by + 34, w, 20, 11);
    CheckDlgButton(hwnd_, IDC_PERAPP_SWITCH, data_.PerAppEnabled ? BST_CHECKED : BST_UNCHECKED);
}

void MainWindow::BuildPageProfiles()
{
    int x = cx0_, w = cx1_ - cx0_;
    int y = cy0_ + 4;
    AddStatic(0, L"配置与快捷键", x, y, 200, 26, 15, true);
    AddStatic(0, L"保存多套滤镜配置，快速切换并绑定全局快捷键。", x + 210, y + 2, w - 210, 20, 11);
    pgProfileList = AddList(IDC_PROFILE_LIST, x, y + 38, w, 200, L"名称", L"引擎", L"快捷键", true);
    int by = y + 248;
    int bx = x;
    AddButton(IDC_PROFILE_NEW, L"新建配置", bx, by, 90, 26); bx += 98;
    AddButton(IDC_PROFILE_RENAME, L"重命名", bx, by, 80, 26); bx += 88;
    AddButton(IDC_PROFILE_DELETE, L"删除配置", bx, by, 90, 26); bx += 98;
    AddButton(IDC_PROFILE_UP, L"上移", bx, by, 60, 26); bx += 68;
    AddButton(IDC_PROFILE_DOWN, L"下移", bx, by, 60, 26);
    by += 34;
    bx = x;
    AddButton(IDC_PROFILE_IMPORT, L"导入配置", bx, by, 100, 26); bx += 108;
    AddButton(IDC_PROFILE_EXPORT, L"导出所选配置", bx, by, 120, 26);
    by += 34;
    AddStatic(0, L"所选配置快捷键", x, by, 140, 24, 12);
    AddButton(IDC_HOTKEY_SET, L"设置快捷键…", x + 148, by - 2, 100, 26);
    AddButton(IDC_HOTKEY_CLEAR, L"清除", x + 256, by - 2, 60, 26);
    by += 32;
    pgHotkeyHint = AddStatic(IDC_HOTKEY_HINT, L"", x, by, w, 20, 11); by += 28;
    AddStatic(0, L"界面主题", x, by, 90, 24, 12);
    pgThemeCombo = AddCombo(IDC_THEME_COMBO, x + 96, by - 2, 160, 200);
    SendMessageW(pgThemeCombo, CB_ADDSTRING, 0, (LPARAM)L"默认主题");
    SendMessageW(pgThemeCombo, CB_ADDSTRING, 0, (LPARAM)L"Mica 主题");
    SendMessageW(pgThemeCombo, CB_SETCURSEL, data_.Theme == L"mica" ? 1 : 0, 0);
    by += 32;
    pgCaptureSwitch = AddCheck(IDC_CAPTURE_SWITCH, L"UI 可被 OBS 捕获", x, by, 200, 26); by += 30;
    pgAutoStartSwitch = AddCheck(IDC_AUTOSTART_SWITCH, L"开机自动启动", x, by, 200, 26); by += 30;
    pgAutoTraySwitch = AddCheck(IDC_AUTOTRAY_SWITCH, L"自启后自动进入托盘", x, by, 220, 26);
    CheckDlgButton(hwnd_, IDC_CAPTURE_SWITCH, data_.Captureable ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_AUTOSTART_SWITCH, AutoStart::IsEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_AUTOTRAY_SWITCH, data_.MinimizeToTray ? BST_CHECKED : BST_UNCHECKED);
}

// ---------------- HSL 滑块 ----------------

void MainWindow::BuildHslSpecs()
{
    hslSpecs_.clear();
    static const DWORD hues[8][3] = {
        { 0xFF00FF, 0xFF0000, 0xFF8000 }, { 0xFF0000, 0xFF8000, 0xFFFF00 },
        { 0xFF8000, 0xFFFF00, 0x00FF00 }, { 0xFFFF00, 0x00FF00, 0x00FFFF },
        { 0x00FF00, 0x00FFFF, 0x0000FF }, { 0x00FFFF, 0x0000FF, 0x8000FF },
        { 0x0000FF, 0x8000FF, 0xFF00FF }, { 0x8000FF, 0xFF00FF, 0xFF0000 },
    };
    static const wchar_t* names[9] = { L"全部（主）", L"红", L"橙", L"黄", L"绿", L"青", L"蓝", L"紫", L"品红" };
    static const DWORD satColors[9] = { 0xFF0000, 0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00,
                                        0x00FFFF, 0x0000FF, 0x8000FF, 0xFF00FF };
    for (int pass = 0; pass < 3; pass++)
    {
        for (int i = 0; i < 9; i++)
        {
            SliderSpec s;
            s.label = names[i];
            s.field = pass;
            s.channel = i;
            if (pass == 0) // 色相
            {
                s.min = (i == 0) ? -180 : -30;
                s.max = (i == 0) ? 180 : 30;
                s.displayBase = NAN;
                if (i == 0)
                    s.spectrum = { 0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0x8000FF, 0xFF00FF, 0xFF0000 };
                else
                    for (int k = 0; k < 3; k++) s.spectrum.push_back(hues[i - 1][k]);
            }
            else if (pass == 1) // 饱和度
            {
                s.min = 0; s.max = 200;
                s.displayBase = 100;
                s.spectrum = { 0x808080, satColors[i] };
            }
            else // 明亮度
            {
                s.min = -30; s.max = 30;
                s.displayBase = NAN;
                s.spectrum = { 0x000000, 0x808080, 0xFFFFFF };
            }
            hslSpecs_.push_back(s);
        }
    }
}

void MainWindow::RebuildHslPage(int page)
{
    if (!pgHslTab) return;
    int x = cx0_, w = cx1_ - cx0_;
    int y = cy0_ + 144;
    // 先销毁上一组滑块并从页面控件表移除
    for (int i = 0; i < 27; i++)
    {
        if (pgSliderHsl[i])
        {
            auto it = std::find(pageControls_.begin(), pageControls_.end(), pgSliderHsl[i]);
            if (it != pageControls_.end()) pageControls_.erase(it);
            DestroyWindow(pgSliderHsl[i]);
            pgSliderHsl[i] = nullptr;
        }
        if (pgEditHsl[i])
        {
            auto it = std::find(pageControls_.begin(), pageControls_.end(), pgEditHsl[i]);
            if (it != pageControls_.end()) pageControls_.erase(it);
            DestroyWindow(pgEditHsl[i]);
            pgEditHsl[i] = nullptr;
        }
    }
    sliderRows_.clear();
    bool wasBuilding = buildingPage_;
    buildingPage_ = true;
    for (int i = page * 9; i < page * 9 + 9; i++)
    {
        const SliderSpec& s = hslSpecs_[i];
        AddStatic(0, s.label, x, y, kLabelW, 22, 12);
        pgSliderHsl[i] = AddTrack(300 + i, x + kLabelW + kTrackGap, y - 2, w - kLabelW - kTrackGap - kEditW - kTrackGap, 26, s.min, s.max);
        pgEditHsl[i] = AddEdit(400 + i, x + w - kEditW, y, kEditW, 24);
        if (!s.spectrum.empty())
            sliderRows_.push_back({ pgSliderHsl[i], { Dip(x + kLabelW + kTrackGap), Dip(y + 24), Dip(w - kLabelW - kTrackGap - kEditW - kTrackGap), 6 }, s.spectrum });
        y += kSliderRowH;
    }
    buildingPage_ = wasBuilding;
    LoadHslSliders(CurrentDisplay().Current);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::UpdateSliderUi(int sliderId, double value, bool updateEdit)
{
    HWND track = nullptr, edit = nullptr;
    double base = NAN;
    if (sliderId >= 100 && sliderId < 106) { track = pgSliderBase[sliderId - 100]; edit = pgEditBase[sliderId - 100]; }
    else if (sliderId >= 300 && sliderId < 327) { track = pgSliderHsl[sliderId - 300]; edit = pgEditHsl[sliderId - 300]; base = hslSpecs_[sliderId - 300].displayBase; }
    if (!track) return;
    SendMessageW(track, TBM_SETPOS, TRUE, (int)value);
    if (updateEdit && edit && GetFocus() != edit)
    {
        std::wstring txt = std::isnan(base) ? NumToStr(value) : NumToStrRel(value, base);
        SetWindowTextW(edit, txt.c_str());
    }
}

void MainWindow::ApplySliderValue(int sliderId, double value)
{
    if (sliderId >= 100 && sliderId < 106)
    {
        auto& cur = CurrentDisplay().Current;
        switch (sliderId - 100)
        {
        case 0: cur.Brightness = value; break;
        case 1: cur.Contrast = value; break;
        case 2: cur.Saturation = value; break;
        case 3: cur.Highlights = value; break;
        case 4: cur.Shadows = value; break;
        case 5: cur.Temperature = value; break;
        }
        ScheduleApply();
    }
    else if (sliderId >= 300 && sliderId < 327)
    {
        int idx = sliderId - 300;
        if (idx < (int)hslSpecs_.size())
        {
            SetHslValue(CurrentDisplay().Current, hslSpecs_[idx].channel, hslSpecs_[idx].field, value);
            ScheduleApply();
        }
    }
}

// ---------------- 窗口过程 ----------------

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    else
    {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self)
    {
        if (!self->hwnd_) self->hwnd_ = hwnd;
        return self->WndProc(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        FillRect(hdc, &cr, bgBrush_);
        // 滑块渐变条（纯装饰）
        for (const auto& row : sliderRows_)
        {
            RECT r = row.rc;
            if (r.bottom < 0 || r.top > cr.bottom) continue;
            const auto& sp = row.spectrum;
            if (sp.size() >= 2)
            {
                int segW = r.right - r.left;
                int n = (int)sp.size() - 1;
                for (int i = 0; i < n; i++)
                {
                    int x0 = r.left + segW * i / n;
                    int x1 = r.left + segW * (i + 1) / n;
                    if (x1 <= x0) continue;
                    for (int xx = x0; xx < x1; xx++)
                    {
                        float t = (float)(xx - x0) / (float)(x1 - x0);
                        int rr = (int)(((sp[i] >> 16) & 0xFF) + (((sp[i + 1] >> 16) & 0xFF) - ((sp[i] >> 16) & 0xFF)) * t);
                        int gg = (int)(((sp[i] >> 8) & 0xFF) + (((sp[i + 1] >> 8) & 0xFF) - ((sp[i] >> 8) & 0xFF)) * t);
                        int bb = (int)((sp[i] & 0xFF) + ((sp[i + 1] & 0xFF) - (sp[i] & 0xFF)) * t);
                        HPEN p = CreatePen(PS_SOLID, 1, RGB(rr, gg, bb));
                        HGDIOBJ op = SelectObject(hdc, p);
                        MoveToEx(hdc, xx, r.top, nullptr);
                        LineTo(hdc, xx, r.bottom);
                        SelectObject(hdc, op);
                        DeleteObject(p);
                    }
                }
            }
        }
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_DPICHANGED:
    {
        UINT dpi = HIWORD(wParam);
        scale_ = dpi / 96.f;
        if (scale_ < 1.f) scale_ = 1.f;
        RECT* r = (RECT*)lParam;
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        DestroyPage();
        HWND child = GetWindow(hwnd_, GW_CHILD);
        while (child)
        {
            HWND next = GetWindow(child, GW_HWNDNEXT);
            DestroyWindow(child);
            child = next;
        }
        BuildChrome();
        ShowPage(currentPage_);
        RefreshBindingList();
        UpdatePerAppStatus();
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        // 固定尺寸窗口：禁止缩小到内容以下
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = Dip(680);
        mmi->ptMinTrackSize.y = Dip(860);
        return 0;
    }
    case WM_COMMAND:
        OnCommand(LOWORD(wParam), (HWND)lParam, HIWORD(wParam));
        return 0;
    case WM_HSCROLL:
        OnHScroll(GetDlgCtrlID((HWND)lParam), (int)SendMessageW((HWND)lParam, TBM_GETPOS, 0, 0));
        return 0;
    case WM_NOTIFY:
        OnNotify((LPNMHDR)lParam);
        return 0;
    case WM_TIMER:
        OnTimer((UINT_PTR)wParam);
        return 0;
    case WM_ACTIVATE:
        if (startToTray_ && !trayHideDone_)
        {
            trayHideDone_ = true;
            Hide();
            if (tray_) tray_->ShowBalloon(L"屏幕滤镜已在后台运行", L"已自动最小化到系统托盘。");
        }
        return 0;
    case WM_CLOSE:
        if (closingToTray_)
        {
            Hide();
            if (tray_) tray_->ShowBalloon(L"屏幕滤镜仍在运行", L"已最小化到系统托盘，右键托盘图标可退出。");
            return 0;
        }
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        destroying_ = true;
        ShutdownApp();
        PostQuitMessage(0);
        return 0;
    case WM_APP + 1:
        for (;;)
        {
            std::function<void()> fn;
            {
                static std::mutex qm;
                static std::vector<std::function<void()>> q;
                std::lock_guard<std::mutex> lock(qm);
                if (q.empty()) break;
                fn = std::move(q.front());
                q.erase(q.begin());
            }
            if (fn) fn();
        }
        return 0;
    case WM_APP + 2: // 数值框提交
    {
        int editId = (int)wParam;
        HWND edit = GetDlgItem(hwnd_, editId);
        int sliderId = (editId >= 200 && editId < 206) ? 100 + (editId - 200)
                     : (editId >= 400 && editId < 427) ? 300 + (editId - 400) : -1;
        if (edit && sliderId > 0)
        {
            wchar_t buf[64];
            GetWindowTextW(edit, buf, 64);
            double parsed = 0;
            double base = sliderId >= 300 && sliderId - 300 < (int)hslSpecs_.size() ? hslSpecs_[sliderId - 300].displayBase : NAN;
            if (ParseDouble(buf, parsed))
            {
                if (!std::isnan(base)) parsed += base;
                double min = 0, max = 0;
                if (sliderId >= 100 && sliderId < 106)
                {
                    static const double mins[6] = { -100, 0, 0, -100, -100, -100 };
                    static const double maxs[6] = { 100, 200, 200, 100, 100, 100 };
                    min = mins[sliderId - 100]; max = maxs[sliderId - 100];
                }
                else if (sliderId - 300 < (int)hslSpecs_.size())
                {
                    min = hslSpecs_[sliderId - 300].min;
                    max = hslSpecs_[sliderId - 300].max;
                }
                parsed = Clamp(parsed, min, max);
                ApplySliderValue(sliderId, parsed);
                UpdateSliderUi(sliderId, parsed, true);
            }
            else
            {
                double cur = 0;
                if (sliderId >= 100 && sliderId < 106)
                    cur = CurrentDisplay().Current.Brightness;
                UpdateSliderUi(sliderId, cur, true);
            }
        }
        return 0;
    }
    case WM_APP + 3:
        return 0;
    case WM_APP + 4:
        SaveBarSave();
        return 0;
    case WM_APP + 5:
        SaveBarCancel();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

// ---------------- 命令与通知 ----------------

void MainWindow::OnCommand(int id, HWND hwndCtl, int code)
{
    switch (id)
    {
    case IDC_DEBUG_LOG: OnOpenDebugLog(); break;
    case IDC_DISPLAY_ENABLE:
        if (code == BN_CLICKED)
        {
            if (displayInit_) break;
            CurrentDisplay().IsEnabled = IsDlgButtonChecked(hwnd_, IDC_DISPLAY_ENABLE) == BST_CHECKED;
            UpdateDisplayEnableStatus();
            ApplyCurrent();
            ScheduleSave();
        }
        break;
    case IDC_ENABLE_FILTER:
        if (code == BN_CLICKED)
        {
            bool on = IsDlgButtonChecked(hwnd_, IDC_ENABLE_FILTER) == BST_CHECKED;
            if (on && data_.Displays.size() > 0)
            {
                bool allOff = true;
                for (auto& d : data_.Displays) if (d.IsEnabled) { allOff = false; break; }
                if (allOff)
                {
                    displayInit_ = true;
                    for (auto& d : data_.Displays) d.IsEnabled = true;
                    CheckDlgButton(hwnd_, IDC_DISPLAY_ENABLE, BST_CHECKED);
                    displayInit_ = false;
                }
            }
            ApplyCurrent();
            ScheduleSave();
        }
        break;
    case IDC_LUT_SWITCH:
        if (code == BN_CLICKED) OnLutSwitchToggled();
        break;
    case IDC_VSYNC_SWITCH:
        if (code == BN_CLICKED)
        {
            if (displayInit_ || uiInit_) break;
            CurrentDisplay().UseVsync = IsDlgButtonChecked(hwnd_, IDC_VSYNC_SWITCH) == BST_CHECKED;
            if (FilterEngine::Instance().Kind() == EngineKind::PixelShader && CurrentDisplay().IsEnabled)
                FilterEngine::Instance().SetVsync(currentDisplayIndex_, CurrentDisplay().UseVsync);
            ScheduleSave();
        }
        break;
    case IDC_PERAPP_SWITCH:
        if (code == BN_CLICKED)
        {
            if (perAppInit_) break;
            data_.PerAppEnabled = IsDlgButtonChecked(hwnd_, IDC_PERAPP_SWITCH) == BST_CHECKED;
            if (data_.PerAppEnabled) StartPerAppWatching();
            else { StopPerAppWatching(); activeBinding_ = -1; }
            UpdatePerAppStatus();
            ApplyCurrent();
            ScheduleSave();
        }
        break;
    case IDC_CAPTURE_SWITCH:
        if (code == BN_CLICKED)
        {
            if (uiInit_) break;
            data_.Captureable = IsDlgButtonChecked(hwnd_, IDC_CAPTURE_SWITCH) == BST_CHECKED;
            TryEnsureUiTopmost();
            ScheduleSave();
        }
        break;
    case IDC_AUTOSTART_SWITCH:
        if (code == BN_CLICKED)
        {
            data_.AutoStart = IsDlgButtonChecked(hwnd_, IDC_AUTOSTART_SWITCH) == BST_CHECKED;
            AutoStart::Set(data_.AutoStart);
            ScheduleSave();
        }
        break;
    case IDC_AUTOTRAY_SWITCH:
        if (code == BN_CLICKED)
        {
            if (uiInit_) break;
            data_.MinimizeToTray = IsDlgButtonChecked(hwnd_, IDC_AUTOTRAY_SWITCH) == BST_CHECKED;
            ScheduleSave();
        }
        break;
    case IDC_DISPLAY_COMBO:
        if (code == CBN_SELCHANGE) SwitchDisplay((int)SendMessageW(hDisplayCombo, CB_GETCURSEL, 0, 0));
        break;
    case IDC_THEME_COMBO:
        if (code == CBN_SELCHANGE)
        {
            if (themeInit_) break;
            int sel = (int)SendMessageW(pgThemeCombo, CB_GETCURSEL, 0, 0);
            data_.Theme = (sel == 1) ? L"mica" : L"default";
            ApplyWindowTheme();
            ScheduleSave();
        }
        break;
    case IDC_PRESET_DEFAULT: ApplyPreset(FilterSettings()); break;
    case IDC_PRESET_EYE:
    {
        FilterSettings p; p.Brightness = -5; p.Contrast = 95; p.Saturation = 95; p.Temperature = 25;
        ApplyPreset(p);
        break;
    }
    case IDC_PRESET_NIGHT:
    {
        FilterSettings p; p.Brightness = -40; p.Contrast = 100; p.Saturation = 90; p.Temperature = 45;
        ApplyPreset(p);
        break;
    }
    case IDC_PRESET_VIVID:
    {
        FilterSettings p; p.Contrast = 110; p.Saturation = 150;
        ApplyPreset(p);
        break;
    }
    case IDC_HSL_RESET: HslReset(); break;
    case IDC_BINDING_ADD: OnAddBinding(); break;
    case IDC_BINDING_PICK: OnPickForeground(); break;
    case IDC_BINDING_EDIT: OnEditBinding(); break;
    case IDC_BINDING_DELETE: OnDeleteBinding(); break;
    case IDC_PROFILE_NEW: OnNewProfile(); break;
    case IDC_PROFILE_RENAME: OnRenameProfile(); break;
    case IDC_PROFILE_DELETE: OnDeleteProfile(); break;
    case IDC_PROFILE_IMPORT: OnImportProfile(); break;
    case IDC_PROFILE_EXPORT: OnExportProfile(); break;
    case IDC_PROFILE_UP: OnMoveProfile(-1); break;
    case IDC_PROFILE_DOWN: OnMoveProfile(1); break;
    case IDC_HOTKEY_SET: OnCaptureHotkey(); break;
    case IDC_HOTKEY_CLEAR: OnClearHotkey(); break;
    case IDC_HOTKEY_SET_GLOBAL: OnCaptureGlobalHotkey(); break;
    case IDC_HOTKEY_CLEAR_GLOBAL: OnClearGlobalHotkey(); break;
    case IDC_SAVEBAR_SAVE: SaveBarSave(); break;
    case IDC_SAVEBAR_CANCEL: SaveBarCancel(); break;
    }
}

void MainWindow::OnHScroll(int sliderId, int pos)
{
    ApplySliderValue(sliderId, pos);
}

void MainWindow::OnNotify(LPNMHDR nm)
{
    if (nm->hwndFrom == hNav && nm->code == LVN_ITEMCHANGED)
    {
        auto* n = (NMLISTVIEW*)nm;
        if ((n->uNewState & LVIS_SELECTED) && !(n->uOldState & LVIS_SELECTED))
            ShowPage(n->iItem);
        return;
    }
    if (nm->hwndFrom == pgHslTab && nm->code == TCN_SELCHANGE)
    {
        hslPage_ = TabCtrl_GetCurSel(pgHslTab);
        RebuildHslPage(hslPage_);
        return;
    }
    if (nm->hwndFrom == pgProfileList && nm->code == LVN_ITEMCHANGED)
    {
        auto* n = (NMLISTVIEW*)nm;
        if ((n->uChanged & LVIF_STATE) && (n->uNewState & LVIS_STATEIMAGEMASK) != (n->uOldState & LVIS_STATEIMAGEMASK))
        {
            if (profileToggleInit_) return;
            bool on = ListView_GetCheckState(pgProfileList, n->iItem) != FALSE;
            if (on) ActivateProfile(n->iItem);
            else DeactivateProfile();
        }
        return;
    }
}

void MainWindow::OnTimer(UINT_PTR id)
{
    switch (id)
    {
    case TimerApply: KillTimer(hwnd_, TimerApply); ApplyCurrent(); break;
    case TimerSave: KillTimer(hwnd_, TimerSave); SaveState(); break;
    case TimerHint:
        KillTimer(hwnd_, TimerHint);
        if (hGlobalHint) SetWindowTextW(hGlobalHint, L"");
        if (pgHotkeyHint) SetWindowTextW(pgHotkeyHint, L"");
        break;
    }
}

// ---------------- 应用逻辑 ----------------

DisplayState& MainWindow::CurrentDisplay()
{
    int idx = currentDisplayIndex_;
    if (idx < 0 || idx >= (int)data_.Displays.size()) idx = 0;
    return data_.Displays[idx];
}

Profile* MainWindow::ActiveProfile()
{
    int idx = CurrentDisplay().ActiveProfileIndex;
    if (idx >= 0 && idx < (int)data_.Profiles.size()) return &data_.Profiles[idx];
    return nullptr;
}

int MainWindow::ActiveProfileIndex() { return CurrentDisplay().ActiveProfileIndex; }

void MainWindow::InitializeDisplays()
{
    monitors_ = Monitors::Enumerate();
    if (monitors_.empty())
        monitors_.push_back(DisplayMonitor{ L"", 0, 0, 1920, 1080, true });
    bool wasEmpty = data_.Displays.empty();
    while ((int)data_.Displays.size() < (int)monitors_.size())
        data_.Displays.push_back(DisplayState{});
    for (int i = 0; i < (int)data_.Displays.size() && i < (int)monitors_.size(); i++)
        data_.Displays[i].Index = i;
    if (wasEmpty && !data_.Displays.empty())
    {
        auto& primary = data_.Displays[0];
        if (!data_.Current.IsDefaultBasic()) primary.Current = data_.Current;
        primary.ActiveProfileIndex = data_.ActiveProfileIndex;
        primary.IsEnabled = data_.IsEnabled;
    }
    currentDisplayIndex_ = 0;
    for (int i = 0; i < (int)monitors_.size(); i++)
        if (monitors_[i].IsPrimary) { currentDisplayIndex_ = i; break; }
}

void MainWindow::SwitchDisplay(int index)
{
    if (index < 0 || index >= (int)monitors_.size()) return;
    currentDisplayIndex_ = index;
    auto& d = data_.Displays[index];
    profileToggleInit_ = true;
    for (auto& p : data_.Profiles) p.IsActive = false;
    if (d.ActiveProfileIndex >= 0 && d.ActiveProfileIndex < (int)data_.Profiles.size())
        data_.Profiles[d.ActiveProfileIndex].IsActive = true;
    profileToggleInit_ = false;

    savedSnapshot_ = d.Current.Clone();
    LoadSettingsIntoUi(d.Current);

    displayInit_ = true;
    CheckDlgButton(hwnd_, IDC_DISPLAY_ENABLE, d.IsEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd_, IDC_VSYNC_SWITCH, d.UseVsync ? BST_CHECKED : BST_UNCHECKED);
    displayInit_ = false;
    UpdateDisplayEnableStatus();

    if (data_.UseDxgi && d.IsEnabled)
        FilterEngine::Instance().SetVsync(index, d.UseVsync);
    Profile* ap = (d.ActiveProfileIndex >= 0 && d.ActiveProfileIndex < (int)data_.Profiles.size())
                      ? &data_.Profiles[d.ActiveProfileIndex] : nullptr;
    if (ap) ApplyProfileLut(d.ActiveProfileIndex);
    else ApplyLutModeNoDialog(data_.UseDxgi);
    ScheduleSave();
}

void MainWindow::UpdateDisplayEnableStatus()
{
    if (hDisplayEnable)
        SetWindowTextW(hDisplayEnable, IsDlgButtonChecked(hwnd_, IDC_DISPLAY_ENABLE) == BST_CHECKED
                                          ? L"启用（已开）" : L"启用（已关）");
}

void MainWindow::ScheduleApply()
{
    MarkFilterChanged();
    KillTimer(hwnd_, TimerApply);
    SetTimer(hwnd_, TimerApply, 80, nullptr);
    ScheduleSave();
}

void MainWindow::ScheduleSave()
{
    KillTimer(hwnd_, TimerSave);
    SetTimer(hwnd_, TimerSave, 400, nullptr);
}

static FilterSettings NeutralizeHsl(const FilterSettings& s)
{
    FilterSettings c = s.Clone();
    c.Hue = 0; c.HslSaturation = 100; c.Lightness = 0;
    for (auto& ch : c.HslChannels) { ch.Hue = 0; ch.Saturation = 100; ch.Lightness = 0; }
    return c;
}

void MainWindow::ApplyCurrent()
{
    bool perAppRequiresHit = data_.PerAppEnabled && !data_.AppBindings.empty();
    bool globalOn = IsDlgButtonChecked(hwnd_, IDC_ENABLE_FILTER) == BST_CHECKED && (!perAppRequiresHit || activeBinding_ != -1);
    Profile* perAppProfile = nullptr;
    if (perAppRequiresHit && activeBinding_ >= 0 &&
        data_.AppBindings[activeBinding_].ProfileIndex >= 0 &&
        data_.AppBindings[activeBinding_].ProfileIndex < (int)data_.Profiles.size())
    {
        perAppProfile = &data_.Profiles[data_.AppBindings[activeBinding_].ProfileIndex];
        SyncAppliedProfileSwitch(data_.AppBindings[activeBinding_].ProfileIndex);
    }
    else if (perAppRequiresHit && activeBinding_ < 0)
    {
        SyncAppliedProfileSwitch(-1);
    }

    bool anyOk = false, anyEnabled = false;
    for (int i = 0; i < (int)monitors_.size(); i++)
    {
        auto& dstate = data_.Displays[i];
        bool displayOn = globalOn && dstate.IsEnabled;
        if (!displayOn) { FilterEngine::Instance().ResetDisplay(i); continue; }
        anyEnabled = true;
        FilterSettings settings;
        if (perAppProfile) settings = perAppProfile->Settings;
        else settings = data_.UseDxgi ? dstate.Current : NeutralizeHsl(dstate.Current);
        if (data_.UseDxgi && dstate.IsEnabled)
            FilterEngine::Instance().SetVsync(i, dstate.UseVsync);
        if (FilterEngine::Instance().Apply(i, monitors_[i], settings))
            anyOk = true;
        else if (hStatus)
            SetWindowTextW(hStatus, (L"应用滤镜失败：" + FilterEngine::Instance().LastError()).c_str());
    }
    if (!anyEnabled) { FilterEngine::Instance().Reset(); UpdateEngineStatus(); }
    else if (anyOk) { UpdateEngineStatus(); TryEnsureUiTopmost(); }
    else if (globalOn) UpdateEngineStatus();
}

void MainWindow::UpdateEngineStatus()
{
    if (!hStatus) return;
    switch (FilterEngine::Instance().Kind())
    {
    case EngineKind::PixelShader:
        SetWindowTextW(hStatus, L"滤镜引擎：LUT 逐像素引擎（3D LUT，支持 HSL 调色）");
        break;
    case EngineKind::FullScreenColorEffect:
        SetWindowTextW(hStatus, L"滤镜引擎：全屏颜色效果（放大镜 API，HSL 不可用）");
        break;
    case EngineKind::GammaRamp:
        SetWindowTextW(hStatus, L"滤镜引擎：显卡伽马曲线（鲜艳度不可用）");
        break;
    default:
        SetWindowTextW(hStatus, (L"滤镜引擎不可用：" + FilterEngine::Instance().LastError()).c_str());
        break;
    }
    UpdateHslHint();
}

void MainWindow::UpdateHslHint()
{
    if (!pgHslHint) return;
    SetWindowTextW(pgHslHint,
        FilterEngine::Instance().Kind() == EngineKind::PixelShader
            ? L"已启用 LUT 引擎：8 个色系可分别精确调整、互不干扰（64³ 3D LUT 逐像素着色器）。"
            : L"提示：关闭 LUT 引擎后使用放大镜引擎，暂不支持分色系 HSL。");
}

void MainWindow::OnLutSwitchToggled()
{
    if (lutInit_) return;
    bool on = IsDlgButtonChecked(hwnd_, IDC_LUT_SWITCH) == BST_CHECKED;
    if (on)
    {
        lutInit_ = true;
        CheckDlgButton(hwnd_, IDC_LUT_SWITCH, BST_UNCHECKED);
        lutInit_ = false;
        if (ConfirmDialog(hwnd_, L"启用 LUT 引擎",
                          L"启用 LUT 引擎后 HSL 功能可用，但会造成性能损失，是否启用？", L"启用"))
        {
            lutInit_ = true;
            CheckDlgButton(hwnd_, IDC_LUT_SWITCH, BST_CHECKED);
            lutInit_ = false;
            ApplyLutMode(true);
        }
    }
    else
    {
        ApplyLutMode(false);
    }
}

void MainWindow::ApplyLutMode(bool useLut)
{
    data_.UseDxgi = useLut;
    if (Profile* ap = ActiveProfile()) ap->UseDxgi = useLut;
    ApplyLutModeNoDialog(useLut);
    ApplyCurrent();
    ScheduleSave();
}

void MainWindow::ApplyLutModeNoDialog(bool useLut)
{
    FilterEngine::Instance().SetUseDxgi(useLut);
    FilterEngine::Instance().Initialize();
    if (useLut && CurrentDisplay().IsEnabled)
        FilterEngine::Instance().SetVsync(currentDisplayIndex_, CurrentDisplay().UseVsync);
    UpdateEngineStatus();
}

void MainWindow::ApplyProfileLut(int profileIndex)
{
    if (profileIndex < 0 || profileIndex >= (int)data_.Profiles.size()) return;
    bool useLut = data_.Profiles[profileIndex].UseDxgi;
    data_.UseDxgi = useLut;
    lutInit_ = true;
    CheckDlgButton(hwnd_, IDC_LUT_SWITCH, useLut ? BST_CHECKED : BST_UNCHECKED);
    lutInit_ = false;
    FilterEngine::Instance().SetUseDxgi(useLut);
    FilterEngine::Instance().Initialize();
    UpdateEngineStatus();
}

// ---------------- HSL 值 ----------------

void MainWindow::SetHslValue(FilterSettings& s, int channel, int field, double value)
{
    if (channel == 0)
    {
        if (field == 0) s.Hue = value;
        else if (field == 1) s.HslSaturation = value;
        else s.Lightness = value;
        return;
    }
    const wchar_t* name = HslChannelNames::ColorNames[channel - 1];
    HslChannel* ch = s.FindChannel(name);
    if (!ch)
    {
        s.HslChannels.push_back(HslChannel{ name, 0, 100, 0 });
        ch = &s.HslChannels.back();
    }
    if (field == 0) ch->Hue = value;
    else if (field == 1) ch->Saturation = value;
    else ch->Lightness = value;
}

double MainWindow::ChannelValue(const FilterSettings& s, int channel, int field) const
{
    if (channel == 0)
    {
        if (field == 0) return s.Hue;
        if (field == 1) return s.HslSaturation;
        return s.Lightness;
    }
    const HslChannel* ch = s.FindChannel(HslChannelNames::ColorNames[channel - 1]);
    if (!ch) return field == 1 ? 100.0 : 0.0;
    if (field == 0) return ch->Hue;
    if (field == 1) return ch->Saturation;
    return ch->Lightness;
}

void MainWindow::LoadSettingsIntoUi(const FilterSettings& s)
{
    static const double mins[6] = { -100, 0, 0, -100, -100, -100 };
    static const double maxs[6] = { 100, 200, 200, 100, 100, 100 };
    double vals[6] = { s.Brightness, s.Contrast, s.Saturation, s.Highlights, s.Shadows, s.Temperature };
    for (int i = 0; i < 6; i++)
        UpdateSliderUi(100 + i, Clamp(vals[i], mins[i], maxs[i]), true);
    LoadHslSliders(const_cast<FilterSettings&>(s));
}

void MainWindow::LoadHslSliders(FilterSettings& s)
{
    if (hslSpecs_.size() < 27) return;
    for (int i = 0; i < 27; i++)
    {
        double v = ChannelValue(s, hslSpecs_[i].channel, hslSpecs_[i].field);
        v = Clamp(v, hslSpecs_[i].min, hslSpecs_[i].max);
        SetHslValue(s, hslSpecs_[i].channel, hslSpecs_[i].field, v);
        UpdateSliderUi(300 + i, v, true);
    }
}

void MainWindow::HslReset()
{
    auto& s = CurrentDisplay().Current;
    s.Hue = 0; s.HslSaturation = 100; s.Lightness = 0;
    for (auto& ch : s.HslChannels) { ch.Hue = 0; ch.Saturation = 100; ch.Lightness = 0; }
    LoadHslSliders(s);
    ScheduleApply();
    ScheduleSave();
}

// ---------------- 预设与保存条 ----------------

void MainWindow::MarkFilterChanged()
{
    if (pendingEdit_) return;
    pendingEdit_ = true;
    ShowSaveBar();
}

static const wchar_t* kSaveBarClass = L"HSFSaveBar";

LRESULT CALLBACK MainWindow::SaveBarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HBRUSH s_brush = nullptr;
    switch (msg)
    {
    case WM_NCCREATE:
        s_brush = CreateSolidBrush(RGB(0xF0, 0xF0, 0xF0));
        return TRUE;
    case WM_ERASEBKGND:
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, s_brush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(0x20, 0x20, 0x20));
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)s_brush;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_SAVEBAR_SAVE) PostMessageW(GetParent(hwnd), WM_APP + 4, 0, 0);
        else if (id == IDC_SAVEBAR_CANCEL) PostMessageW(GetParent(hwnd), WM_APP + 5, 0, 0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MainWindow::ShowSaveBar()
{
    if (!hSaveBar)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SaveBarWndProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = kSaveBarClass;
        RegisterClassExW(&wc);
        hSaveBar = CreateWindowExW(WS_EX_TOPMOST, kSaveBarClass, L"",
                                   WS_CHILD | WS_POPUP | WS_VISIBLE,
                                   0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
        if (hSaveBar)
        {
            CreateWindowExW(0, L"STATIC", L"配置发生改变，是否保存？", WS_CHILD | WS_VISIBLE | SS_LEFT,
                            Dip(16), Dip(16), Dip(180), Dip(24), hSaveBar, nullptr, hInst_, nullptr);
            CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                            Dip(230), Dip(12), Dip(80), Dip(30), hSaveBar, (HMENU)IDC_SAVEBAR_SAVE, hInst_, nullptr);
            CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            Dip(318), Dip(12), Dip(80), Dip(30), hSaveBar, (HMENU)IDC_SAVEBAR_CANCEL, hInst_, nullptr);
            auto setfont = [](HWND c, int px) {
                HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Microsoft YaHei UI");
                SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
            };
            setfont(GetDlgItem(hSaveBar, 0), (int)(13 * scale_));
            setfont(GetDlgItem(hSaveBar, IDC_SAVEBAR_SAVE), (int)(13 * scale_));
            setfont(GetDlgItem(hSaveBar, IDC_SAVEBAR_CANCEL), (int)(13 * scale_));
        }
    }
    if (!hSaveBar) return;
    PositionSaveBar();
    ShowWindow(hSaveBar, SW_SHOW);
    SetWindowPos(hSaveBar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::PositionSaveBar()
{
    if (!hSaveBar) return;
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    int barW = Dip(420), barH = Dip(56);
    int barX = (cr.right - barW) / 2;
    int barY = cr.bottom - barH - Dip(20);
    MoveWindow(hSaveBar, barX, barY, barW, barH, TRUE);
}

void MainWindow::HideSaveBar()
{
    if (hSaveBar) ShowWindow(hSaveBar, SW_HIDE);
    pendingEdit_ = false;
}

void MainWindow::SaveBarSave()
{
    if (Profile* ap = ActiveProfile())
        ap->Settings = CurrentDisplay().Current.Clone();
    savedSnapshot_ = CurrentDisplay().Current.Clone();
    pendingEdit_ = false;
    HideSaveBar();
    SaveState();
}

void MainWindow::SaveBarCancel()
{
    CurrentDisplay().Current = savedSnapshot_.Clone();
    LoadSettingsIntoUi(CurrentDisplay().Current);
    ApplyCurrent();
    pendingEdit_ = false;
    HideSaveBar();
    SaveState();
}

void MainWindow::ApplyPreset(const FilterSettings& preset)
{
    MarkFilterChanged();
    CurrentDisplay().Current = preset.Clone();
    LoadSettingsIntoUi(CurrentDisplay().Current);
    ApplyCurrent();
    ScheduleSave();
}

// ---------------- 配置管理 ----------------

void MainWindow::RefreshProfileList()
{
    if (!pgProfileList) return;
    ListView_DeleteAllItems(pgProfileList);
    profileToggleInit_ = true;
    for (int i = 0; i < (int)data_.Profiles.size(); i++)
    {
        auto& p = data_.Profiles[i];
        std::wstring name = p.Name, api = p.ApiText(), hotkey = p.HasHotkey() ? p.HotkeyDisplay : L"";
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)name.c_str();
        int row = ListView_InsertItem(pgProfileList, &it);
        ListView_SetItemText(pgProfileList, row, 1, (LPWSTR)api.c_str());
        ListView_SetItemText(pgProfileList, row, 2, (LPWSTR)hotkey.c_str());
        ListView_SetCheckState(pgProfileList, row, p.IsActive ? TRUE : FALSE);
    }
    profileToggleInit_ = false;
}

void MainWindow::OnNewProfile()
{
    std::wstring name;
    if (!PromptTextDialog(hwnd_, L"新建配置", L"配置名称",
                          Format(L"配置 %d", (int)data_.Profiles.size() + 1), name))
        return;
    Profile p;
    p.Name = name;
    p.Settings = CurrentDisplay().Current.Clone();
    p.UseDxgi = data_.UseDxgi;
    data_.Profiles.push_back(std::move(p));
    RefreshProfileList();
    SaveState();
}

void MainWindow::OnRenameProfile()
{
    int idx = ListView_GetNextItem(pgProfileList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    auto& p = data_.Profiles[idx];
    std::wstring name;
    if (!PromptTextDialog(hwnd_, L"重命名配置", L"配置名称", p.Name, name)) return;
    p.Name = name;
    RefreshProfileList();
    RefreshBindingList();
    SaveState();
}

void MainWindow::OnDeleteProfile()
{
    int idx = ListView_GetNextItem(pgProfileList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    auto it = profileHotkeyIds_.find(idx);
    if (it != profileHotkeyIds_.end()) { hotkeys_->Unregister(it->second); profileHotkeyIds_.erase(it); }
    bool wasActive = (ActiveProfileIndex() == idx);
    data_.Profiles.erase(data_.Profiles.begin() + idx);
    for (auto& b : data_.AppBindings)
    {
        if (b.ProfileIndex == idx) b.ProfileIndex = -1;
        else if (b.ProfileIndex > idx) b.ProfileIndex--;
    }
    if (wasActive)
    {
        profileToggleInit_ = true;
        for (auto& p : data_.Profiles) p.IsActive = false;
        profileToggleInit_ = false;
        for (auto& d : data_.Displays) d.ActiveProfileIndex = -1;
        CurrentDisplay().Current = FilterSettings();
        LoadSettingsIntoUi(CurrentDisplay().Current);
        ApplyCurrent();
    }
    else
    {
        for (auto& d : data_.Displays)
            if (d.ActiveProfileIndex > idx) d.ActiveProfileIndex--;
    }
    std::map<int, int> remapped;
    for (auto& kv : profileHotkeyIds_) if (kv.first > idx) remapped[kv.first - 1] = kv.second;
    profileHotkeyIds_ = std::move(remapped);
    RefreshProfileList();
    RefreshBindingList();
    SaveState();
}

void MainWindow::OnMoveProfile(int delta)
{
    int idx = ListView_GetNextItem(pgProfileList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    int target = idx + delta;
    if (target < 0 || target >= (int)data_.Profiles.size()) return;
    auto p = data_.Profiles[idx];
    data_.Profiles.erase(data_.Profiles.begin() + idx);
    data_.Profiles.insert(data_.Profiles.begin() + target, p);
    int activeIdx = -1;
    for (int i = 0; i < (int)data_.Profiles.size(); i++)
        if (data_.Profiles[i].IsActive) { activeIdx = i; break; }
    for (auto& d : data_.Displays) d.ActiveProfileIndex = activeIdx;
    RefreshProfileList();
    RefreshBindingList();
    ListView_SetItemState(pgProfileList, target, LVIS_SELECTED, LVIS_SELECTED);
    SaveState();
}

void MainWindow::OnImportProfile()
{
    std::wstring fileName, jsonText;
    if (!ImportProfileDialog(hwnd_, fileName, jsonText)) return;
    JsonValue root;
    if (!JsonValue::Parse(jsonText, root) || !root.IsObject())
    {
        if (hStatus) SetWindowTextW(hStatus, L"导入配置失败：文件内容为空或格式不正确");
        return;
    }
    FilterSettings settings;
    settings.FromJson(root);
    std::wstring name = fileName;
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);
    Profile p;
    p.Name = name;
    p.Settings = settings;
    p.UseDxgi = data_.UseDxgi;
    data_.Profiles.push_back(std::move(p));
    RefreshProfileList();
    SaveState();
    if (hStatus) SetWindowTextW(hStatus, (L"已导入配置「" + name + L"」").c_str());
}

void MainWindow::OnExportProfile()
{
    int idx = ListView_GetNextItem(pgProfileList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    auto& p = data_.Profiles[idx];
    std::wstring json = p.Settings.ToJson().Serialize(true);
    std::wstring fileName;
    if (!ExportProfileDialog(hwnd_, p.Name, json)) return;
    if (hStatus) SetWindowTextW(hStatus, (L"已导出配置「" + p.Name + L"」").c_str());
}

void MainWindow::SetActiveProfileAndApply(int index)
{
    if (index < 0 || index >= (int)data_.Profiles.size()) return;
    profileToggleInit_ = true;
    for (int i = 0; i < (int)data_.Profiles.size(); i++)
        data_.Profiles[i].IsActive = (i == index);
    profileToggleInit_ = false;
    CurrentDisplay().ActiveProfileIndex = index;
    CurrentDisplay().Current = data_.Profiles[index].Settings.Clone();
    savedSnapshot_ = CurrentDisplay().Current.Clone();
    pendingEdit_ = false;
    HideSaveBar();
    LoadSettingsIntoUi(CurrentDisplay().Current);
    ApplyProfileLut(index);
    ApplyCurrent();
    SaveState();
    RefreshProfileList();
}

void MainWindow::DeactivateProfile()
{
    profileToggleInit_ = true;
    for (auto& p : data_.Profiles) p.IsActive = false;
    profileToggleInit_ = false;
    CurrentDisplay().ActiveProfileIndex = -1;
    CurrentDisplay().Current = FilterSettings();
    savedSnapshot_ = CurrentDisplay().Current.Clone();
    pendingEdit_ = false;
    HideSaveBar();
    LoadSettingsIntoUi(CurrentDisplay().Current);
    ApplyCurrent();
    SaveState();
    RefreshProfileList();
}

void MainWindow::SyncAppliedProfileSwitch(int profileIndex)
{
    profileToggleInit_ = true;
    for (int i = 0; i < (int)data_.Profiles.size(); i++)
        data_.Profiles[i].IsActive = (i == profileIndex);
    profileToggleInit_ = false;
    for (auto& d : data_.Displays) d.ActiveProfileIndex = profileIndex;
    RefreshProfileList();
}

// ---------------- 按应用切换 ----------------

void MainWindow::StartPerAppWatching()
{
    watcher_->SetTargets(data_.AppBindings);
    watcher_->Start(500);
}

void MainWindow::StopPerAppWatching() { watcher_->Stop(); }

void MainWindow::ForegroundMatchChanged(int hitBindingIndex, const std::wstring& proc, const std::wstring& title)
{
    activeBinding_ = hitBindingIndex;
    Log::WriteFmt(L"PerApp", L"%s (前台=%s)", hitBindingIndex >= 0 ? L"命中" : L"未命中", proc.c_str());
    UpdatePerAppStatus();
    ApplyCurrent();
    ScheduleSave();
}

void MainWindow::UpdatePerAppStatus()
{
    if (!pgPerAppStatus) return;
    if (!data_.PerAppEnabled) { SetWindowTextW(pgPerAppStatus, L""); return; }
    if (data_.AppBindings.empty())
    {
        SetWindowTextW(pgPerAppStatus, L"○ 尚未添加要检测的应用（请点击「添加应用…」）");
        return;
    }
    if (activeBinding_ < 0)
    {
        SetWindowTextW(pgPerAppStatus, (Format(L"○ 列表内无进程在前台，滤镜已自动关闭（共 %d 个检测目标）",
                                               (int)data_.AppBindings.size())).c_str());
        return;
    }
    std::wstring cfg;
    int pi = data_.AppBindings[activeBinding_].ProfileIndex;
    if (pi >= 0 && pi < (int)data_.Profiles.size())
        cfg = L"已自动应用配置「" + data_.Profiles[pi].Name + L"」";
    else
        cfg = L"按当前设置应用";
    SetWindowTextW(pgPerAppStatus, (L"● " + data_.AppBindings[activeBinding_].ProcessName + L" 在前台，滤镜已启用（" + cfg + L"）").c_str());
}

void MainWindow::RefreshBindingList()
{
    if (!pgBindingList) return;
    ListView_DeleteAllItems(pgBindingList);
    for (int i = 0; i < (int)data_.AppBindings.size(); i++)
    {
        auto& b = data_.AppBindings[i];
        std::wstring dn = b.DisplayName();
        int pi = b.ProfileIndex;
        std::wstring pn = (pi >= 0 && pi < (int)data_.Profiles.size()) ? data_.Profiles[pi].Name : L"";
        std::wstring pd = b.ProfileDisplay(pn);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)dn.c_str();
        int row = ListView_InsertItem(pgBindingList, &it);
        ListView_SetItemText(pgBindingList, row, 1, (LPWSTR)pd.c_str());
    }
}

void MainWindow::OnAddBinding()
{
    AppBinding binding;
    std::vector<std::wstring> names;
    for (auto& p : data_.Profiles) names.push_back(p.Name);
    if (!EditBindingDialog(hwnd_, L"添加检测应用", names, binding)) return;
    data_.AppBindings.push_back(binding);
    RefreshBindingList();
    if (data_.PerAppEnabled) StartPerAppWatching();
    SaveState();
    UpdatePerAppStatus();
    ApplyCurrent();
}

void MainWindow::OnEditBinding()
{
    int idx = ListView_GetNextItem(pgBindingList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.AppBindings.size()) return;
    AppBinding copy = data_.AppBindings[idx];
    std::vector<std::wstring> names;
    for (auto& p : data_.Profiles) names.push_back(p.Name);
    if (!EditBindingDialog(hwnd_, L"编辑检测应用", names, copy)) return;
    data_.AppBindings[idx] = copy;
    RefreshBindingList();
    if (data_.PerAppEnabled) StartPerAppWatching();
    SaveState();
    UpdatePerAppStatus();
    ApplyCurrent();
}

void MainWindow::OnDeleteBinding()
{
    int idx = ListView_GetNextItem(pgBindingList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.AppBindings.size()) return;
    if (activeBinding_ == idx) activeBinding_ = -1;
    data_.AppBindings.erase(data_.AppBindings.begin() + idx);
    if (activeBinding_ > idx) activeBinding_--;
    RefreshBindingList();
    if (data_.PerAppEnabled) StartPerAppWatching();
    SaveState();
    UpdatePerAppStatus();
    ApplyCurrent();
}

void MainWindow::OnPickForeground()
{
    if (!ForegroundCountdownDialog(hwnd_)) return;
    HWND fg = ForegroundAppWatcher::GetForegroundWindowForPicker();
    std::wstring proc, title;
    if (!fg || !ForegroundAppWatcher::GetForegroundInfo(fg, proc, title) || proc.empty())
    {
        SetWindowTextW(pgPerAppStatus, L"未能读取当前前台应用");
        return;
    }
    AppBinding binding;
    binding.ProcessName = proc;
    binding.WindowTitle = title;
    std::vector<std::wstring> names;
    for (auto& p : data_.Profiles) names.push_back(p.Name);
    if (!EditBindingDialog(hwnd_, L"添加检测应用", names, binding)) return;
    data_.AppBindings.push_back(binding);
    RefreshBindingList();
    if (data_.PerAppEnabled) StartPerAppWatching();
    SaveState();
    UpdatePerAppStatus();
    ApplyCurrent();
}

// ---------------- 快捷键 ----------------

void MainWindow::OnCaptureHotkey()
{
    int idx = ListView_GetNextItem(pgProfileList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.Profiles.size())
    {
        SetWindowTextW(pgHotkeyHint, L"请先选择一个配置");
        return;
    }
    capturingFor_ = idx;
    capturingGlobal_ = false;
    SetWindowTextW(pgHotkeyHint, L"请按下要绑定的按键（可带也可不带 Ctrl/Alt/Shift/Win），Esc 取消…");
    SetFocus(hwnd_);
}

void MainWindow::OnCaptureGlobalHotkey()
{
    capturingFor_ = -1;
    capturingGlobal_ = true;
    SetWindowTextW(hGlobalHint, L"请按下全局开关按键（可带也可不带修饰键），Esc 取消…");
    SetFocus(hwnd_);
}

void MainWindow::OnClearHotkey()
{
    int idx = ListView_GetNextItem(pgProfileList, -1, LVNI_SELECTED);
    if (idx < 0 || idx >= (int)data_.Profiles.size()) return;
    auto& p = data_.Profiles[idx];
    p.HotkeyModifiers = 0; p.HotkeyKey = 0; p.HotkeyDisplay = L"";
    RegisterProfileHotkey(idx);
    SaveState();
    RefreshProfileList();
}

void MainWindow::OnClearGlobalHotkey()
{
    data_.GlobalModifiers = 0; data_.GlobalKey = 0; data_.GlobalDisplay = L"";
    RegisterGlobalToggle();
    UpdateGlobalHotkeyBadge();
    SaveState();
}

void MainWindow::UpdateGlobalHotkeyBadge()
{
    if (!hGlobalHint) return;
    SetWindowTextW(hGlobalHint, data_.GlobalDisplay.empty() ? L"" : (L"全局开关快捷键：" + data_.GlobalDisplay).c_str());
}

void MainWindow::HandleHotkeyCapture(int vk)
{
    if (vk == VK_ESCAPE)
    {
        capturingFor_ = -1; capturingGlobal_ = false;
        SetWindowTextW(pgHotkeyHint, L"已取消");
        SetWindowTextW(hGlobalHint, L"已取消");
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
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
        mods |= kModWin;
    std::wstring display = HotkeyText::Format(mods, vk);
    if (capturingGlobal_)
    {
        data_.GlobalModifiers = mods; data_.GlobalKey = vk; data_.GlobalDisplay = display;
        RegisterGlobalToggle();
        capturingGlobal_ = false;
        SetWindowTextW(hGlobalHint, (L"全局开关快捷键已设置为 " + display).c_str());
        UpdateGlobalHotkeyBadge();
    }
    else if (capturingFor_ >= 0 && capturingFor_ < (int)data_.Profiles.size())
    {
        auto& p = data_.Profiles[capturingFor_];
        p.HotkeyModifiers = mods; p.HotkeyKey = vk; p.HotkeyDisplay = display;
        RegisterProfileHotkey(capturingFor_);
        SetWindowTextW(pgHotkeyHint, (L"已为「" + p.Name + L"」设置快捷键 " + display).c_str());
        capturingFor_ = -1;
    }
    SaveState();
    RefreshProfileList();
}

void MainWindow::RegisterProfileHotkey(int index)
{
    if (index < 0 || index >= (int)data_.Profiles.size()) return;
    auto& p = data_.Profiles[index];
    auto it = profileHotkeyIds_.find(index);
    if (it != profileHotkeyIds_.end()) { hotkeys_->Unregister(it->second); profileHotkeyIds_.erase(it); }
    if (!p.HasHotkey()) return;
    int id = 0;
    if (hotkeys_->Register(p.HotkeyModifiers, p.HotkeyKey,
                           [this, index]() { InvokeUi([this, index]() { ToggleProfileHotkey(index); }); }, id))
        profileHotkeyIds_[index] = id;
}

void MainWindow::RegisterGlobalToggle()
{
    if (globalHotkeyId_ != 0) { hotkeys_->Unregister(globalHotkeyId_); globalHotkeyId_ = 0; }
    if (data_.GlobalKey == 0) return;
    int id = 0;
    if (hotkeys_->Register(data_.GlobalModifiers, data_.GlobalKey,
                           [this]() { InvokeUi([this]() { ToggleGlobal(); }); }, id))
        globalHotkeyId_ = id;
}

void MainWindow::ToggleGlobal()
{
    CheckDlgButton(hwnd_, IDC_ENABLE_FILTER,
                   IsDlgButtonChecked(hwnd_, IDC_ENABLE_FILTER) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
    ApplyCurrent();
    ScheduleSave();
}

void MainWindow::ToggleProfileHotkey(int index)
{
    if (ActiveProfileIndex() == index) DeactivateProfile();
    else SetActiveProfileAndApply(index);
}

// ---------------- 顶层与主题 ----------------

void MainWindow::TryEnsureUiTopmost()
{
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex |= kWsExTopmost;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowDisplayAffinity(hwnd_, data_.Captureable ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
    FilterEngine::Instance().SetOverlayCapturable(data_.Captureable);
}

void MainWindow::ApplyWindowTheme()
{
    // 浅色主题：关闭沉浸式深色标题栏；Mica 背景仅在 Win11 可用
    BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    int backdrop = data_.Theme == L"mica" ? 2 : 1;
    DwmSetWindowAttribute(hwnd_, 38, &backdrop, sizeof(backdrop));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::OnOpenDebugLog()
{
    std::wstring logPath = Log::FilePath();
    if (!PathFileExistsW(logPath.c_str())) Log::Write(L"UI", L"首次打开调试日志");
    ShellExecuteW(nullptr, L"open", logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------- 保存与关闭 ----------------

void MainWindow::SaveState()
{
    if (!hwnd_) return;
    CurrentDisplay().Current = CurrentDisplay().Current.Clone();
    CurrentDisplay().ActiveProfileIndex = ActiveProfileIndex();
    CurrentDisplay().IsEnabled = IsDlgButtonChecked(hwnd_, IDC_DISPLAY_ENABLE) == BST_CHECKED;
    CurrentDisplay().UseVsync = pgVsyncSwitch ? IsDlgButtonChecked(hwnd_, IDC_VSYNC_SWITCH) == BST_CHECKED : CurrentDisplay().UseVsync;

    data_.IsEnabled = IsDlgButtonChecked(hwnd_, IDC_ENABLE_FILTER) == BST_CHECKED;
    data_.ActiveProfileIndex = data_.Displays.empty() ? -1 : data_.Displays[0].ActiveProfileIndex;
    data_.Current = data_.Displays.empty() ? FilterSettings() : data_.Displays[0].Current.Clone();
    if (!data_.Displays.empty())
        data_.Displays[0].UseVsync = CurrentDisplay().UseVsync;

    data_.PerAppEnabled = pgPerAppSwitch ? IsDlgButtonChecked(hwnd_, IDC_PERAPP_SWITCH) == BST_CHECKED : data_.PerAppEnabled;
    data_.Captureable = pgCaptureSwitch ? IsDlgButtonChecked(hwnd_, IDC_CAPTURE_SWITCH) == BST_CHECKED : data_.Captureable;
    data_.UseDxgi = pgLutSwitch ? IsDlgButtonChecked(hwnd_, IDC_LUT_SWITCH) == BST_CHECKED : data_.UseDxgi;
    data_.AutoStart = pgAutoStartSwitch ? IsDlgButtonChecked(hwnd_, IDC_AUTOSTART_SWITCH) == BST_CHECKED : data_.AutoStart;
    data_.MinimizeToTray = pgAutoTraySwitch ? IsDlgButtonChecked(hwnd_, IDC_AUTOTRAY_SWITCH) == BST_CHECKED : data_.MinimizeToTray;
    store_.Save(data_);
}

bool MainWindow::AutoStartLaunched()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    if (argv)
    {
        for (int i = 1; i < argc; i++)
            if (_wcsicmp(argv[i], L"--autostart") == 0) { found = true; break; }
        LocalFree(argv);
    }
    return found;
}

void MainWindow::ShutdownApp()
{
    if (shutdownStarted_) return;
    shutdownStarted_ = true;
    SaveState();
    if (watcher_) StopPerAppWatching();
    FilterEngine::Instance().Reset();
    FilterEngine::Instance().Shutdown();
    if (hotkeys_) hotkeys_->UnregisterAll();
    if (tray_) tray_->Remove();
    if (hwnd_ && !destroying_)
        DestroyWindow(hwnd_);
}

} // namespace hsf
