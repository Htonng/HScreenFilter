#include "tray.h"
#include "log.h"
#include <shellapi.h>

namespace hsf {

static constexpr DWORD kNimAdd = 0x00000000;
static constexpr DWORD kNimModify = 0x00000001;
static constexpr DWORD kNimDelete = 0x00000002;
static constexpr DWORD kNifMessage = 0x00000001;
static constexpr DWORD kNifIcon = 0x00000002;
static constexpr DWORD kNifTip = 0x00000004;
static constexpr DWORD kNifInfo = 0x00000010;
static constexpr DWORD kNiifInfo = 0x00000001;
static constexpr UINT kMfString = 0x00000000;
static constexpr UINT kMfSeparator = 0x00000800;
static constexpr UINT kMfChecked = 0x00000008;
static constexpr UINT kTpmRightButton = 0x0002;
static constexpr UINT kTpmReturnCmd = 0x0100;

TrayIcon::TrayIcon(MessageWindow& window) : window_(window)
{
    window_.AddMessageHandler([this](uint32_t msg, WPARAM wParam, LPARAM lParam) {
        if (msg == kTrayMsg)
        {
            UINT mouseMsg = (UINT)lParam;
            if (mouseMsg == WM_LBUTTONUP)
            {
                if (OnShow) OnShow();
            }
            else if (mouseMsg == WM_RBUTTONUP)
            {
                ShowContextMenu();
            }
        }
    });
}

TrayIcon::~TrayIcon()
{
    Remove();
}

void TrayIcon::Show()
{
    HWND hwnd = window_.Handle();
    if (!hwnd) return;
    hIcon_ = LoadTrayIcon();

    memset(&nid_, 0, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd;
    nid_.uID = (UINT)kTrayId;
    nid_.uFlags = kNifMessage | kNifIcon | kNifTip;
    nid_.uCallbackMessage = kTrayMsg;
    nid_.hIcon = hIcon_;
    wcscpy_s(nid_.szTip, L"HScreenFilter");
    added_ = Shell_NotifyIconW(kNimAdd, &nid_) != FALSE;
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& message)
{
    if (!added_) return;
    nid_.uFlags |= kNifInfo;
    wcscpy_s(nid_.szInfo, message.c_str());
    wcscpy_s(nid_.szInfoTitle, title.c_str());
    nid_.dwInfoFlags = kNiifInfo;
    Shell_NotifyIconW(kNimModify, &nid_);
}

void TrayIcon::Remove()
{
    if (added_)
    {
        Shell_NotifyIconW(kNimDelete, &nid_);
        added_ = false;
    }
    if (hIcon_)
    {
        DestroyIcon(hIcon_);
        hIcon_ = nullptr;
    }
}

HICON TrayIcon::LoadTrayIcon()
{
    // 优先使用随程序发布的 icon.ico
    std::wstring iconPath = ExeDir() + L"\\assets\\icon.ico";
    HICON h = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    if (h) return h;
    // 兜底：加载 exe 自带图标
    h = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"APPICON", IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (h) return h;
    // 再兜底：程序图标资源（如果编译时嵌入）
    h = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(101), IMAGE_ICON, 32, 32, 0);
    return h; // 可能为 null（托盘显示默认图标）
}

void TrayIcon::ShowContextMenu()
{
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, kMfString, (UINT_PTR)1, L"显示主窗口");

    std::vector<std::pair<std::wstring, bool>> profiles;
    if (ProfilesProvider) profiles = ProfilesProvider();
    if (!profiles.empty())
    {
        AppendMenuW(menu, kMfSeparator, 0, nullptr);
        for (size_t i = 0; i < profiles.size(); i++)
        {
            UINT flags = kMfString | (profiles[i].second ? kMfChecked : 0);
            AppendMenuW(menu, flags, (UINT_PTR)(100 + i), profiles[i].first.c_str());
        }
    }

    AppendMenuW(menu, kMfSeparator, 0, nullptr);
    AppendMenuW(menu, kMfString, (UINT_PTR)2, L"退出");

    HWND hwnd = window_.Handle();
    if (hwnd)
    {
        SetForegroundWindow(hwnd);
        int cmd = (int)TrackPopupMenuEx(menu, kTpmReturnCmd | kTpmRightButton, pt.x, pt.y, hwnd, nullptr);
        if (cmd == 1)
        {
            if (OnShow) OnShow();
        }
        else if (cmd == 2)
        {
            if (OnExit) OnExit();
        }
        else if (cmd >= 100 && cmd < 100 + (int)profiles.size())
        {
            if (ProfileSelected) ProfileSelected(cmd - 100);
        }
    }
    DestroyMenu(menu);
}

} // namespace hsf
