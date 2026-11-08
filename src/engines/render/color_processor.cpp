#include "color_processor.h"
#include "log.h"
#include <cmath>

namespace hsf {

namespace {
double SmoothStep01(double e0, double e1, double x)
{
    double t = (x - e0) / (e1 - e0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}
} // namespace

bool ColorProcessor::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                int width, int height)
{
    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;
    LastError.clear();
    if (!EnsureShaders()) return false;
    if (!CreateResources()) return false;
    return true;
}

void ColorProcessor::Shutdown()
{
    ReleaseResources();
    device_ = nullptr;
    context_ = nullptr;
}

void ColorProcessor::SetSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    {
        std::lock_guard<std::mutex> lock(paramsMutex_);
        params_[38] = width_ > 0 ? 1.0f / (float)width_ : 0.0f;
        params_[39] = height_ > 0 ? 1.0f / (float)height_ : 0.0f;
        paramsDirty_.store(true);
    }
}

bool ColorProcessor::EnsureShaders()
{
    // 顶点着色器
    ComPtr<ID3DBlob> vsBlob;
    if (!CompileShader(g_vsSource, "main", "vs_4_0", vsBlob, LastError)) return false;
    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                           nullptr, vs_.GetAddressOf())))
        return false;

    // 像素着色器（单遍合并：LUT 采样 + 可选线性光后处理）
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(g_psLutSource, "main", "ps_4_0", psBlob, LastError)) return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                          nullptr, ps_.GetAddressOf())))
        return false;

    // 像素着色器（中性直通）
    ComPtr<ID3DBlob> psPassBlob;
    if (!CompileShader(g_psPassthroughSource, "main", "ps_4_0", psPassBlob, LastError)) return false;
    if (FAILED(device_->CreatePixelShader(psPassBlob->GetBufferPointer(), psPassBlob->GetBufferSize(),
                                          nullptr, psPassthrough_.GetAddressOf())))
        return false;

    // 计算着色器（LUT 重建）
    ComPtr<ID3DBlob> csBlob;
    if (!CompileShader(g_csLutSource, "CSMain", "cs_5_0", csBlob, LastError)) return false;
    if (FAILED(device_->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
                                            nullptr, cs_.GetAddressOf())))
        return false;

    // 输入布局
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device_->CreateInputLayout(elements, 2, vsBlob->GetBufferPointer(),
                                          vsBlob->GetBufferSize(), inputLayout_.GetAddressOf())))
        return false;

    // 光栅化：禁用背面剔除（视口变换翻转 Y 后三角形呈逆时针，默认 Back 剔除会黑屏）
    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rsDesc, rasterizer_.GetAddressOf()))) return false;
    return true;
}

bool ColorProcessor::CreateResources()
{
    // 顶点缓冲：全屏三角形（UV 的 V 轴：v=0 顶部、v=1 底部）
    float vertices[] = {
        -1.0f, -1.0f, 0.5f, 1.0f,   0.0f, 1.0f,
         3.0f, -1.0f, 0.5f, 1.0f,   2.0f, 1.0f,
        -1.0f,  3.0f, 0.5f, 1.0f,   0.0f, -1.0f,
    };
    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = sizeof(vertices);
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{ vertices, 0, 0 };
    if (FAILED(device_->CreateBuffer(&vbDesc, &vbData, vertexBuffer_.GetAddressOf())))
        return false;

    // 常量缓冲（44 float = 176 字节）
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = kParamsFloatCount * sizeof(float);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&cbDesc, nullptr, paramsBuffer_.GetAddressOf())))
        return false;

    // 3D LUT（64^3，R16G16B16A16_FLOAT，SRV + UAV）
    D3D11_TEXTURE3D_DESC lutDesc{};
    lutDesc.Width = kLutSize;
    lutDesc.Height = kLutSize;
    lutDesc.Depth = kLutSize;
    lutDesc.MipLevels = 1;
    lutDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lutDesc.Usage = D3D11_USAGE_DEFAULT;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device_->CreateTexture3D(&lutDesc, nullptr, lutTexture_.GetAddressOf())))
        return false;
    if (FAILED(device_->CreateShaderResourceView(lutTexture_.Get(), nullptr,
                                                 lutSrv_.GetAddressOf())))
        return false;
    if (FAILED(device_->CreateUnorderedAccessView(lutTexture_.Get(), nullptr,
                                                  lutUav_.GetAddressOf())))
        return false;

    // 采样器：输入线性+clamp；LUT 三线性+clamp
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD = 0;
    if (FAILED(device_->CreateSamplerState(&sampDesc, inputSampler_.GetAddressOf())))
        return false;
    if (FAILED(device_->CreateSamplerState(&sampDesc, lutSampler_.GetAddressOf())))
        return false;

    // 初始 LUT 必须是确定内容（不是未初始化纹理）
    {
        std::lock_guard<std::mutex> lock(paramsMutex_);
        for (int i = 0; i < kParamsFloatCount; i++) params_[i] = 0.0f;
        params_[1] = params_[3] = 1.0f;
        neutral_.store(true);
        paramsDirty_.store(true);
    }
    return true;
}

void ColorProcessor::ReleaseResources()
{
    lutUav_.Reset();
    lutSrv_.Reset();
    lutTexture_.Reset();
    paramsBuffer_.Reset();
    vertexBuffer_.Reset();
    rasterizer_.Reset();
    inputLayout_.Reset();
    ps_.Reset();
    psPassthrough_.Reset();
    vs_.Reset();
    cs_.Reset();
    inputSampler_.Reset();
    lutSampler_.Reset();
}

void ColorProcessor::Apply(const FilterSettings& s)
{
    std::lock_guard<std::mutex> lock(paramsMutex_);
    float next[kParamsFloatCount] = {};
    float* p = next;
    // 与旧版一致的 0.75 调色强度系数：只减半「相对中性值」的偏差，不改用户保存值。
    constexpr double kAdjustStrength = 0.75;
    p[0] = (float)(s.Hue * kAdjustStrength);
    p[1] = (float)(1.0 + (s.HslSaturation / 100.0 - 1.0) * kAdjustStrength);
    p[2] = (float)(s.Lightness / 100.0 * kAdjustStrength);
    p[3] = (float)(1.0 + (s.Saturation / 100.0 - 1.0) * kAdjustStrength);
    p[4] = (float)(s.Temperature / 100.0);
    p[5] = (float)(s.Contrast / 100.0);
    p[6] = (float)(s.Brightness / 100.0 * 0.5);
    p[7] = (float)(s.Highlights / 100.0);
    p[8] = (float)(s.Shadows / 100.0);

    int idx = 9;
    for (int i = 0; i < HslChannelNames::ColorCount; i++)
    {
        const HslChannel* ch = s.FindChannel(HslChannelNames::ColorNames[i]);
        p[idx++] = ch == nullptr ? 0.0f : (float)(ch->Hue * kAdjustStrength);
        p[idx++] = ch == nullptr ? 1.0f : (float)(1.0 + (ch->Saturation / 100.0 - 1.0) * kAdjustStrength);
        p[idx++] = ch == nullptr ? 0.0f : (float)(ch->Lightness / 100.0 * kAdjustStrength);
    }
    for (; idx < kParamsFloatCount; idx++) p[idx] = 0.0f;
    p[33] = (float)(s.Sharpen / 100.0 * 0.75);
    p[34] = (float)(s.NoiseReduction / 100.0 * 0.65);
    p[35] = (float)(s.EdgeEnhancement / 100.0 * 0.75);
    p[36] = (float)(s.Clarity / 100.0 * 0.65);
    p[37] = (float)(s.QualityEnhancement / 100.0 * 0.65);
    p[38] = width_ > 0 ? 1.0f / (float)width_ : 0.0f;
    p[39] = height_ > 0 ? 1.0f / (float)height_ : 0.0f;
    p[40] = (p[33] == 0.0f && p[34] == 0.0f && p[35] == 0.0f &&
             p[36] == 0.0f && p[37] == 0.0f) ? 0.0f : 1.0f;
    // 槽 41 只写用户滑块值；槽 42（场景权重）由渲染线程在 DispatchLut 中合并。
    p[41] = (float)(s.DarkSceneBoost / 100.0);

    // 参数没变不要重新标脏：窗口/显示器/监听事件会反复 Apply 同一份参数。
    bool changed = memcmp(next, params_, sizeof(next)) != 0;
    if (changed) memcpy(params_, next, sizeof(next));
    neutral_.store(s.IsNeutral());
    if (changed) paramsDirty_.store(true);
}

void ColorProcessor::DispatchLut()
{
    paramsDirty_.store(false);
    lastLutRebuildTick_ = GetTickCount();
    {
        std::lock_guard<std::mutex> lock(paramsMutex_);
        memcpy(paramsCopy_, params_, sizeof(params_));
    }
    // 槽 42 是场景自适应权重，不属于界面下发参数；单一写入者，无需加锁。
    paramsCopy_[42] = (float)sceneWeight_;
    context_->UpdateSubresource(paramsBuffer_.Get(), 0, nullptr, paramsCopy_, 0, 0);

    // 即使当前是中性也生成一次：保证 3D LUT 纹理永远有确定内容。
    context_->CSSetShader(cs_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, paramsBuffer_.GetAddressOf());
    context_->CSSetUnorderedAccessViews(0, 1, lutUav_.GetAddressOf(), nullptr);
    context_->Dispatch(kLutSize / 4, kLutSize / 4, kLutSize / 4);
    ID3D11UnorderedAccessView* nullUav[] = { nullptr };
    context_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    context_->CSSetShader(nullptr, nullptr, 0);
}

bool ColorProcessor::Draw(ID3D11ShaderResourceView* input, ID3D11RenderTargetView* target,
                          bool* rebuilt)
{
    if (rebuilt) *rebuilt = false;
    if (!input || !target || !context_) return false;

    // 本帧的直通/LUT 选择只取一次：如果「重建 LUT」和「绘制」各读一次 neutral_，
    // 两次之间被 UI 线程改掉就会跳过 LUT 生成、却用 LUT 着色器绘制 —— 采样到上一代
    // （甚至未初始化）的 3D LUT，画面闪一下/颜色全错。这是旧实现修过的回归点。
    const bool neutralNow = neutral_.load();

    // 重建 LUT（拖动滑块时最快 ~33 次/秒；未到间隔先沿用上一代）
    if (paramsDirty_.load())
    {
        DWORD nowTick = GetTickCount();
        if (lastLutRebuildTick_ == 0 || nowTick - lastLutRebuildTick_ >= kLutRebuildMinIntervalMs)
        {
            DispatchLut();
            if (rebuilt) *rebuilt = true;
        }
    }

    // 全屏三角形覆盖全部像素，因此不需要 ClearRenderTargetView。
    context_->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)width_, (float)height_, 0, 1 };
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(rasterizer_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(inputLayout_.Get());
    UINT stride = 24, offset = 0;
    context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    context_->VSSetShader(vs_.Get(), nullptr, 0);

    if (neutralNow)
    {
        context_->PSSetShader(psPassthrough_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, &input);
        context_->PSSetSamplers(0, 1, inputSampler_.GetAddressOf());
        context_->Draw(3, 0);
        return true;
    }

    // 单遍合并：捕获帧 →（可选）线性光邻域后处理 → LUT 采样 → 后缓冲。
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, paramsBuffer_.GetAddressOf());
    ID3D11ShaderResourceView* srvs[] = { input, lutSrv_.Get() };
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 1, inputSampler_.GetAddressOf());
    context_->PSSetSamplers(1, 1, lutSampler_.GetAddressOf());
    context_->Draw(3, 0);
    return true;
}

bool ColorProcessor::UpdateSceneWeight(double mean, DWORD nowTick, bool startupStable)
{
    if (mean < 0.0) return false;

    // 越暗越强，但最低保留 50%：用户明确拖动的滑块不能被 5 块采样静默清零。
    double weight = kDarkSceneMinWeight +
                    (1.0 - kDarkSceneMinWeight) * (1.0 - SmoothStep01(0.08, 0.35, mean));

    bool sliderOn = false;
    {
        std::lock_guard<std::mutex> lock(paramsMutex_);
        sliderOn = params_[41] > 0.001f;
    }
    if (!sliderOn)
    {
        // 没开暗场景优化时清掉记录：用户一打开滑块，下一拍会立刻按当前场景重建一次。
        lastSceneWeight_ = -1.0;
        return false;
    }

    bool weightMoved = (lastSceneWeight_ < 0.0) || fabs(weight - lastSceneWeight_) > 0.03;
    if (!weightMoved) return false;
    // 启动后的前 2.5 秒先不因场景亮度改 LUT：捕获/flip 池/合成器都还在建立。
    if (!startupStable) return false;
    if (lastSceneRebuildTick_ != 0 && nowTick - lastSceneRebuildTick_ < 500)
        return false;

    lastSceneRebuildTick_ = nowTick;
    lastSceneWeight_ = weight;
    sceneWeight_ = weight;
    paramsDirty_.store(true);
    Log::WriteFmt(L"ColorProcessor",
                  L"暗场景优化：场景平均亮度=%.2f → 自适应权重=%.2f（越暗越强）",
                  mean, weight);
    return true;
}

} // namespace hsf
