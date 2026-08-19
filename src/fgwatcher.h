// fgwatcher.h — 前台应用监听器：轮询当前前台窗口，命中绑定则触发事件。
#pragma once
#include "common.h"
#include "models.h"

namespace hsf {

class ForegroundAppWatcher
{
public:
    // 命中状态变化：(命中绑定索引或 -1, processName, windowTitle)。回调在工作线程。
    std::function<void(int hitBindingIndex, const std::wstring& process, const std::wstring& title)> MatchChanged;

    ForegroundAppWatcher();
    ~ForegroundAppWatcher();

    void SetTargets(const std::vector<AppBinding>& targets); // 复制目标列表（线程安全）
    void Start(int intervalMs = 500);
    void Stop();
    int CurrentHit() const; // 当前命中绑定索引或 -1（线程安全）

    // 供 UI 使用：获取当前前台窗口的进程名与标题
    static HWND GetForegroundWindowForPicker() { return GetForegroundWindow(); }
    static bool GetForegroundInfo(HWND hwnd, std::wstring& process, std::wstring& title);

private:
    void CheckNow();
    void OnTick();

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int intervalMs_ = 500;
    std::vector<AppBinding> targets_; // 内部副本（SetTargets 时更新，受 mutex_ 保护）
    int currentHit_ = -1;
    bool hasHit_ = false;
};

} // namespace hsf
