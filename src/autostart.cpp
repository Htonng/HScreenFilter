#include "autostart.h"
#include <winreg.h>

namespace hsf {

static const wchar_t* kValueName = L"HScreenFilter";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

void AutoStart::Set(bool enabled)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enabled)
    {
        std::wstring exe = ExePath();
        if (!exe.empty())
        {
            // 带 --autostart 参数，便于区分“开机自启”与手动启动（用于“自启后自动进入托盘”）
            std::wstring cmd = L"\"" + exe + L"\" --autostart";
            RegSetValueExW(key, kValueName, 0, REG_SZ,
                (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        }
    }
    else
    {
        RegDeleteValueW(key, kValueName);
    }
    RegCloseKey(key);
}

bool AutoStart::IsEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    LONG r = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

} // namespace hsf
