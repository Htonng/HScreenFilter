// render_types.h — 捕获/处理/呈现三层之间传递的轻量值类型。
//
// 设计约束：三层只通过这些结构通信，不共享窗口句柄、不共享对方的内部状态。
//   * 捕获层只输出「一帧纹理 + 帧元数据 + 场景采样」；
//   * 处理层只接收「输入 SRV + 输出 RTV」，不知道帧从哪来；
//   * 呈现层只接收「已经画好的后缓冲」，不知道颜色是怎么算出来的。
#pragma once
#include "common.h"
#include "comptr.h"
#include <d3d11.h>

namespace hsf {

// 每秒一次的场景探测结果（延迟一帧回读；整个渲染链路唯一的 GPU→CPU 回读）。
struct SceneSample
{
    bool valid = false;       // 本轮是否读到有效结果
    bool notReady = false;    // 非阻塞 Map 时 GPU 还没写完（计入帧率明细的「探测未就绪」）
    double maxLuma = -1.0;    // 5 个采样区域的最大分量值（0..1）
    double mean = -1.0;       // 同一批采样点的平均亮度（sRGB 编码口径）
    double costMs = 0.0;      // 读取耗时
};

} // namespace hsf
