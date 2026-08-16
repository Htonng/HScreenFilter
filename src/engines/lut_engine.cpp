#include "lut_engine.h"
#include "hlsl.h"
#include "log.h"
#include <dxgi.h>

namespace hsf {

static constexpr uint32_t kDxgiErrorWaitTimeout = 0x887A0027;
static constexpr uint32_t kDxgiErrorAccessLost = 0x887A0026;

// 调色强度系数（与旧版一致：只减半“相对中性值”的偏差，不改动用户保存的参数）
static constexpr double kAdjustStrength = 0.75;

LutEngine::~LutEngine()
{
    Dispose();
}

// ---------------- 参数 ----------------

void LutEngine::Apply(const FilterSettings& s)
{
    std::lock_guard<std::mutex> lock(paramsMutex_);
    float* p = params_;
    p[0] = (float)(s.Hue * kAdjustStrength);                                  // MasterHue
    p[1] = (float)(1.0 + (s.HslSaturation / 100.0 - 1.0) * kAdjustStrength);  // MasterSat
    p[2] = (float)(s.Lightness / 100.0 * kAdjustStrength);                    // MasterLight
    p[3] = (float)(1.0 + (s.Saturation / 100.0 - 1.0) * kAdjustStrength);     // GlobalSat
    p[4] = (float)(s.Temperature / 100.0);                                    // Temperature
    p[5] = (float)(s.Contrast / 100.0);                                       // Contrast
    p[6] = (float)(s.Brightness / 100.0 * 0.5);                               // Brightness
    p[7] = (float)(s.Highlights / 100.0);                                     // Highlights
    p[8] = (float)(s.Shadows / 100.0);                                        // Shadows

    int idx = 9;
    for (int i = 0; i < HslChannelNames::ColorCount; i++)
    {
        const HslChannel* ch = s.FindChannel(HslChannelNames::ColorNames[i]);
        p[idx++] = ch == nullptr ? 0.0f : (float)(ch->Hue * kAdjustStrength);
        p[idx++] = ch == nullptr ? 1.0f : (float)(1.0 + (ch->Saturation / 100.0 - 1.0) * kAdjustStrength);
        p[idx++] = ch == nullptr ? 0.0f : (float)(ch->Lightness / 100.0 * kAdjustStrength);
    }
    for (; idx < kParamsFloatCount; idx++) p[idx] = 0.0f;

    neutral_.store(s.IsNeutral());
    paramsDirty_.store(true);
}

// ---------------- 生命周期 ----------------

bool LutEngine::Start(int x, int y, int width, int height, int outputIndex)
{
    x_ = x; y_ = y; width_ = width; height_ = height; outputIndex_ = outputIndex;
    try
    {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr, context_.GetAddressOf());
        if (FAILED(hr))
        {
            // 无 GPU（虚拟机/远程桌面）→ 回退 WARP 软件光栅化
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr, context_.GetAddressOf());
        }
        if (FAILED(hr))
        {
            LastError = Format(L"D3D11CreateDevice 失败 (0x%08X)", (unsigned)hr);
            return false;
        }

        CreateOverlayWindow();
        if (!hwnd_) return false;
        CreateSwapChain();
        if (!swapChain_) return false;
        if (!CreatePipeline()) return false;
        CreateCapture();
        if (!duplication_) return false;

        // 初始参数（中性），渲染线程首帧会重建 LUT
        {
            std::lock_guard<std::mutex> lock(paramsMutex_);
            for (int i = 0; i < kParamsFloatCount; i++) params_[i] = 0.0f;
            params_[1] = params_[3] = 1.0f; // MasterSat / GlobalSat 中性
            neutral_.store(true);
            paramsDirty_.store(true);
        }

        running_ = true;
        renderThread_ = std::thread([this] { RenderLoop(); });
        Log::WriteFmt(L"LutEngine", L"Start OK: %dx%d @%d,%d output=%d", width, height, x, y, outputIndex);
        return true;
    }
    catch (...)
    {
        LastError = L"引擎启动异常";
        Log::Write(L"LutEngine", L"Start FAILED: 异常");
        Dispose();
        return false;
    }
}

// ---------------- 覆盖层窗口 ----------------

void LutEngine::CreateOverlayWindow()
{
    // 若已注册过窗口类（反复开关），忽略 ERROR_CLASS_ALREADY_EXISTS
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOverlayWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LastError = Format(L"RegisterClassExW 失败 (0x%08X)", (unsigned)GetLastError());
        return;
    }

    // 点击穿透必须 WS_EX_LAYERED | WS_EX_TRANSPARENT；
    // 不能加 WS_EX_TOOLWINDOW（OBS 窗口捕获会过滤工具窗口）
    DWORD exStyle = WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    DWORD style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS;

    hwnd_ = CreateWindowExW(exStyle, kOverlayWindowClass, L"HScreenFilter 滤镜层",
                            style, x_, y_, width_, height_, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_)
    {
        LastError = Format(L"CreateWindowExW 失败 (0x%08X)", (unsigned)GetLastError());
        return;
    }

    ApplyOverlayAffinity();

    // 置顶必须改 WS_EX_TOPMOST 样式位（对分层窗口仅 SetWindowPos 无效）
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex |= kWsExTopmost;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void LutEngine::ApplyOverlayAffinity()
{
    if (!hwnd_) return;
    SetWindowDisplayAffinity(hwnd_, Capturable ? WDA_MONITOR : WDA_EXCLUDEFROMCAPTURE);
}

LRESULT CALLBACK LutEngine::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 点击穿透
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 交换链 ----------------

void LutEngine::CreateSwapChain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())))) return;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) return;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = (UINT)width_;
    desc.Height = (UINT)height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;

    HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                                 swapChain_.GetAddressOf());
    if (FAILED(hr))
    {
        LastError = Format(L"CreateSwapChainForHwnd 失败 (0x%08X)", (unsigned)hr);
        return;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
}

// ---------------- 渲染管线 ----------------

bool LutEngine::CreatePipeline()
{
    // 顶点着色器
    ComPtr<ID3DBlob> vsBlob;
    if (!CompileShader(g_vsSource, "main", "vs_4_0", vsBlob, LastError)) return false;
    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                           vs_.GetAddressOf())))
        return false;

    // 像素着色器（LUT 采样）
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(g_psLutSource, "main", "ps_4_0", psBlob, LastError)) return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                          ps_.GetAddressOf())))
        return false;

    // 计算着色器（LUT 重建）
    ComPtr<ID3DBlob> csBlob;
    if (!CompileShader(g_csLutSource, "CSMain", "cs_5_0", csBlob, LastError)) return false;
    if (FAILED(device_->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr,
                                            cs_.GetAddressOf())))
        return false;

    // 输入布局
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device_->CreateInputLayout(elements, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                          inputLayout_.GetAddressOf())))
        return false;

    // 光栅化：禁用背面剔除（视口变换翻转 Y 后三角形呈逆时针，默认 Back 剔除会黑屏）
    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rsDesc, rasterizer_.GetAddressOf()))) return false;

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
    if (FAILED(device_->CreateBuffer(&vbDesc, &vbData, vertexBuffer_.GetAddressOf()))) return false;

    // 常量缓冲（40 float = 160 字节）
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = kParamsFloatCount * sizeof(float);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&cbDesc, nullptr, paramsBuffer_.GetAddressOf()))) return false;

    // 帧纹理（GPU 内拷贝目标）+ SRV
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = (UINT)width_;
    texDesc.Height = (UINT)height_;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&texDesc, nullptr, frameTexture_.GetAddressOf()))) return false;
    if (FAILED(device_->CreateShaderResourceView(frameTexture_.Get(), nullptr, frameSrv_.GetAddressOf()))) return false;

    // 3D LUT（64^3，R16G16B16A16_FLOAT，SRV + UAV）
    D3D11_TEXTURE3D_DESC lutDesc{};
    lutDesc.Width = kLutSize;
    lutDesc.Height = kLutSize;
    lutDesc.Depth = kLutSize;
    lutDesc.MipLevels = 1;
    lutDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lutDesc.Usage = D3D11_USAGE_DEFAULT;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device_->CreateTexture3D(&lutDesc, nullptr, lutTexture_.GetAddressOf()))) return false;
    if (FAILED(device_->CreateShaderResourceView(lutTexture_.Get(), nullptr, lutSrv_.GetAddressOf()))) return false;
    if (FAILED(device_->CreateUnorderedAccessView(lutTexture_.Get(), nullptr, lutUav_.GetAddressOf()))) return false;

    // 采样器：输入线性+clamp；LUT 三线性+clamp
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD = 0;
    if (FAILED(device_->CreateSamplerState(&sampDesc, inputSampler_.GetAddressOf()))) return false;
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(device_->CreateSamplerState(&sampDesc, lutSampler_.GetAddressOf()))) return false;

    return true;
}

// ---------------- 捕获 ----------------

void LutEngine::CreateCapture()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())))) return;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) return;
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs((UINT)outputIndex_, output.GetAddressOf())) || !output)
    {
        LastError = Format(L"EnumOutputs(%d) 失败", outputIndex_);
        return;
    }
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(output1.GetAddressOf())))) return;
    HRESULT hr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
    if (FAILED(hr))
    {
        LastError = Format(L"DuplicateOutput 失败 (0x%08X)", (unsigned)hr);
        duplication_.Reset();
    }
}

// ---------------- 渲染循环 ----------------

void LutEngine::RenderLoop()
{
    int frameCount = 0;
    while (running_)
    {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(100, &frameInfo, resource.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            if (resource)
            {
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(tex.GetAddressOf()))))
                {
                    // GPU 内拷贝：捕获帧 → 渲染纹理（无 CPU 往返）
                    context_->CopyResource(frameTexture_.Get(), tex.Get());
                }
            }
            duplication_->ReleaseFrame();
            DrawAndPresent();
            if (++frameCount == 30) RenderSelfCheck();
        }
        else if (hr == kDxgiErrorWaitTimeout)
        {
            continue; // 桌面无变化
        }
        else if (hr == kDxgiErrorAccessLost)
        {
            // 桌面模式/分辨率变化：重建捕获后继续
            if (!RecoverCapture()) break;
        }
        else
        {
            LastError = Format(L"DXGI 捕获错误: 0x%08X", (unsigned)hr);
            Log::WriteFmt(L"LutEngine", L"RenderLoop exit: %s", LastError.c_str());
            break;
        }
    }
    running_ = false;
    Log::Write(L"LutEngine", L"RenderLoop ended");
}

bool LutEngine::RecoverCapture()
{
    duplication_.Reset();
    CreateCapture();
    if (!duplication_)
    {
        Log::WriteFmt(L"LutEngine", L"RecoverCapture FAILED: %s", LastError.c_str());
        return false;
    }
    LastError.clear();
    Log::Write(L"LutEngine", L"RecoverCapture OK");
    return true;
}

void LutEngine::DrawAndPresent()
{
    // 1) 需要重建 LUT？把最新参数拷出并上传，派发计算着色器
    if (paramsDirty_.exchange(false))
    {
        {
            std::lock_guard<std::mutex> lock(paramsMutex_);
            memcpy(paramsCopy_, params_, sizeof(params_));
        }
        context_->UpdateSubresource(paramsBuffer_.Get(), 0, nullptr, paramsCopy_, 0, 0);

        if (!neutral_.load())
        {
            context_->CSSetShader(cs_.Get(), nullptr, 0);
            context_->CSSetConstantBuffers(0, 1, paramsBuffer_.GetAddressOf());
            context_->CSSetUnorderedAccessViews(0, 1, lutUav_.GetAddressOf(), nullptr);
            context_->Dispatch(kLutSize / 4, kLutSize / 4, kLutSize / 4);
            // 解绑 UAV，避免与后续 SRV 绑定冲突
            ID3D11UnorderedAccessView* nullUav[] = { nullptr };
            context_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            context_->CSSetShader(nullptr, nullptr, 0);
        }
    }

    // 2) 绘制（flip 模型交换链：缓冲在 Present 后轮转，必须每帧重新获取后缓冲，
    //    否则缓存的 RTV 指向已释放表面 → d3d11.dll 访问违例）
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return;
    backBufferTex_ = backBuffer;
    rtv_.Reset();
    if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv_.GetAddressOf())))
        return;

    if (neutral_.load())
    {
        // 中性直通：不采样 LUT，直接拷贝捕获帧
        context_->CopyResource(backBufferTex_.Get(), frameTexture_.Get());
    }
    else
    {
        const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
        context_->ClearRenderTargetView(rtv_.Get(), clearColor);
        context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp{ 0, 0, (float)width_, (float)height_, 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(rasterizer_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(inputLayout_.Get());
        UINT stride = 24, offset = 0;
        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, frameSrv_.GetAddressOf());
        context_->PSSetShaderResources(1, 1, lutSrv_.GetAddressOf());
        context_->PSSetSamplers(0, 1, inputSampler_.GetAddressOf());
        context_->PSSetSamplers(1, 1, lutSampler_.GetAddressOf());
        context_->Draw(3, 0);
    }

    UINT syncInterval = UseVsync ? 1u : 0u;
    swapChain_->Present(syncInterval, 0);
}

void LutEngine::RenderSelfCheck()
{
    // 从 back buffer 读回中心像素，黑屏时记录日志
    try
    {
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC desc{};
        backBufferTex_->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return;
        context_->CopyResource(staging.Get(), backBufferTex_.Get());
        context_->Flush();
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
        int cx = width_ / 2, cy = height_ / 2;
        const BYTE* px = (const BYTE*)map.pData + (size_t)cy * map.RowPitch + (size_t)cx * 4;
        BYTE b = px[0], g = px[1], r = px[2], a = px[3];
        context_->Unmap(staging.Get(), 0);
        bool visible = (r | g | b) > 8;
        Log::WriteFmt(L"LutEngine", L"渲染自检: 中心像素 R=%u G=%u B=%u A=%u -> %s",
                      r, g, b, a, visible ? L"画面正常" : L"画面全黑!");
    }
    catch (...)
    {
    }
}

// ---------------- 销毁 ----------------

void LutEngine::ReleaseAll()
{
    duplication_.Reset();
    lutUav_.Reset();
    lutSrv_.Reset();
    lutTexture_.Reset();
    frameSrv_.Reset();
    frameTexture_.Reset();
    paramsBuffer_.Reset();
    vertexBuffer_.Reset();
    rasterizer_.Reset();
    inputLayout_.Reset();
    ps_.Reset();
    vs_.Reset();
    cs_.Reset();
    inputSampler_.Reset();
    lutSampler_.Reset();
    rtv_.Reset();
    backBufferTex_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

void LutEngine::Dispose()
{
    if (disposed_.exchange(true)) return;
    running_ = false;
    Log::Write(L"LutEngine", L"Dispose begin");
    if (renderThread_.joinable())
    {
        renderThread_.join();
    }
    ReleaseAll();
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    Log::Write(L"LutEngine", L"Dispose end");
}

} // namespace hsf
