// lut_engine.h — 渲染协调层：把「捕获 / 处理 / 呈现」三层串成每显示器一条渲染线程。
//
// 分层边界（新增功能时请守住）：
//   DesktopCapture  —— 只产出帧纹理 + 帧元数据 + 场景采样；
//   ColorProcessor  —— 只把 SRV 画到 RTV（LUT/后处理/暗场景权重都在这里）；
//   OverlayPresenter—— 只负责窗口、交换链、Present 与帧率 OSD。
// 本类不直接碰 D3D 纹理内容，只做生命周期、线程、自愈、统计和三层之间的编排。
#pragma once
#include "common.h"
#include "comptr.h"
#include "models.h"
#include "render/desktop_capture.h"
#include "render/color_processor.h"
#include "render/overlay_presenter.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>

namespace hsf {

// 查询坐标所在显示器当前是否处于 HDR（HDR10 PQ / scRGB）状态。
// 供 UI 判断「放大镜/伽马引擎在 HDR 下会过曝、文字发虚」并给出提示。
bool QueryDisplayHdr(int x, int y);

class LutEngine
{
public:
    LutEngine() = default;
    ~LutEngine();

    // 启动：创建覆盖层窗口、交换链、着色器、捕获。失败返回 false 并记录 LastError。
    bool Start(int x, int y, int width, int height, int outputIndex);
    void Dispose();

    // 更新滤镜参数（线程安全：写入参数缓冲，渲染线程在重建 LUT 时读取）
    void Apply(const FilterSettings& s);

    // 垂直同步开关（历史字段）：覆盖层固定 Present(1)，本字段只作记录。
    bool UseVsync = false;
    bool Capturable = false;      // 覆盖层可被 OBS 等捕获（WDA_MONITOR）
    bool IsRendering() const { return running_ && renderThread_.joinable(); }

    // 滤镜实时呈现帧率（界面右上角）。0 = 尚无样本。
    double PresentFps() const { return presentFps_.load(); }
    void SetFpsDetail(bool on);
    void SetOsdText(const std::wstring& text);

    std::wstring LastError;
    HWND Hwnd() const { return presenter_.Hwnd(); }

    void ApplyOverlayAffinity();                 // 立即应用 Capturable 的 WDA 标志
    void SetOverlayVisible(bool visible);        // 捕获不可用期间隐藏，避免冻结最后一帧
    bool OverlayVisible() const { return presenter_.IsVisible(); }
    bool CaptureOk() const { return captureOk_.load(); }

private:
    struct FrameTiming
    {
        double acquireWaitMs = 0, probeMs = 0, prepMs = 0, drawMs = 0, vsyncWaitMs = 0, iterMs = 0;
    };

    void RenderLoop();
    bool DrawAndPresent();
    bool RenderSelfCheck();
    bool CreateDevice(int x, int y);
    void NotePresentedFrame(const FrameTiming& t);
    void ResetStatsWindow();

    // D3D11 设备/上下文（三层共用，但只有协调层持有所有权）
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    DesktopCapture capture_;
    ColorProcessor processor_;
    OverlayPresenter presenter_;

    // 帧节奏统计
    DWORD lastPresentTick_ = 0;
    DWORD statsStartTick_ = 0;
    bool statsLogged_ = false;
    LARGE_INTEGER lastPresentQpc_{};
    double sumIntervalMs_ = 0, maxIntervalMs_ = 0, minIntervalMs_ = 0;
    double sumWorkMs_ = 0, maxWorkMs_ = 0;
    int intervalCount_ = 0, longGapCount_ = 0;
    int noFramePolls_ = 0;
    int lutRebuilds_ = 0;
    int probeNotReady_ = 0;
    double sumAcquireMs_ = 0, maxAcquireMs_ = 0;
    double sumProbeMs_ = 0, maxProbeMs_ = 0;
    double sumPrepMs_ = 0, maxPrepMs_ = 0;
    double sumDrawMs_ = 0, maxDrawMs_ = 0;
    double sumVsyncWaitMs_ = 0, maxVsyncWaitMs_ = 0;
    double sumIterMs_ = 0, maxIterMs_ = 0;
    double sumUnaccountedMs_ = 0, maxUnaccountedMs_ = 0;
    LARGE_INTEGER prevIterStartQpc_{};
    int dupFrames_ = 0, pointerOnlyFrames_ = 0, accumulatedFrames_ = 0, coalescedRects_ = 0;
    bool seenDesktopPresentTime_ = false;
    double lastVsyncWaitMs_ = 0;

    // 实时帧率（1 秒窗口；每 0.5 秒刷新显示）
    std::atomic<double> presentFps_{ 0.0 };
    std::atomic<bool> fpsDetail_{ false };
    LARGE_INTEGER fpsWindowStart_{};
    int fpsWindowCount_ = 0;

    // 启动保护期 / 统计预热期
    DWORD startTick_ = 0;
    DWORD statsWarmupUntil_ = 0;
    static constexpr DWORD kStartupGraceMs = 1500;
    static constexpr DWORD kStatsWarmupMs = 1000;
    // 覆盖层中间隐藏过（恢复后第一帧只重建基准）
    bool presentsSuppressed_ = false;
    bool selfChecked_ = false;

    // 全黑帧护栏
    int pinnedBlackStreak_ = 0;
    bool pausedForBlackFrames_ = false;
    bool pausedForUnusableFrames_ = false;
    static constexpr int kBlackStreakToPause = 3;
    static constexpr double kBlackLumaThreshold = 0.004;
    static constexpr DWORD kNoUsableFramePauseMs = 2500;

    // 线程与生命周期
    std::thread renderThread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> disposed_{ false };
    std::atomic<bool> captureOk_{ true };
};

} // namespace hsf
