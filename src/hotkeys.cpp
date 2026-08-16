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
    InstallKeyboardHook();
}

HotkeyService::~HotkeyService()
{
    UnregisterAll();
    if (g_self == this) g_self = nullptr;
}

bool HotkeyService::Register(int modifiers, int key, std::function<void()> callback, int& id)
{
    id = 0;
    if (!window_.Handle() || key == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (comboIds_.count({ modifiers, key })) return false;

    id = nextId_++;
    int regId = id;
    uint32_t fsMods = (uint32_t)modifiers | kModNoRepeat;
    bool ok = false;
    // RegisterHotKey 必须在“拥有窗口的消息线程”上调用
    window_.InvokeOnMessageThread([&] {
        ok = RegisterHotKey(window_.Handle(), regId, fsMods, (uint32_t)key) != FALSE;
    });

    callbacks_[id] = std::move(callback);
    comboIds_[{ modifiers, key }] = id;
    if (ok) registeredWithWin32_.insert(id);
    else InstallKeyboardHook(); // 组合被占用/注册失败 → 用钩子兜底

    Log::WriteFmt(L"Hotkey", L"注册 %s: %s", HotkeyText::Format(modifiers, key).c_str(),
                  ok ? L"RegisterHotKey 成功" : L"RegisterHotKey 失败，使用键盘钩子兜底");
    return true;
}

bool HotkeyService::Unregister(int id)
{
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = callbacks_.find(id);
    if (it == callbacks_.end()) return false;

    if (window_.Handle() && registeredWithWin32_.count(id))
    {
        window_.InvokeOnMessageThread([&] { UnregisterHotKey(window_.Handle(), id); });
    }
    registeredWithWin32_.erase(id);
    callbacks_.erase(it);
    for (auto kv = comboIds_.begin(); kv != comboIds_.end(); ++kv)
    {
        if (kv->second == id) { comboIds_.erase(kv); break; }
    }
    if (comboIds_.empty()) UninstallKeyboardHook();
    return true;
}

void HotkeyService::UnregisterAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (window_.Handle())
    {
        std::vector<int> ids(registeredWithWin32_.begin(), registeredWithWin32_.end());
        window_.InvokeOnMessageThread([&] {
            for (int id : ids) UnregisterHotKey(window_.Handle(), id);
        });
    }
    registeredWithWin32_.clear();
    callbacks_.clear();
    comboIds_.clear();
    UninstallKeyboardHook();
}

void HotkeyService::InstallKeyboardHook()
{
    if (hookInstalled_.exchange(true)) return;
    HHOOK hook = SetWindowsHookExW(kWHKeyboardLL, HookCallback, GetModuleHandleW(nullptr), 0);
    if (hook)
    {
        hookHandle_ = hook;
        Log::Write(L"Hotkey", L"低级键盘钩子已安装");
    }
    else
    {
        hookInstalled_.store(false);
    }
}

void HotkeyService::UninstallKeyboardHook()
{
    if (!hookInstalled_.exchange(false)) return;
    if (hookHandle_)
    {
        UnhookWindowsHookEx(hookHandle_);
        hookHandle_ = nullptr;
    }
}

LRESULT CALLBACK HotkeyService::HookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        UINT msg = (UINT)wParam;
        bool keyDown = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (keyDown && lParam)
        {
            auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            int vk = (int)kb->vkCode;

            int mods = 0;
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) mods |= MOD_CONTROL;
            if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) mods |= MOD_SHIFT;
            if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) mods |= MOD_ALT;
            if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
                mods |= kModWin;

            // 需要访问实例：通过文件级静态指针 g_self（钩子回调无 this）
            if (g_self)
            {
                int id = -1;
                bool regWithWin32 = false;
                {
                    std::lock_guard<std::mutex> lock(g_self->mutex_);
                    auto it = g_self->comboIds_.find({ mods, vk });
                    if (it != g_self->comboIds_.end())
                    {
                        id = it->second;
                        regWithWin32 = g_self->registeredWithWin32_.count(id) > 0;
                    }
                }
                if (id > 0)
                {
                    // 若该组合已成功注册到 RegisterHotKey（系统会自行投递 WM_HOTKEY），
                    // 这里必须跳过，否则同一按键回调触发两次（toggle 两次=开→关）
                    if (regWithWin32)
                        return CallNextHookEx(nullptr, nCode, wParam, lParam);

                    // 钩子兜底：向消息窗口投递 WM_HOTKEY，走统一分发路径
                    if (g_self->window_.Handle())
                    {
                        PostMessageW(g_self->window_.Handle(), WM_HOTKEY, (WPARAM)id, 0);
                    }
                    else
                    {
                        std::function<void()> cb;
                        {
                            std::lock_guard<std::mutex> lock(g_self->mutex_);
                            auto it2 = g_self->callbacks_.find(id);
                            if (it2 != g_self->callbacks_.end()) cb = it2->second;
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
