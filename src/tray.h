// tray.h — 系统托盘图标（Shell_NotifyIcon）。左键单击显示主窗口，右键弹出菜单。
#pragma once
#include "common.h"
#include "msgwindow.h"
#include <shellapi.h>

namespace hsf {

class TrayIcon
{
public:
    // 回调在消息线程触发
    std::function<void()> OnShow;
    std::function<void()> OnExit;
    // 提供配置列表（名称 + 是否激活），用于右键菜单显示并打勾；空则不显示配置菜单
    std::function<std::vector<std::pair<std::wstring, bool>>()> ProfilesProvider;
    // 用户在右键菜单点击某个配置时回调（参数为配置索引）
    std::function<void(int)> ProfileSelected;

    explicit TrayIcon(MessageWindow& window);
    ~TrayIcon();

    void Show();
    void ShowBalloon(const std::wstring& title, const std::wstring& message);
    void Remove();

private:
    void ShowContextMenu();
    HICON LoadTrayIcon();

    MessageWindow& window_;
    NOTIFYICONDATAW nid_{};
    bool added_ = false;
    HICON hIcon_ = nullptr;
    static constexpr UINT kTrayMsg = WM_APP + 0x64;
    static constexpr UINT_PTR kTrayId = 1001;
};

} // namespace hsf
