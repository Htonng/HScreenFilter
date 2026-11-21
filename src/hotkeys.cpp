#include "hotkeys.h"
#include "log.h"
#include "models.h"
#include <set>
#include <map>

namespace hsf {

static constexpr UINT kModNoRepeat = 0x4000;
static constexpr int kWHKeyboardLL = 13;

// 钩子回调为静态函数，需要访问实例；用文件级静态指针（单实例服务）
static HotkeyService* g_self = nullptr;

HotkeyService::HotkeyService(MessageWindow& window) : window_(window)
{
    g_self = this;
    window_.AddMessageHandler([this](uint32_t msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_HOTKEY)
        {
            int id = (int)wParam;
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = callbacks_.find(id);
                if (it != callbacks_.end()) cb = it->second;
            }
            if (cb) cb();
        }
    });
    // 注意：这里不再无条件 InstallKeyboardHook()。
    // 旧实现的构造器一启动就装一个全局 WH_KEYBOARD_LL，即使所有组合都已成功
    // RegisterHotKey；这个常驻的全局键盘钩子会与游戏/反作弊自己的键盘钩子
    // 互相干扰（用户反馈的「和有些游戏快捷键冲突」），也完全没必要。
}

HotkeyService::~HotkeyService()
{
    UnregisterAll();
    if (g_self == this) g_self = nullptr;
}

void HotkeyService::SetPreferHook(bool prefer)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (preferHook_ == prefer) return;
        preferHook_ = prefer;
    }
    // 已有的注册不会因为改标志位而改变后端：调用方需要 UnregisterAll() 后重新 Register。
    Log::WriteFmt(L"Hotkey", L"按键兼容模式：%s",
                  prefer ? L"开（全部走被动键盘钩子，不独占游戏按键）"
                         : L"关（优先 RegisterHotKey）");
}

bool HotkeyService::Register(int modifiers, int key, std::function<void()> callback, int& id)
{
    id = 0;
    if (!window_.Handle() || key == 0) return false;

    // 先占位：重复组合直接拒绝，避免两个 id 抢同一个按键。
    int assigned = 0;
    bool preferHook = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (comboIds_.count({ modifiers, key })) return false;
        preferHook = preferHook_;
        assigned = nextId_++;
        callbacks_[assigned] = std::move(callback);
        comboIds_[{ modifiers, key }] = assigned;
    }
    id = assigned;

    const int regId = assigned;
    const uint32_t fsMods = (uint32_t)modifiers | kModNoRepeat;
    bool ok = false;
    if (!preferHook)
    {
        // RegisterHotKey 必须在消息窗口线程上执行。结果用 shared_ptr 承载：
        // InvokeOnMessageThread 超时返回时 lambda 仍可能晚一点执行，绝不能让 lambda
        // 写一个已经离开作用域的栈变量（旧实现的悬空引用）。
        auto result = std::make_shared<std::atomic<bool>>(false);
        const bool invoked = window_.InvokeOnMessageThread([this, regId, fsMods, key, result] {
            result->store(RegisterHotKey(window_.Handle(), regId, fsMods, (uint32_t)key) != FALSE);
        }, 3000);
        ok = invoked && result->load();
    }
    // 兼容模式不调用 RegisterHotKey：系统不会独占该键，游戏/其它程序仍能收到。
    // 代价是若游戏自己的低级钩子先消费了这个键，本程序可能收不到热键。

    if (ok)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registeredWithWin32_.insert(regId);
    }
    // 注册失败时按需装钩子；成功时也可能因为别的组合需要钩子而保持安装。
    SyncHookState();

    if (!ok && !hookInstalled_.load())
    {
        // 组合被其它程序占用，且系统不允许装钩子：撤销占位并告诉调用方。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_.erase(regId);
            registeredWithWin32_.erase(regId);
            for (auto it = comboIds_.begin(); it != comboIds_.end(); ++it)
                if (it->second == regId) { comboIds_.erase(it); break; }
        }
        id = 0;
        Log::WriteFmt(L"Hotkey", L"注册 %s (VK%02X) 失败：%s",
                      HotkeyText::Format(modifiers, key).c_str(), key,
                      preferHook ? L"按键兼容模式的键盘钩子不可用"
                                 : L"RegisterHotKey 被占用且键盘钩子不可用");
        return false;
    }

    // 附上 VK 码：小键盘 -/. / Enter 与主键区 M/./Enter 共用同一 VK，只按名字看不出区别
    Log::WriteFmt(L"Hotkey", L"注册 %s (VK%02X): %s", HotkeyText::Format(modifiers, key).c_str(), key,
                  ok ? L"RegisterHotKey 成功（不装全局键盘钩子）"
                     : (preferHook ? L"按键兼容模式：被动键盘钩子（不独占按键）"
                                   : L"RegisterHotKey 失败，使用键盘钩子兜底（不独占按键）"));
    return true;
}

bool HotkeyService::Unregister(int id)
{
    if (id == 0) return false;

    bool withWin32 = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = callbacks_.find(id);
        if (it == callbacks_.end()) return false;
        withWin32 = registeredWithWin32_.count(id) > 0;
        registeredWithWin32_.erase(id);
        callbacks_.erase(it);
        for (auto kv = comboIds_.begin(); kv != comboIds_.end(); ++kv)
        {
            if (kv->second == id) { comboIds_.erase(kv); break; }
        }
    }

    if (withWin32 && window_.Handle())
    {
        window_.InvokeOnMessageThread([this, id] { UnregisterHotKey(window_.Handle(), id); }, 3000);
    }
    // 最后一条「钩子兜底」组合消失后立刻卸载，不能把全局钩子留在进程里。
    SyncHookState();
    return true;
}

void HotkeyService::UnregisterAll()
{
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ids.assign(registeredWithWin32_.begin(), registeredWithWin32_.end());
        registeredWithWin32_.clear();
        callbacks_.clear();
        comboIds_.clear();
    }
    if (window_.Handle() && !ids.empty())
    {
        window_.InvokeOnMessageThread([this, ids] {
            for (int id : ids) UnregisterHotKey(window_.Handle(), id);
        }, 3000);
    }
    UninstallKeyboardHook();
}

bool HotkeyService::HasHookOnlyComboLocked() const
{
    for (const auto& kv : comboIds_)
        if (!registeredWithWin32_.count(kv.second)) return true;
    return false;
}

void HotkeyService::SyncHookState()
{
    bool needHook = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needHook = HasHookOnlyComboLocked();
    }
    if (needHook) InstallKeyboardHook();
    else UninstallKeyboardHook();
}

void HotkeyService::InstallKeyboardHook()
{
    if (hookInstalled_.load()) return;
    if (!window_.Handle()) return;

    // 装在消息线程上：它有自己的消息循环且是 THREAD_PRIORITY_HIGHEST，
    // 不会像 UI/WebView2 线程那样被页面渲染卡住；钩子回调也必须在该线程泵消息。
    auto handle = std::make_shared<std::atomic<HHOOK>>(nullptr);
    const bool invoked = window_.InvokeOnMessageThread([this, handle] {
        handle->store(SetWindowsHookExW(kWHKeyboardLL, HookCallback, GetModuleHandleW(nullptr), 0));
        keyDown_.fill(false);   // 仅消息线程访问
    }, 3000);
    HHOOK hook = invoked ? handle->load() : nullptr;
    if (!hook)
    {
        Log::WriteFmt(L"Hotkey", L"低级键盘钩子安装失败 (err=%u)，被占用的组合无法兜底",
                      (unsigned)GetLastError());
        return;
    }
    hookHandle_.store(hook);
    hookInstalled_.store(true);
    Log::Write(L"Hotkey", L"低级键盘钩子已安装（仅用于 RegisterHotKey 失败的组合，不独占按键）");
}

void HotkeyService::UninstallKeyboardHook()
{
    if (!hookInstalled_.exchange(false)) return;
    HHOOK hook = hookHandle_.exchange(nullptr);
    if (!hook) return;

    if (!window_.Handle() || GetCurrentThreadId() == window_.ThreadId())
    {
        UnhookWindowsHookEx(hook);
        keyDown_.fill(false);
    }
    else
    {
        window_.InvokeOnMessageThread([this, hook] {
            UnhookWindowsHookEx(hook);
            keyDown_.fill(false);
        }, 1000);
    }
    Log::Write(L"Hotkey", L"低级键盘钩子已卸载（没有需要兜底的热键）");
}

LRESULT CALLBACK HotkeyService::HookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && lParam)
    {
        const UINT msg = (UINT)wParam;
        const bool keyDownMsg = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        const bool keyUpMsg = msg == WM_KEYUP || msg == WM_SYSKEYUP;
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const int vk = (int)kb->vkCode;

        HotkeyService* self = g_self;
        if (self && vk >= 0 && vk < 256)
        {
            // 释放：清掉按下状态。按住热键时 Windows 会重复发 WM_KEYDOWN，
            // 不去重就会把“开关滤镜”连续触发（开→关→开…）。
            if (keyUpMsg)
            {
                self->keyDown_[vk] = false;
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }
            if (keyDownMsg)
            {
                if (self->keyDown_[vk])
                    return CallNextHookEx(nullptr, nCode, wParam, lParam);
                self->keyDown_[vk] = true;

                int mods = 0;
                if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) mods |= MOD_CONTROL;
                if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) mods |= MOD_SHIFT;
                if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) mods |= MOD_ALT;
                if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                    (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
                    mods |= kModWin;

                int id = -1;
                bool regWithWin32 = false;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    auto it = self->comboIds_.find({ mods, vk });
                    if (it != self->comboIds_.end())
                    {
                        id = it->second;
                        regWithWin32 = self->registeredWithWin32_.count(id) > 0;
                    }
                }
#ifdef HSF_HOTKEY_DEBUG
                Log::WriteFmt(L"Hotkey", L"[hook-debug] vk=%d mods=%d id=%d win32=%d",
                              vk, mods, id, regWithWin32 ? 1 : 0);
#endif

                if (id > 0 && !regWithWin32)
                {
                    // 若该组合已成功注册到 RegisterHotKey，系统会自行投递 WM_HOTKEY，
                    // 这里必须跳过，否则同一按键回调触发两次（toggle 两次=开→关）。
                    // 钩子只观察、不消费：CallNextHookEx 让游戏/前台程序照常收到按键。
                    if (HWND hwnd = self->window_.Handle())
                    {
                        PostMessageW(hwnd, WM_HOTKEY, (WPARAM)id, 0);
                    }
                    else
                    {
                        std::function<void()> cb;
                        {
                            std::lock_guard<std::mutex> lock(self->mutex_);
                            auto it2 = self->callbacks_.find(id);
                            if (it2 != self->callbacks_.end()) cb = it2->second;
                        }
                        if (cb) cb();
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace hsf
