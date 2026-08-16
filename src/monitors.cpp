#include "monitors.h"

namespace hsf {

static BOOL CALLBACK EnumMonitorProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam)
{
    auto* list = reinterpret_cast<std::vector<DisplayMonitor>*>(lParam);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMonitor, &mi))
    {
        DisplayMonitor m;
        m.X = mi.rcMonitor.left;
        m.Y = mi.rcMonitor.top;
        m.Width = mi.rcMonitor.right - mi.rcMonitor.left;
        m.Height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        m.IsPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        list->push_back(m);
    }
    return TRUE;
}

std::vector<DisplayMonitor> Monitors::Enumerate()
{
    std::vector<DisplayMonitor> list;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, (LPARAM)&list);
    return list;
}

void Monitors::VirtualBounds(const std::vector<DisplayMonitor>& monitors,
                             int& left, int& top, int& right, int& bottom)
{
    if (monitors.empty())
    {
        left = 0; top = 0; right = 1920; bottom = 1080;
        return;
    }
    left = INT_MAX; top = INT_MAX; right = INT_MIN; bottom = INT_MIN;
    for (const auto& m : monitors)
    {
        left = (std::min)(left, m.X);
        top = (std::min)(top, m.Y);
        right = (std::max)(right, m.X + m.Width);
        bottom = (std::max)(bottom, m.Y + m.Height);
    }
}

} // namespace hsf
