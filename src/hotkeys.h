// hotkeys.h — 全局热键：RegisterHotKey + 低级键盘钩子兜底（与旧版逻辑一致）。
#pragma once
#include "common.h"
#include "msgwindow.h"
#include <map>
#include <set>

namespace hsf {

class HotkeyService
{
public:
    explicit HotkeyService(MessageWindow& window);
    ~HotkeyService();

    // 注册全局热键。组合已被占用返回 false。
    bool Register(int modifiers, int key, std::function<void()> callback, int& id);
    bool Unregister(int id);
    void UnregisterAll();

private:
    void InstallKeyboardHook();
    void UninstallKeyboardHook();
    static LRESULT CALLBACK HookCallback(int nCode, WPARAM wParam, LPARAM lParam);

    MessageWindow& window_;
    std::mutex mutex_;
    std::map<int, std::function<void()>> callbacks_;                 // id -> callback
    std::map<std::pair<int, int>, int> comboIds_;                    // (mods,key) -> id
    std::set<int> registeredWithWin32_;                              // 已成功 RegisterHotKey 的 id
    int nextId_ = 1;

    HHOOK hookHandle_ = nullptr;
    std::atomic<bool> hookInstalled_{ false };
};

} // namespace hsf
