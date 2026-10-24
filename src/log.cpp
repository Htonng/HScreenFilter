#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <thread>
#include <condition_variable>

namespace hsf {

// ---------------------------------------------------------------------------
// 异步日志：热路径（渲染线程 / UI 线程）只把整行文本推进队列，真正的文件写入由
// 后台写线程做。
//
// 为什么必须异步：原来 Write 在调用线程里 _wfopen/fwrite/fclose，而这个文件在
// %LocalAppData%（可能被 Defender/索引/同步软件扫），实测偶发 0.5~0.9 秒的写盘阻塞。
// 渲染线程每秒要写两行（帧率明细 + 场景探测），一旦卡住就是「画面冻一下」——
// 用户日志里那些 `帧间隔 最大 921ms / 准备 最大 917ms` 就是它，表现为滤镜帧率
// 在 20~180 之间乱跳（RTSS 里游戏却稳在 100+）。
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kMaxQueuedLines = 4000;   // 队列上限：写盘跟不上时丢最老的，绝不无限增长

struct LogState
{
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::wstring> queue;
    bool stop = false;
    std::thread writer;
    bool started = false;
    size_t dropped = 0;

    // 进程退出时先把队列写完再让写线程结束（std::thread 析构时仍 joinable 会 terminate）
    ~LogState()
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();
        if (writer.joinable()) writer.join();
    }
};

LogState& State()
{
    static LogState s;
    return s;
}

std::wstring LogPathImpl()
{
    static std::wstring path = HScreenFilterDataDir() + L"\\debug.log";
    return path;
}

// 后台写线程：批量取走队列里的行，一次 fopen 写完（减少 open/close 次数）
void WriterLoop()
{
    for (;;)
    {
        std::deque<std::wstring> batch;
        {
            std::unique_lock<std::mutex> lock(State().mtx);
            State().cv.wait_for(lock, std::chrono::milliseconds(500),
                                [] { return !State().queue.empty() || State().stop; });
            batch.swap(State().queue);
            if (batch.empty() && State().stop) return;
        }
        if (batch.empty()) continue;

        FILE* f = _wfopen(LogPathImpl().c_str(), L"a, ccs=UTF-8");
        if (!f) continue;   // 写不了就丢掉这批（日志从不该拖慢滤镜）
        for (const auto& line : batch)
        {
            fputws(line.c_str(), f);
            fputwc(L'\n', f);
        }
        fclose(f);
    }
}

void EnsureWriterStarted()
{
    if (State().started) return;
    State().started = true;
    State().writer = std::thread(WriterLoop);
}

} // namespace

void Log::Write(const std::wstring& category, const std::wstring& message)
{
    std::wstring line = L"[" + TimestampNow() + L"] [" + category + L"] " + message;
    {
        std::lock_guard<std::mutex> lock(State().mtx);
        EnsureWriterStarted();
        if (State().queue.size() >= kMaxQueuedLines)
        {
            State().queue.pop_front();
            State().dropped++;
        }
        State().queue.push_back(std::move(line));
    }
    State().cv.notify_one();
}

void Log::WriteFmt(const wchar_t* category, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vscwprintf(fmt, ap);
    va_end(ap);
    std::wstring msg(n + 1, L'\0');
    va_start(ap, fmt);
    vswprintf(&msg[0], msg.size(), fmt, ap);
    va_end(ap);
    msg.resize(n);
    Write(category, msg);
}

std::wstring Log::FilePath() { return LogPathImpl(); }

void Log::Clear()
{
    std::lock_guard<std::mutex> lock(State().mtx);
    State().queue.clear();
    _wremove(LogPathImpl().c_str());
}

} // namespace hsf
