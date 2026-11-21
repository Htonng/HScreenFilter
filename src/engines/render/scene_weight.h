// scene_weight.h — 暗场景优化的「场景自适应权重」纯逻辑（可在自检里直接验证）。
//
// 历史问题：权重每秒根据 5 个 16×16 采样块的平均亮度重算一次。游戏里手电、枪口
// 闪光、明暗房间切换都会让这个小样本均值在 0.05~0.34 之间跳，权重于是 0.5↔1.0
// 来回跳 —— 用户看到的就是「滤镜强度一会强一会弱」。
//
// 这里不改变「越暗越强」的设计，只做两件事：
//   1. 对场景平均亮度做长时间平滑（τ=15s），短促的明暗变化不进入权重；
//   2. 对权重变化限速（每次最多 0.04，最快 2.5s 一次），即使真的换场景也是
//      缓慢过渡，而不是一秒内翻倍。
// 纯函数、无 D3D 依赖，`--selftest` 用交替明暗序列验证它不再抖动。
#pragma once
#include <cmath>
#include <cstdint>

namespace hsf {

// 场景平均亮度（sRGB 编码口径）→ 目标权重。
// 越暗越接近 1；明亮场景最低 kMinWeight（用户滑块永远至少保留这个比例，
// 5 块采样不足以静默否决用户意图）。
constexpr double kDarkSceneMinWeight = 0.5;
constexpr double kSceneSmoothTauSec = 15.0;
constexpr double kSceneWeightDeadband = 0.02;
constexpr double kSceneWeightMaxStep = 0.04;
constexpr uint32_t kSceneWeightMinIntervalMs = 2500;

inline double TargetDarkSceneWeight(double mean)
{
    double t = (mean - 0.08) / (0.35 - 0.08);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double s = t * t * (3.0 - 2.0 * t);   // smoothstep(0.08, 0.35, mean)
    return kDarkSceneMinWeight + (1.0 - kDarkSceneMinWeight) * (1.0 - s);
}

struct SceneWeightState
{
    double smoothedMean = -1.0;       // <0 表示还没有稳定采样
    uint32_t lastSampleTick = 0;
    double appliedWeight = 1.0;       // 当前真正写进常量缓冲槽 42 的权重
    uint32_t lastChangeTick = 0;

    void Reset()
    {
        smoothedMean = -1.0;
        lastSampleTick = 0;
        appliedWeight = 1.0;
        lastChangeTick = 0;
    }
};

// 处理一拍场景采样。返回 true 表示权重改变、需要重建 LUT；newWeight 为本次生效值。
// sliderOn=false 时清空平滑状态（用户关掉暗场景优化后再打开，下一拍按当前场景重新起算）。
inline bool AdvanceDarkSceneWeight(SceneWeightState& st, double mean, uint32_t nowTick,
                                   bool startupStable, bool sliderOn, double& newWeight)
{
    if (!sliderOn || mean < 0.0)
    {
        st.Reset();
        return false;
    }
    // 启动后的前 2.5 秒捕获/flip 池/合成器都还没稳定，采样本身可能是全黑帧。
    if (!startupStable) return false;

    if (st.smoothedMean < 0.0)
    {
        // 第一拍直接采用当前场景：否则用户一开滤镜要等十几秒才看到正确强度。
        st.smoothedMean = mean;
        st.lastSampleTick = nowTick;
        st.appliedWeight = TargetDarkSceneWeight(mean);
        st.lastChangeTick = nowTick;
        newWeight = st.appliedWeight;
        return true;
    }

    double dtSec = (double)(nowTick - st.lastSampleTick) / 1000.0;
    st.lastSampleTick = nowTick;
    if (dtSec < 0.2) return false;
    // 卡顿/断点后不要用一次跳变把平滑值拉过去。
    if (dtSec > 5.0) dtSec = 5.0;
    const double alpha = 1.0 - exp(-dtSec / kSceneSmoothTauSec);
    st.smoothedMean += alpha * (mean - st.smoothedMean);

    const double target = TargetDarkSceneWeight(st.smoothedMean);
    if (fabs(target - st.appliedWeight) < kSceneWeightDeadband) return false;
    if (st.lastChangeTick != 0 && nowTick - st.lastChangeTick < kSceneWeightMinIntervalMs)
        return false;

    double delta = target - st.appliedWeight;
    if (delta > kSceneWeightMaxStep) delta = kSceneWeightMaxStep;
    else if (delta < -kSceneWeightMaxStep) delta = -kSceneWeightMaxStep;
    if (fabs(delta) < 0.005) return false;

    st.appliedWeight += delta;
    st.lastChangeTick = nowTick;
    newWeight = st.appliedWeight;
    return true;
}

} // namespace hsf
