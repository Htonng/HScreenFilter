// log.h — 简单文件日志（%LocalAppData%\HScreenFilter\debug.log），线程安全。
#pragma once
#include "common.h"

namespace hsf {

class Log
{
public:
    static void Write(const std::wstring& category, const std::wstring& message);
    static void WriteFmt(const wchar_t* category, const wchar_t* fmt, ...);
    static std::wstring FilePath();
    static void Clear();
};

} // namespace hsf
