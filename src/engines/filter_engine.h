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

    // 引擎看门狗（UI 线程定期调用，建议每 5 秒一次）：
    //  - LUT 引擎渲染线程已停止（长时间捕获不可用、驱动重置、显示器重插等）→ 自动重建；
    //  - 伽马曲线引擎：任何显示模式切换都会由驱动把伽马表重置为线性 → 检测到不一致自动重设。
    // 返回 true 表示本次做了恢复动作（已记日志）。
    bool Watchdog();

    // 覆盖层是否正处于"暂停显示"状态（捕获不可用：游戏独占全屏、显示模式切换中）。
    // 用于界面提示"滤镜暂时没生效是因为拿不到画面"，而不是让用户以为滤镜坏了。
    bool OverlayPaused();

    // 临时显示/隐藏滤镜覆盖层（不停止引擎）。隐藏期间引擎不呈现，
    // 恢复显示后自动继续（渲染循环会重新呈现最新画面）。
    void SetOverlayVisible(bool visible);

    // 自检（不创建覆盖层，验证 D3D11 设备与着色器可用）
    static std::wstring RunSelfTest();

private:
    FilterEngine() = default;
    bool EnsureMagReadyLocked();
    void StopAll();
    bool InitializeLocked(); // 调用方必须已持有 mutex_

    // 每个显示器最后一次成功应用的参数（看门狗重建引擎时复用）
    struct AppliedState
    {
        DisplayMonitor Display;
        FilterSettings Settings;
    };

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
    // 每显示器最后一次应用的参数（看门狗用）
    std::map<int, AppliedState> appliedByDisplay_;
    FilterSettings lastGammaSettings_;   // 伽马引擎最后应用的参数（看门狗比对用）
    bool gammaApplied_ = false;
    bool capturable_ = false;
    bool gammaSaturationWarned_ = false; // 伽马引擎下饱和度告警去重
};

} // namespace hsf
