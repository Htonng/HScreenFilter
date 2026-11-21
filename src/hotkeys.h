// hotkeys.h — 全局热键：RegisterHotKey 优先，低级键盘钩子仅在注册失败时兜底。
//
// 关键设计（用户反馈「和游戏快捷键冲突」后重做）：
//   * 构造时不再无条件安装 WH_KEYBOARD_LL。正常启动（所有组合都注册到 Win32）
//     时进程里没有任何全局键盘钩子，不会与游戏/反作弊的键盘钩子互相干扰。
//   * 只有某个组合 RegisterHotKey 失败（被其它程序占用）时才按需安装钩子，
//     且钩子只观察、不消费按键；组合全部注销后立即卸载。
//   * 钩子安装在 MessageWindow 的消息线程（HIGHEST 优先级 + 消息循环）上，
//     不占用 UI/WebView2 线程；按住热键不会重复触发（按 keyDown 状态去重）。
#pragma once
#include "common.h"
#include "msgwindow.h"
#include <array>
#include <map>
#include <set>

namespace hsf {

class HotkeyService
{
public:
    explicit HotkeyService(MessageWindow& window);
    ~HotkeyService();

    // 注册全局热键。组合已被本服务占用/既不能 RegisterHotKey 也不能装钩子时返回 false。
    bool Register(int modifiers, int key, std::function<void()> callback, int& id);
    bool Unregister(int id);
    void UnregisterAll();

    // 按键兼容模式：所有组合都改用被动低级键盘钩子（不 RegisterHotKey），
    // 游戏/其它程序仍能收到同一按键。调用方切换模式后需要重新注册全部热键。
    void SetPreferHook(bool prefer);

private:
    void InstallKeyboardHook();
    void UninstallKeyboardHook();
    void SyncHookState();                 // 按「是否还有只能靠钩子兜底的组合」安装/卸载
    bool HasHookOnlyComboLocked() const;  // 调用方必须已持有 mutex_
    static LRESULT CALLBACK HookCallback(int nCode, WPARAM wParam, LPARAM lParam);

    MessageWindow& window_;
    std::mutex mutex_;
    std::map<int, std::function<void()>> callbacks_;                 // id -> callback
    std::map<std::pair<int, int>, int> comboIds_;                    // (mods,key) -> id
    std::set<int> registeredWithWin32_;                              // 已成功 RegisterHotKey 的 id
    int nextId_ = 1;

    bool preferHook_ = false;   // 按键兼容模式（受 mutex_ 保护）

    // 仅由消息线程（安装钩子的线程）访问：按住热键时的自动重复不重复触发。
    std::array<bool, 256> keyDown_{};

    std::atomic<HHOOK> hookHandle_{ nullptr };
    std::atomic<bool> hookInstalled_{ false };
};

} // namespace hsf
