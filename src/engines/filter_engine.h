// filter_engine.h — 滤镜引擎门面（等价旧版 FilterEngine + ShaderFilterEngine）：
//   PixelShader（DXGI 覆盖层 + 3D LUT 引擎，支持 HSL 分色系）→ 放大镜颜色矩阵 → 伽马曲线
#pragma once
#include "common.h"
#include "models.h"
#include "lut_engine.h"
#include "mag_engine.h"
#include <map>

namespace hsf {

enum class EngineKind
{
    None,
    PixelShader,       // 逐像素着色器（3D LUT 引擎，能力最强：真正的分色系 HSL）
    FullScreenColorEffect,
    GammaRamp,
};

class FilterEngine
{
public:
    static FilterEngine& Instance();

    // 探测并选定可用引擎（幂等）
    bool Initialize();
    EngineKind Kind();
    std::wstring LastError() const { return lastError_; }
    // 是否启用 DXGI（LUT）引擎。设置时会停止当前引擎并强制重新探测。
    bool UseDxgi() const { return useDxgi_; }
    void SetUseDxgi(bool useDxgi);

    // 对指定显示器应用滤镜（UI 线程调用）
    bool Apply(int displayIndex, const DisplayMonitor& display, const FilterSettings& s);
    bool Reset();
    void ResetDisplay(int displayIndex);
    void SetOverlayCapturable(bool capturable);
    void SetVsync(int displayIndex, bool useVsync);
    void Shutdown();

    // 自检（不创建覆盖层，验证 D3D11 设备与着色器可用）
    static std::wstring RunSelfTest();

private:
    FilterEngine() = default;
    bool EnsureMagReadyLocked();
    void StopAll();
    bool InitializeLocked(); // 调用方必须已持有 mutex_

    std::mutex mutex_;
    bool checked_ = false;
    bool useDxgi_ = false;
    EngineKind kind_ = EngineKind::None;
    std::wstring lastError_;
    MagEngine mag_;
    bool magUsed_ = false;

    // 每显示器一个 LUT 引擎
    std::map<int, std::unique_ptr<LutEngine>> lutEngines_;
    // 每显示器期望的垂直同步设置（引擎尚未创建时先记录，创建时套用）
    std::map<int, bool> vsyncByDisplay_;
    bool capturable_ = false;
    bool gammaSaturationWarned_ = false; // 伽马引擎下饱和度告警去重
};

} // namespace hsf
