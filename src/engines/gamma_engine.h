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
    static bool Reset();

private:
    static Ramp LinearRamp();
    static Ramp BuildRamp(const FilterSettings& s);
};

} // namespace hsf
