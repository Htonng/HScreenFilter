#include "fgwatcher.h"
#include "log.h"

namespace hsf {

ForegroundAppWatcher::ForegroundAppWatcher() = default;

ForegroundAppWatcher::~ForegroundAppWatcher()
{
    Stop();
}

void ForegroundAppWatcher::SetTargets(const std::vector<AppBinding>& targets)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets_ = targets;
    }
    CheckNow(); // 立即重算一次，避免状态滞后
}

void ForegroundAppWatcher::Start(int intervalMs)
{
    if (running_) return;
    intervalMs_ = intervalMs;
    running_ = true;
    stopRequested_ = false;
    thread_ = std::thread([this] { OnTick(); });
}

void ForegroundAppWatcher::Stop()
{
    if (!running_) return;
    stopRequested_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void ForegroundAppWatcher::OnTick()
{
    CheckNow();
    while (!stopRequested_)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(intervalMs_));
        if (stopRequested_) break;
        lock.unlock();
        CheckNow();
    }
}

void ForegroundAppWatcher::CheckNow()
{
    std::wstring proc, title;
    HWND hwnd = GetForegroundWindow();
    if (hwnd) GetForegroundInfo(hwnd, proc, title);

    int hit = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!proc.empty())
        {
            for (size_t i = 0; i < targets_.size(); i++)
            {
                const auto& t = targets_[i];
                if (t.ProcessName.empty()) continue;
                if (!IEquals(proc, Trim(t.ProcessName))) continue;
                if (!Trim(t.WindowTitle).empty() &&
                    ToLower(title).find(ToLower(Trim(t.WindowTitle))) == std::wstring::npos) continue;
                hit = (int)i;
                break;
            }
        }
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasHit_ || hit != currentHit_)
        {
            hasHit_ = true;
            currentHit_ = hit;
            changed = true;
        }
    }
    if (changed && MatchChanged)
    {
        MatchChanged(hit, proc, title);
    }
}

bool ForegroundAppWatcher::GetForegroundInfo(HWND hwnd, std::wstring& process, std::wstring& title)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return false;
    process = ProcessNameOfPid(pid);

    int len = GetWindowTextLengthW(hwnd);
    if (len > 0)
    {
        std::wstring buf(len + 1, L'\0');
        GetWindowTextW(hwnd, &buf[0], len + 1);
        buf.resize(len);
        title = buf;
    }
    return !process.empty();
}

} // namespace hsf
