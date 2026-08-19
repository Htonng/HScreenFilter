// lut_engine.h — D3D11 覆盖层 + DXGI Desktop Duplication 捕获 + 3D LUT 滤镜引擎。
//
// 与旧版 VorticeHslEngine 的区别（需求 3：提高 HSL 分色系调整整体效率 / 换实现思路）：
//   旧版：每像素每帧执行 完整 RGB→HSL 掩码 + RGB→OKLCH + 调色 + OKLCH→RGB + 基础调节
//         （每像素约 10+ 个超越函数：pow/atan2/sin/cos）。
//   新版：参数变化时用 计算着色器 对 64^3 个格子点各算一次（GPU 并行，亚毫秒级），
//         生成 3D LUT；每帧像素着色器只做一次三线性采样 + 屏幕空间抖动。
//         中性参数时直接 CopyResource 直通，连采样都省掉。
#pragma once
#include "common.h"
#include "comptr.h"
#include "models.h"
#include "hlsl.h"
#include <d3d11.h>
#include <dxgi1_2.h>

namespace hsf {

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

    bool UseVsync = false;        // Present(1) 防撕裂
    bool Capturable = false;      // 覆盖层可被 OBS 等捕获（WDA_MONITOR）
    bool IsRendering() const { return running_ && renderThread_.joinable(); }

    std::wstring LastError;
    HWND Hwnd() const { return hwnd_; }

    // 立即把 Capturable 应用到覆盖层（WDA 标志改变后 DWM 才按新设置合成；
    // 仅改标志位不会生效）
    void ApplyOverlayAffinity();

private:
    void RenderLoop();
    bool RecoverCapture();
    void DrawAndPresent();
    void RebuildLutIfNeeded();
    void RenderSelfCheck();
    void CreateOverlayWindow();
    void CreateSwapChain();
    bool CreatePipeline();
    bool CreateDevice();
    // 按显示器坐标（而非索引）查找 DXGI 输出：EnumDisplayMonitors 顺序与
    // EnumOutputs 顺序不一定一致，多显示器时按索引可能捕获到错误的屏幕
    bool FindOutput(ComPtr<IDXGIAdapter>& adapter, ComPtr<IDXGIOutput>& output);
    void CreateCapture();
    // 取得当前后缓冲对应的 RTV。flip 模型交换链的后缓冲每帧在多个缓冲间轮转，
    // 单槽缓存会退化为每帧重建 RTV（4060 等卡顿的元凶之一），这里按缓冲身份缓存。
    ID3D11RenderTargetView* GetBackBufferRtv(ID3D11Texture2D* buffer);
    // 分辨率变化后重建帧纹理/交换链（防止 CopyResource 失败 → 黑屏/花屏）
    void EnsureFrameTexture(const ComPtr<ID3D11Texture2D>& src);
    void EnsureSwapChainSize(UINT w, UINT h);
    bool EnsureShaders();
    void ReleaseAll();

    // 覆盖层窗口
    HWND hwnd_ = nullptr;
    int x_ = 0, y_ = 0, width_ = 0, height_ = 0;
    int outputIndex_ = 0;

    // D3D11
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    // 后缓冲 RTV 缓存（按缓冲身份缓存；BufferCount=2 时只用前 2 槽）
    struct BackBufferRtv { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; };
    BackBufferRtv backBufferRtv_[3];
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11ComputeShader> cs_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> paramsBuffer_;
    ComPtr<ID3D11Texture2D> frameTexture_;
    ComPtr<ID3D11ShaderResourceView> frameSrv_;
    ComPtr<ID3D11Texture2D> backBufferTex_;   // 当前后缓冲（渲染自检读回用）
    ComPtr<ID3D11Texture3D> lutTexture_;
    ComPtr<ID3D11ShaderResourceView> lutSrv_;
    ComPtr<ID3D11UnorderedAccessView> lutUav_;
    ComPtr<ID3D11SamplerState> inputSampler_;   // 线性
    ComPtr<ID3D11SamplerState> lutSampler_;     // 三线性 + clamp
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIAdapter> adapter_;
    ComPtr<IDXGIOutput> output_;

    // 自检只做一次（原来每 30 帧整屏读回 → GPU 停顿 → 卡顿）
    bool selfChecked_ = false;

    // 线程与状态
    std::thread renderThread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> disposed_{ false };
    std::mutex paramsMutex_;
    float params_[kParamsFloatCount] = {};
    float paramsCopy_[kParamsFloatCount] = {};
    std::atomic<bool> paramsDirty_{ false };
    std::atomic<bool> neutral_{ true };

    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace hsf
