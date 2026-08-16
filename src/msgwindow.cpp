#include "msgwindow.h"
#include "log.h"
#include <objbase.h>

namespace hsf {

static const wchar_t* kMsgClassName = L"HScreenFilterMsgWindow";

MessageWindow::MessageWindow() = default;

MessageWindow::~MessageWindow()
{
    Destroy();
}

bool MessageWindow::Create()
{
    if (running_) return true;
    running_ = true;
    thread_ = std::thread([this] { ThreadMain(); });

    // 等待窗口创建完成（最多 5 秒）
    for (int i = 0; i < 500 && !created_.load(); i++)
        Sleep(10);
    return created_.load();
}

void MessageWindow::ThreadMain()
{
    // 提高线程优先级，确保 WM_HOTKEY 在前台全屏/游戏进程时仍能及时被调度
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    threadId_ = GetCurrentThreadId();

    // 初始化 OLE（托盘菜单 TrackPopupMenuEx 无 COM 需求，但保持与旧版一致的 STA 环境）
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    CreateNative();
    created_ = true;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    CoUninitialize();
    running_ = false;
}

void MessageWindow::CreateNative()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMsgClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        Log::WriteFmt(L"MsgWindow", L"RegisterClassEx 失败: %u", GetLastError());
        return;
    }
    hwnd_ = CreateWindowExW(0, kMsgClassName, L"", 0, 0, 0, 0, 0,
                            nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_)
    {
        Log::WriteFmt(L"MsgWindow", L"CreateWindowEx 失败: %u", GetLastError());
    }
}

LRESULT CALLBACK MessageWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MessageWindow* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MessageWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    else
    {
        self = reinterpret_cast<MessageWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->InstanceWndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MessageWindow::InstanceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == kInvokeMsg)
    {
        DrainInvokeQueue();
        return 0;
    }
    // 把消息分发给所有订阅者（热键服务、托盘图标等）
    std::vector<std::function<void(uint32_t, WPARAM, LPARAM)>> copy;
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        copy = handlers_;
    }
    for (auto& h : copy)
    {
        if (h) h(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool MessageWindow::InvokeOnMessageThread(std::function<void()> fn, int timeoutMs)
{
    if (!hwnd_ || GetCurrentThreadId() == threadId_)
    {
        if (fn) fn();
        return true;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        invokeQueue_.emplace_back(std::move(fn), done);
    }
    if (!PostMessageW(hwnd_, kInvokeMsg, 0, 0))
    {
        // 投递失败：从队列移除
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!invokeQueue_.empty() && invokeQueue_.back().second == done)
            invokeQueue_.pop_back();
        return false;
    }
    // 等待执行完成
    for (int waited = 0; waited < timeoutMs && !done->load(); waited += 5)
        Sleep(5);
    return done->load();
}

void MessageWindow::DrainInvokeQueue()
{
    for (;;)
    {
        std::function<void()> fn;
        std::shared_ptr<std::atomic<bool>> done;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (invokeQueue_.empty()) return;
            fn = std::move(invokeQueue_.front().first);
            done = invokeQueue_.front().second;
            invokeQueue_.pop_front();
        }
        if (fn) fn();
        if (done) done->store(true);
    }
}

void MessageWindow::Destroy()
{
    if (!running_) return;
    // 向消息线程投递 WM_QUIT，让 GetMessage 返回 0 从而退出消息循环
    PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

} // namespace hsf
