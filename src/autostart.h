// autostart.h — 开机自启（HKCU\...\Run，无需管理员权限）。
#pragma once
#include "common.h"

namespace hsf {

class AutoStart
{
public:
    static void Set(bool enabled);
    static bool IsEnabled();
};

} // namespace hsf
