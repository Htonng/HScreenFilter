// msgwindow.h — 后台消息线程 + 隐藏窗口（接收 WM_HOTKEY / 托盘消息）。
// 线程模型与旧版一致：独立 STA 线程 + 标准 Win32 消息循环，保证前台全屏/游戏时也能收到热键。
#pragma once
#include "common.h"
#include <deque>
#include <condition_variable>

namespace hsf {

class MessageWindow
{
public:
    // 收到窗口消息时触发：(msg, wParam, lParam)。回调在后台消息线程上执行。
    // 支持多个订阅者（热键服务、托盘图标等）。
    void AddMessageHandler(std::function<void(uint32_t msg, WPARAM wParam, LPARAM lParam)> h)
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        handlers_.push_back(std::move(h));
    }

    MessageWindow();
    ~MessageWindow();

    bool Create();              // 启动线程并创建窗口
    HWND Handle() const { return hwnd_; }
    uint32_t ThreadId() const { return threadId_; }

    // 把委托切到“拥有窗口的后台消息线程”上同步执行（RegisterHotKey 等必须在该线程调用）。
    // 返回 false 表示投递失败（窗口不存在等）。
    bool InvokeOnMessageThread(std::function<void()> fn, int timeoutMs = 3000);

    void Destroy();

private:
    void ThreadMain();
    void CreateNative();
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT InstanceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void DrainInvokeQueue();

    static constexpr UINT kInvokeMsg = WM_APP + 1;

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> created_{ false };
    HWND hwnd_ = nullptr;
    uint32_t threadId_ = 0;

    std::mutex queueMutex_;
    std::deque<std::pair<std::function<void()>, std::shared_ptr<std::atomic<bool>>>> invokeQueue_;

    std::mutex handlerMutex_;
    std::vector<std::function<void(uint32_t, WPARAM, LPARAM)>> handlers_;
};

} // namespace hsf
