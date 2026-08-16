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

private:
    void RenderLoop();
    bool RecoverCapture();
    void DrawAndPresent();
    void RebuildLutIfNeeded();
    void RenderSelfCheck();
    void CreateOverlayWindow();
    void CreateSwapChain();
    bool CreatePipeline();
    void CreateCapture();
    void ApplyOverlayAffinity();
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
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11ComputeShader> cs_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> paramsBuffer_;
    ComPtr<ID3D11Texture2D> frameTexture_;
    ComPtr<ID3D11ShaderResourceView> frameSrv_;
    ComPtr<ID3D11Texture2D> backBufferTex_;
    ComPtr<ID3D11Texture3D> lutTexture_;
    ComPtr<ID3D11ShaderResourceView> lutSrv_;
    ComPtr<ID3D11UnorderedAccessView> lutUav_;
    ComPtr<ID3D11SamplerState> inputSampler_;   // 线性
    ComPtr<ID3D11SamplerState> lutSampler_;     // 三线性 + clamp
    ComPtr<IDXGIOutputDuplication> duplication_;

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
