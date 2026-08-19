// common.h — 公共定义与工具（HScreenFilter C++ 重构版）
// 仅依赖系统 DLL：d2d1/dwrite/d3d11/dxgi/d3dcompiler_47/magnification/gdi32/shell32 等。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// 统一使用宽字符 Win32 API
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
// 目标系统：Windows 10（启用较新的 API 声明，如 SetProcessDpiAwarenessContext）
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

namespace hsf {

// ---------------- 常量 ----------------
// 窗口类名（与旧版 C# 保持一致，用于 UI 逻辑跳过覆盖层窗口）
inline const wchar_t* kOverlayWindowClass = L"HScreenFilterVorticeOverlay";
inline const wchar_t* kAppName = L"HScreenFilter";
inline const wchar_t* kAppNameCn = L"屏幕滤镜";

// 版号标识（便捷：窗口标题/托盘/日志统一引用，升级版本只改这一处）
inline const wchar_t* kVersionString = L"v2.0.0-beta";

// 自定义修饰键掩码（MOD_WIN 为扩展；MOD_ALT/MOD_CONTROL/MOD_SHIFT 用系统宏）
constexpr int kModWin = 0x8;
constexpr long kWsExTopmost = 0x00000008L;

// ---------------- 基础工具 ----------------
inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

inline std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// 不区分大小写的字符串比较（用于 JSON 键名 / 进程名等）
inline bool IEquals(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

inline std::wstring ToLower(const std::wstring& s)
{
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)towlower(c);
    return r;
}

inline std::wstring Trim(const std::wstring& s)
{
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 把宽字符串格式化成 wstring（snprintf 风格）
std::wstring Format(const wchar_t* fmt, ...);

// 数字格式化
inline std::wstring NumToStr(double v)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.0f", v);
    return buf;
}

// 相对基准显示（饱和度 +20/-15 风格）
inline std::wstring NumToStrRel(double value, double base)
{
    double rel = value - base;
    if (rel > 0) return L"+" + NumToStr(rel);
    return NumToStr(rel);
}

// 解析宽字符串为 double（失败返回 false）
inline bool ParseDouble(const std::wstring& s, double& out)
{
    if (s.empty()) return false;
    errno = 0;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || errno == ERANGE) return false;
    while (*end == L' ' || *end == L'\t') end++;
    if (*end != L'\0') return false;
    out = v;
    return true;
}

inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float  ClampF(float v, float lo, float hi)   { return v < lo ? lo : (v > hi ? hi : v); }

// 取进程名（不含 .exe，例如 notepad）
std::wstring ProcessNameOfPid(uint32_t pid);

// 时间戳字符串（yyyy-MM-dd HH:mm:ss.fff）
std::wstring TimestampNow();

// 当前可执行文件路径
std::wstring ExePath();
std::wstring ExeDir();

// 本地应用数据目录 + 子目录
std::wstring LocalAppDataDir();
std::wstring HScreenFilterDataDir();

} // namespace hsf
