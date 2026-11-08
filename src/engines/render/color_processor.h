// color_processor.h — 处理层：FilterSettings → 64³ LUT → 单遍像素着色器。
//
// 这里是渲染链路的「颜色边界」：
//   * 输入只有一张 SRV（工作纹理），输出只有一个 RTV（后缓冲）；
//   * 不创建窗口/交换链，不知道帧从哪来、怎么呈现；
//   * 负责参数上传、LUT 重建限速、中性直通和暗场景自适应权重。
#pragma once
#include "render_types.h"
#include "models.h"
#include "../hlsl.h"

namespace hsf {

class ColorProcessor
{
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    int width, int height);
    void Shutdown();
    void SetSize(int width, int height);

    // UI 线程调用：只更新待处理参数（线程安全），真正的 LUT 重建在渲染线程里做。
    void Apply(const FilterSettings& s);

    // 渲染线程调用：重建 LUT（如需）并把 input 画到 target。
    // rebuilt 返回本帧是否真的跑了 64³ LUT 重建（用于帧率明细统计）。
    bool Draw(ID3D11ShaderResourceView* input, ID3D11RenderTargetView* target,
              bool* rebuilt);

    bool HasPendingParams() const { return paramsDirty_.load(); }
    bool IsNeutral() const { return neutral_.load(); }

    // 每秒一次的场景采样进入：越暗保留越强的暗场景优化权重。
    // 返回 true 表示权重变化已请求一次 LUT 重建（已记日志）。
    bool UpdateSceneWeight(double mean, DWORD nowTick, bool startupStable);

    std::wstring LastError;

private:
    bool EnsureShaders();
    bool CreateResources();
    void ReleaseResources();
    void DispatchLut();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11PixelShader> psPassthrough_;
    ComPtr<ID3D11ComputeShader> cs_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> paramsBuffer_;
    ComPtr<ID3D11Texture3D> lutTexture_;
    ComPtr<ID3D11ShaderResourceView> lutSrv_;
    ComPtr<ID3D11UnorderedAccessView> lutUav_;
    ComPtr<ID3D11SamplerState> inputSampler_;
    ComPtr<ID3D11SamplerState> lutSampler_;

    std::mutex paramsMutex_;
    float params_[kParamsFloatCount] = {};
    float paramsCopy_[kParamsFloatCount] = {};
    std::atomic<bool> paramsDirty_{ false };
    std::atomic<bool> neutral_{ true };

    // 暗场景优化的场景自适应权重（0.5..1）。只由渲染线程写、渲染线程读。
    double sceneWeight_ = 1.0;
    double lastSceneWeight_ = -1.0;
    DWORD lastSceneRebuildTick_ = 0;

    // LUT 重建最小间隔：拖动滑块时参数每帧都变，重建 64³ 计算着色器很贵。
    static constexpr DWORD kLutRebuildMinIntervalMs = 30;
    DWORD lastLutRebuildTick_ = 0;

    // 用户滑块永远至少保留这个比例（场景采样只有 5 块 16×16，不足以否决用户意图）。
    static constexpr double kDarkSceneMinWeight = 0.5;
};

} // namespace hsf
