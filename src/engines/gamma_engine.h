// gamma_engine.h — 回退引擎：显卡伽马表（SetDeviceGammaRamp）。
// 可表达亮度/对比度/亮部/暗部/色温，但不能做跨通道的鲜艳度。
#pragma once
#include "common.h"
#include "models.h"

namespace hsf {

class GammaEngine
{
public:
    struct Ramp { unsigned short red[256], green[256], blue[256]; };

    static bool Test();
    static bool Apply(const FilterSettings& s);
    // 只在当前伽马表与我们期望的不一致时才重设。
    // 用途：任何显示模式切换（分辨率/HDR/游戏进出全屏/DSR）都会由驱动把伽马表
    // 重置为线性，导致滤镜"静默失效"；看门狗用这个接口低成本地恢复。
    static bool ApplyIfChanged(const FilterSettings& s);
    static bool Reset();

private:
    static Ramp LinearRamp();
    static Ramp BuildRamp(const FilterSettings& s);
};

} // namespace hsf
