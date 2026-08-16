// monitors.h — 显示器枚举（虚拟坐标 + 尺寸 + 主显示器标记）。
#pragma once
#include "common.h"
#include "models.h"

namespace hsf {

class Monitors
{
public:
    // 用 EnumDisplayMonitors 枚举全部显示器。失败返回空列表。
    static std::vector<DisplayMonitor> Enumerate();

    // 虚拟桌面总边界
    static void VirtualBounds(const std::vector<DisplayMonitor>& monitors,
                              int& left, int& top, int& right, int& bottom);
};

} // namespace hsf
