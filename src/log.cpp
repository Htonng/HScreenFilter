#include "log.h"
#include <cstdarg>
#include <cstdio>

namespace hsf {

static std::mutex g_logMutex;
static std::wstring g_logPath;

static std::wstring LogPathImpl()
{
    if (!g_logPath.empty()) return g_logPath;
    std::wstring dir = HScreenFilterDataDir();
    g_logPath = dir + L"\\debug.log";
    return g_logPath;
}

void Log::Write(const std::wstring& category, const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    // 使用 _wfopen（宽字符路径），追加写一行
    FILE* f = _wfopen(LogPathImpl().c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"[%s] [%s] %s\n", TimestampNow().c_str(), category.c_str(), message.c_str());
    fclose(f);
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
    std::lock_guard<std::mutex> lock(g_logMutex);
    _wremove(LogPathImpl().c_str());
}

} // namespace hsf
