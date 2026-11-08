// desktop_capture.h — 捕获层：DXGI Desktop Duplication → 固定格式工作纹理。
//
// 这里是渲染链路的「输入边界」：
//   * 只负责取帧、格式校验、全黑/异格式诊断、场景亮度采样；
//   * 不创建也不操作覆盖层窗口，不接触 LUT/着色器；
//   * 对外只暴露帧元数据、工作纹理 SRV、场景采样和错误状态。
#pragma once
#include "render_types.h"
#include <dxgi1_2.h>
#include <atomic>

namespace hsf {

class DesktopCapture
{
public:
    enum class Status
    {
        Frame,       // 拿到一帧（仍由调用方持有，必须 ReleaseFrame 或 CopyToWorkTexture）
        NoFrame,     // 桌面暂无新帧（DXGI_ERROR_WAIT_TIMEOUT）
        AccessLost,  // 显示模式/分辨率/色彩空间变化，需要 Recover()
        Error,       // 其它捕获错误，需要 Recover()
    };

    struct Frame
    {
        ComPtr<ID3D11Texture2D> texture;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
        bool desktopImageUpdated = false;
        bool pointerUpdated = false;
        bool newPointerShape = false;
        UINT accumulatedFrames = 1;
        bool rectsCoalesced = false;
        DWORD tick = 0;
        bool held = false;   // AcquireNextFrame 返回的帧尚未 Release
    };

    DesktopCapture() = default;
    ~DesktopCapture();

    bool Start(ID3D11Device* device, ID3D11DeviceContext* context,
               int x, int y, int width, int height, int outputIndex);
    void Stop();
    bool Ready() const { return duplication_.Get() != nullptr; }

    // 取一帧。成功时 frame.held=true；调用方必须二选一：
    //   ReleaseFrame(frame)       —— 丢弃这一帧
    //   CopyToWorkTexture(frame)  —— 拷进工作纹理并释放
    Status Poll(Frame& frame, UINT timeoutMs);

    bool IsForeign(const Frame& frame) const;
    void OnForeignFrame(const Frame& frame);

    // 标记「固定工作格式的帧已到达」——全黑/重复帧也算可用格式，
    // 供渲染循环判断「捕获持续只返回不可用格式」。
    void MarkUsableFrame();

    // 每秒最多一次：先读上一轮结果，再为当前帧发起本轮探测（全 GPU 队列）。
    SceneSample TakeSceneSample(const Frame& frame);

    bool CopyToWorkTexture(Frame& frame);
    void ReleaseFrame(Frame& frame);

    bool Recover();
    bool RecoverWithRetry(int attempts, int delayMs, const std::atomic<bool>* running);

    // 捕获故障后的恢复动作由协调层决定；这里只提供「拆掉重来」。
    ID3D11ShaderResourceView* WorkSrv() const { return frameSrv_.Get(); }
    ID3D11Texture2D* WorkTexture() const { return frameTexture_.Get(); }

    DWORD LastFrameTick() const { return lastFrameTick_; }
    DWORD LastUsableFrameTick() const { return lastUsableFrameTick_; }
    DXGI_FORMAT WorkFormat() const { return captureFormat_; }

    // 按桌面坐标（而非索引）查找 DXGI 输出：EnumDisplayMonitors 与 EnumOutputs
    // 的顺序不一定一致，多显示器时按索引会捕获到错误屏幕。
    static bool FindOutputForPoint(int x, int y, ComPtr<IDXGIAdapter>& adapter,
                                   ComPtr<IDXGIOutput>& output);
    static const wchar_t* FormatName(DXGI_FORMAT format);

    std::wstring LastError;

private:
    bool CreateCapture();
    bool FindOutput(ComPtr<IDXGIAdapter>& adapter, ComPtr<IDXGIOutput>& output);
    // 分辨率/格式变化后重建帧纹理，否则 CopyResource 失败 → 黑屏/花屏
    bool EnsureWorkTexture(ID3D11Texture2D* src);
    bool EnsureProbeStaging(DXGI_FORMAT format);
    void BeginSceneProbe(ID3D11Texture2D* tex, DXGI_FORMAT format);
    bool ReadProbeStaging(SceneSample& out, bool nonBlocking);
    // 同步采样：只给「异格式帧」这类每秒最多一次的诊断路径用。
    double SampleCaptureMaxLuma(ID3D11Texture2D* tex, DXGI_FORMAT format,
                                double* meanOut = nullptr);
    void ResetProbeState();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    int x_ = 0, y_ = 0;

    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIAdapter> adapter_;
    ComPtr<IDXGIOutput> output_;

    // 捕获/工作格式固定为 B8G8R8A8_UNORM（桌面复制的 SDR 画面，也是 LUT 的工作空间）。
    // 异格式帧一律丢弃：逐帧换解释方式会让画面在两套色彩映射间来回跳（全屏闪烁）。
    DXGI_FORMAT captureFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool foreignLogged_ = false;

    ComPtr<ID3D11Texture2D> frameTexture_;
    ComPtr<ID3D11ShaderResourceView> frameSrv_;

    // 场景探测（延迟一帧回读）
    ComPtr<ID3D11Texture2D> probeStaging_;
    DXGI_FORMAT probeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    bool probePending_ = false;
    DWORD lastSceneSampleTick_ = 0;

    // 全黑/异格式诊断按格式分别缓存
    struct FormatProbe { DWORD tick = 0; bool black = false; bool known = false; };
    FormatProbe formatProbe_[3];

    DWORD lastFrameTick_ = 0;
    DWORD lastUsableFrameTick_ = 0;
};

} // namespace hsf
