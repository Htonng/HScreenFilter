#include "common.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <psapi.h>
#include <cstdarg>

namespace hsf {

std::wstring Format(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n < 0) return L"";
    std::wstring buf(n + 1, L'\0');
    va_start(ap, fmt);
    vswprintf(&buf[0], buf.size(), fmt, ap);
    va_end(ap);
    buf.resize(n);
    return buf;
}

std::wstring ProcessNameOfPid(uint32_t pid)
{
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return name;
    wchar_t buf[MAX_PATH];
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz))
    {
        // 取文件名（不含路径与 .exe）
        wchar_t* p = wcsrchr(buf, L'\\');
        wchar_t* base = p ? p + 1 : buf;
        wchar_t* dot = wcsrchr(base, L'.');
        if (dot) *dot = L'\0';
        name = base;
    }
    CloseHandle(h);
    return name;
}

std::wstring TimestampNow()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

std::wstring ExePath()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n > 0 ? std::wstring(buf, n) : L"";
}

std::wstring ExeDir()
{
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

std::wstring LocalAppDataDir()
{
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf) != S_OK)
        return L"";
    return buf;
}

std::wstring HScreenFilterDataDir()
{
    std::wstring dir = LocalAppDataDir() + L"\\HScreenFilter";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

} // namespace hsf
