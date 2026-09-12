#include "lut_engine.h"
#include "hlsl.h"
#include "log.h"
#include <dxgi.h>
#include <dxgi1_6.h>

namespace hsf {

static constexpr uint32_t kDxgiErrorWaitTimeout = 0x887A0027;
static constexpr uint32_t kDxgiErrorAccessLost = 0x887A0026;
static constexpr uint32_t kDxgiErrorNotCurrentlyAvailable = 0x887A0021;

// 调色强度系数（与旧版一致：只减半“相对中性值”的偏差，不改动用户保存的参数）
static constexpr double kAdjustStrength = 0.75;

LutEngine::~LutEngine()
{
    Dispose();
}

// ---------------- 参数 ----------------

// 诊断跟踪：设置环境变量 HSF_TRACE=1 后，渲染循环会在关键阶段打点，
// 用于定位"卡在捕获 / 卡在 Present / 卡在恢复"这类问题（默认关闭，零开销）。
static bool TraceEnabled()
{
    static int cached = -1;
    if (cached < 0)
    {
        wchar_t buf[8] = {};
        cached = (GetEnvironmentVariableW(L"HSF_TRACE", buf, 8) > 0 && buf[0] != L'0') ? 1 : 0;
    }
    return cached == 1;
}

static void Trace(const wchar_t* stage)
{
    if (TraceEnabled()) Log::WriteFmt(L"LutEngine", L"[trace] %s", stage);
}

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
    p[33] = (float)(s.Sharpen / 100.0 * 0.75);                                // Sharpen
    p[34] = (float)(s.NoiseReduction / 100.0 * 0.65);                         // NoiseReduction
    p[35] = (float)(s.EdgeEnhancement / 100.0 * 0.75);                         // EdgeEnhancement
    p[36] = (float)(s.Clarity / 100.0 * 0.65);                                // Clarity
    p[37] = (float)(s.QualityEnhancement / 100.0 * 0.65);                     // QualityEnhancement
    p[38] = width_ > 0 ? 1.0f / (float)width_ : 0.0f;                         // TexelX
    p[39] = height_ > 0 ? 1.0f / (float)height_ : 0.0f;                       // TexelY
    // PostActive：五项后处理参数全为 0 时走单遍着色器的快路径（只做 1 次输入采样）
    p[40] = (p[33] == 0.0f && p[34] == 0.0f && p[35] == 0.0f &&
             p[36] == 0.0f && p[37] == 0.0f) ? 0.0f : 1.0f;

    neutral_.store(s.IsNeutral());
    paramsDirty_.store(true);
}

// ---------------- 生命周期 ----------------

bool LutEngine::Start(int x, int y, int width, int height, int outputIndex)
{
    x_ = x; y_ = y; width_ = width; height_ = height; outputIndex_ = outputIndex;
    try
    {
        if (!CreateDevice()) return false;

        // 探测输出色彩空间并选择匹配的交换链格式（SDR / HDR10 PQ / scRGB）
        DetectColorSpace();
        Log::WriteFmt(L"LutEngine", L"输出色彩空间模式=%d (0 SDR / 1 HDR10 / 2 scRGB)", colorMode_);

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
    // 注意：WS_VISIBLE 必须在创建时就有 —— flip 模型交换链首次 Present 到从未
    // 显示过的隐藏窗口会永久阻塞（公开测试发现的卡死根因）。分层窗口 alpha 必须
    // 显式初始化（否则部分驱动/显卡会花屏或黑屏）。
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

    // 分层窗口必须显式初始化 alpha（不调用的话 DWM 对覆盖层的合成状态未定义，
    // 部分驱动/显卡会显示花屏或黑屏）
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);

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
    Log::WriteFmt(L"LutEngine", L"覆盖层捕获亲和性: %s (Capturable=%d)",
                  Capturable ? L"WDA_MONITOR 可被第三方录制"
                             : L"WDA_EXCLUDEFROMCAPTURE 第三方录制看不到本层/部分录制工具会失败",
                  Capturable ? 1 : 0);
}

// 显示/隐藏覆盖层。捕获不可用（游戏独占全屏、显示模式切换中）时必须隐藏，
// 否则屏幕上会冻结最后一帧 —— 表现为"画面不动/屏幕变暗"且滤镜已失效。
// 注意历史坑（v2.0.0-beta 卡死根因）：flip 模型交换链向"未显示的窗口"Present 会永久
// 阻塞。因此：隐藏期间绝不 Present（见 DrawAndPresent 的可见性护栏），显示后先确认
// 窗口真的可见再继续渲染。
void LutEngine::SetOverlayVisible(bool visible)
{
    if (!hwnd_) return;
    if (overlayVisible_.exchange(visible) == visible) return;
    // 覆盖层窗口属于"调用 Start 的线程"（通常是 UI 线程），而本函数在渲染线程里调用：
    // 必须用异步窗口操作（*Async / SWP_ASYNCWINDOWPOS），否则一旦 UI 线程正阻塞在
    // join()/模态循环里，这里的同步 SendMessage 会与它互相死等。
    if (visible)
    {
        ShowWindowAsync(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, x_, y_, width_, height_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
        // 等窗口真正可见（异步操作由 UI 线程执行；有界等待，避免与 join 死锁）
        for (int i = 0; i < 50 && running_; i++)
        {
            if (IsWindowVisible(hwnd_)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!IsWindowVisible(hwnd_))
            Log::Write(L"LutEngine", L"覆盖层显示尚未生效，本帧跳过呈现");
    }
    else
    {
        ShowWindowAsync(hwnd_, SW_HIDE);
    }
    Trace(visible ? L"覆盖层显示" : L"覆盖层隐藏");
    Log::WriteFmt(L"LutEngine", L"覆盖层%s", visible ? L"已显示" : L"已隐藏（等待捕获恢复）");
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
    desc.Format = swapChainFormat_;
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
    if (FAILED(hr) && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
    {
        // 部分驱动/显卡不支持分层窗口 + flip 模型（黑屏/花屏的常见原因）→ 回退传统 DISCARD
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                             swapChain_.GetAddressOf());
        if (SUCCEEDED(hr))
            Log::Write(L"LutEngine", L"交换链回退为 DISCARD（分层窗口 + flip 不可用）");
    }
    if (FAILED(hr))
    {
        LastError = Format(L"CreateSwapChainForHwnd 失败 (0x%08X)", (unsigned)hr);
        return;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    // 声明交换链色彩空间（HDR10 PQ 或 scRGB），DWM 据此正确合成
    ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(swapChain_->QueryInterface(IID_PPV_ARGS(sc3.GetAddressOf()))))
        sc3->SetColorSpace1(colorSpace_);
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

    // 像素着色器（单遍合并：LUT 采样 + 可选线性光后处理）
    ComPtr<ID3DBlob> psBlob;
    if (!CompileShader(g_psLutSource, "main", "ps_4_0", psBlob, LastError)) return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                          ps_.GetAddressOf())))
        return false;

    // 像素着色器（中性直通）
    ComPtr<ID3DBlob> psPassBlob;
    if (!CompileShader(g_psPassthroughSource, "main", "ps_4_0", psPassBlob, LastError)) return false;
    if (FAILED(device_->CreatePixelShader(psPassBlob->GetBufferPointer(), psPassBlob->GetBufferSize(), nullptr,
                                          psPassthrough_.GetAddressOf())))
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

    // 像素着色器色彩空间常量缓冲（b1：输入/输出模式 + padding）
    D3D11_BUFFER_DESC modeDesc{};
    modeDesc.ByteWidth = 16;
    modeDesc.Usage = D3D11_USAGE_DEFAULT;
    modeDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&modeDesc, nullptr, psModeBuffer_.GetAddressOf()))) return false;
    UpdateColorModeBuffer();

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

bool LutEngine::FindOutput(ComPtr<IDXGIAdapter>& adapter, ComPtr<IDXGIOutput>& output)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return false;
    for (UINT ai = 0;; ai++)
    {
        ComPtr<IDXGIAdapter> a;
        if (factory->EnumAdapters(ai, a.GetAddressOf()) != S_OK) break;
        for (UINT oi = 0;; oi++)
        {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(oi, o.GetAddressOf()) != S_OK) break;
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(o->GetDesc(&d)) &&
                x_ >= d.DesktopCoordinates.left && x_ < d.DesktopCoordinates.right &&
                y_ >= d.DesktopCoordinates.top && y_ < d.DesktopCoordinates.bottom)
            {
                adapter = a;
                output = o;
                return true;
            }
        }
    }
    return false;
}

void LutEngine::DetectColorSpace()
{
    colorMode_ = 0;
    swapChainFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    colorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    if (!FindOutput(adapter, output)) return;
    ComPtr<IDXGIOutput6> o6;
    DXGI_OUTPUT_DESC1 desc1{};
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(o6.GetAddressOf()))) ||
        FAILED(o6->GetDesc1(&desc1)))
        return;

    colorSpace_ = desc1.ColorSpace;
    if (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
    {
        colorMode_ = 1;
        swapChainFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    else if (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
    {
        colorMode_ = 2;
        swapChainFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
}

void LutEngine::UpdateColorModeBuffer()
{
    if (!psModeBuffer_ || !context_) return;
    struct { uint32_t inputMode; uint32_t outputMode; float pad[2]; } data =
    { (uint32_t)inputMode_, (uint32_t)colorMode_, { 0.0f, 0.0f } };
    context_->UpdateSubresource(psModeBuffer_.Get(), 0, nullptr, &data, 0, 0);
}

bool LutEngine::CreateDevice()
{
    // 笔记本混合显卡（Optimus/双卡）：默认适配器可能是核显或错误的那块 GPU，
    // 导致 DuplicateOutput 失败 → 引擎回退伽马 → 鲜艳度/HSL 全部失效。
    // 先找到真正驱动目标显示器的适配器来创建设备。
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    if (FindOutput(adapter, output))
    {
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                                       context_.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            adapter_ = adapter;
            output_ = output;
            return true;
        }
        device_.Reset();
        context_.Reset();
    }

    // 回退：默认硬件适配器 → WARP 软件光栅化（虚拟机/远程桌面）
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                                   context_.GetAddressOf());
    if (SUCCEEDED(hr)) return true;
    device_.Reset();
    context_.Reset();
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                           context_.GetAddressOf());
    if (FAILED(hr))
    {
        LastError = Format(L"D3D11CreateDevice 失败 (0x%08X)", (unsigned)hr);
        return false;
    }
    return true;
}

void LutEngine::CreateCapture()
{
    // 按坐标匹配输出而不是按索引：EnumDisplayMonitors 与 EnumOutputs 的顺序
    // 不一定一致，多显示器时按索引可能捕获到错误的屏幕（导致滤镜作用错屏）。
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    if (!FindOutput(adapter, output))
    {
        LastError = Format(L"未找到坐标 (%d,%d) 对应的 DXGI 输出", x_, y_);
        return;
    }
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(output1.GetAddressOf()))))
    {
        LastError = L"IDXGIOutput1 不可用";
        return;
    }
    // 固定捕获格式为 B8G8R8A8，避免 HDR 下 DuplicateOutput 在 scRGB float 与 BGRA8
    // 之间逐帧切换（导致黑条闪烁与色彩抖动）。DuplicateOutput1 支持指定格式。
    HRESULT hr = E_FAIL;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output5.GetAddressOf()))))
    {
        DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = output5->DuplicateOutput1(device_.Get(), 0, 1, &fmt, duplication_.GetAddressOf());
        if (SUCCEEDED(hr))
            Log::Write(L"LutEngine", L"DuplicateOutput1 固定捕获格式 B8G8R8A8");
    }
    if (FAILED(hr))
        hr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
    // 启动瞬间显卡可能暂时不可用（显示器刚切换/驱动忙），短暂重试几次
    for (int attempt = 0; FAILED(hr) && attempt < 3; attempt++)
    {
        if (hr != kDxgiErrorNotCurrentlyAvailable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        hr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
    }
    if (FAILED(hr))
    {
        LastError = Format(L"DuplicateOutput 失败 (0x%08X)", (unsigned)hr);
        duplication_.Reset();
        return;
    }
    adapter_ = adapter;
    output_ = output;
}

void LutEngine::EnsureFrameTexture(const ComPtr<ID3D11Texture2D>& src)
{
    if (!src || !frameTexture_) return;
    D3D11_TEXTURE2D_DESC cur{}, srcDesc{};
    frameTexture_->GetDesc(&cur);
    src->GetDesc(&srcDesc);
    if (cur.Width == srcDesc.Width && cur.Height == srcDesc.Height && cur.Format == srcDesc.Format)
        return;
    // 分辨率/格式（如 HDR）变化：重建帧纹理，否则 CopyResource 失败 → 黑屏/花屏
    frameSrv_.Reset();
    frameTexture_.Reset();
    D3D11_TEXTURE2D_DESC texDesc{};
    src->GetDesc(&texDesc);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&texDesc, nullptr, frameTexture_.GetAddressOf()))) return;
    device_->CreateShaderResourceView(frameTexture_.Get(), nullptr, frameSrv_.GetAddressOf());
}

void LutEngine::EnsureSwapChainSize(UINT w, UINT h)
{
    if ((int)w == width_ && (int)h == height_) return;
    width_ = (int)w;
    height_ = (int)h;
    {
        std::lock_guard<std::mutex> lock(paramsMutex_);
        params_[38] = width_ > 0 ? 1.0f / (float)width_ : 0.0f;
        params_[39] = height_ > 0 ? 1.0f / (float)height_ : 0.0f;
        paramsDirty_.store(true);
    }
    backBufferTex_.Reset();
    for (auto& slot : backBufferRtv_) { slot.tex.Reset(); slot.rtv.Reset(); }
    if (swapChain_)
        swapChain_->ResizeBuffers(2, w, h, swapChainFormat_, 0);
    // 覆盖层隐藏期间（等待捕获恢复）不要顺手把它显示出来
    if (hwnd_ && overlayVisible_.load())
        SetWindowPos(hwnd_, HWND_TOPMOST, x_, y_, (int)w, (int)h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
}

// ---------------- 渲染循环 ----------------

// 自愈：捕获失效（显示模式/分辨率/色彩空间变化、进出独占全屏、驱动忙）时不再
// 一次失败就终止渲染线程 —— 那样会留下一个冻结在屏幕上的覆盖层，且滤镜静默失效，
// 只能等用户动 UI 才恢复。这里改为带退避重试；仍失败就保持覆盖层隐藏、继续等待，
// 直到捕获恢复（running_ 由 Dispose 控制）。
void LutEngine::RenderLoop()
{
    int frameCount = 0;
    while (running_)
    {
        Trace(L"循环开始");
        if (!duplication_)
        {
            SetOverlayVisible(false);
            captureOk_ = false;
            if (!RecoverCaptureWithRetry(5, 500))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        bool neutralNow = neutral_.load();
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(neutralNow ? 500u : 100u,
                                                    &frameInfo, resource.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            if (!captureOk_.exchange(true))
            {
                SetOverlayVisible(true);
                Log::Write(L"LutEngine", L"捕获已恢复");
            }
            if (resource)
            {
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(tex.GetAddressOf()))))
                {
                    // 分辨率/格式变化：先重建纹理与交换链，避免 CopyResource 失败
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    EnsureSwapChainSize(d.Width, d.Height);
                    EnsureFrameTexture(tex);

                    // 输入模式按捕获纹理格式判定（与输出模式相互独立）
                    int newInputMode = 0;
                    if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM) newInputMode = 1;
                    else if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) newInputMode = 2;
                    if (newInputMode != inputMode_)
                    {
                        inputMode_ = newInputMode;
                        UpdateColorModeBuffer();
                        Log::WriteFmt(L"LutEngine", L"捕获格式=%d 输入模式=%d 输出模式=%d",
                                      (int)d.Format, inputMode_, colorMode_);
                    }

                    // GPU 内拷贝：捕获帧 → 渲染纹理（无 CPU 往返）。
                    // 中性参数也照常拷贝+呈现（直通），否则 flip 交换链的覆盖层
                    // 内容会卡在旧帧。
                    context_->CopyResource(frameTexture_.Get(), tex.Get());
                    duplication_->ReleaseFrame();
                    DrawAndPresent();
                }
                else
                {
                    duplication_->ReleaseFrame();
                }
            }
            else
            {
                duplication_->ReleaseFrame();
            }
            // 自检只做一次：原来每 30 帧整屏读回会卡 GPU（4060 上明显卡顿的元凶之一）
            if (!selfChecked_ && ++frameCount >= 30)
            {
                selfChecked_ = true;
                RenderSelfCheck();
            }
        }
        else if (hr == kDxgiErrorWaitTimeout)
        {
            // 桌面无新帧：若参数刚变化（静态画面下调滑块），用最近一帧补一次重绘
            if (paramsDirty_.load())
                DrawAndPresent();
        }
        else if (hr == kDxgiErrorAccessLost)
        {
            // 桌面模式/分辨率/色彩空间变化（含 DSR、HDR 切换、游戏进出全屏）。
            // 先隐藏覆盖层避免冻结画面，再重建捕获。
            Log::WriteFmt(L"LutEngine", L"捕获丢失 (0x%08X)，开始重建", (unsigned)hr);
            SetOverlayVisible(false);
            captureOk_ = false;
            int oldMode = colorMode_;
            if (!RecoverCaptureWithRetry(10, 400))
            {
                // 仍不可用（例如游戏独占全屏）：保持隐藏并等待，绝不留下死覆盖层
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // 色彩空间可能已变化：交换链格式与色彩空间声明都要跟着重建
            DetectColorSpace();
            if (colorMode_ != oldMode)
            {
                Log::WriteFmt(L"LutEngine", L"色彩空间模式 %d -> %d，重建交换链",
                              oldMode, colorMode_);
                RebuildSwapChainForColorSpace();
            }
            UpdateColorModeBuffer();
            SetOverlayVisible(true);
            captureOk_ = true;
        }
        else
        {
            Log::WriteFmt(L"LutEngine", L"DXGI 捕获错误 0x%08X，尝试恢复", (unsigned)hr);
            SetOverlayVisible(false);
            captureOk_ = false;
            duplication_.Reset();
            if (!RecoverCaptureWithRetry(5, 500))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            SetOverlayVisible(true);
            captureOk_ = true;
        }
    }
    SetOverlayVisible(false);
    running_ = false;
    Log::Write(L"LutEngine", L"RenderLoop ended");
}

bool LutEngine::RecoverCapture()
{
    duplication_.Reset();
    adapter_.Reset();
    output_.Reset();
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

bool LutEngine::RecoverCaptureWithRetry(int attempts, int delayMs)
{
    for (int i = 0; i < attempts && running_; i++)
    {
        if (RecoverCapture()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    return duplication_.Get() != nullptr;
}

// 显示模式/色彩空间变化后重建交换链：交换链格式与 SetColorSpace1 声明都必须跟着变，
// 旧实现直接停止引擎并留一个冻结覆盖层（滤镜静默失效），现改为原地重建。
void LutEngine::RebuildSwapChainForColorSpace()
{
    backBufferTex_.Reset();
    for (auto& slot : backBufferRtv_) { slot.tex.Reset(); slot.rtv.Reset(); }
    swapChain_.Reset();
    CreateSwapChain();
    if (!swapChain_)
    {
        LastError = L"色彩空间变化后重建交换链失败";
        Log::Write(L"LutEngine", LastError.c_str());
    }
}

void LutEngine::DrawAndPresent()
{
    // 护栏：覆盖层不可见时绝不能 Present —— flip 模型交换链向未显示的窗口呈现会
    // 永久阻塞等待 DWM 取帧（v2.0.0-beta 卡死根因）。捕获恢复期间窗口是隐藏的。
    if (!swapChain_) return;
    if (hwnd_ && !IsWindowVisible(hwnd_))
    {
        if (!presentSkipLogged_)
        {
            presentSkipLogged_ = true;
            Log::Write(L"LutEngine", L"覆盖层不可见，暂停呈现（等待恢复）");
        }
        return;
    }
    presentSkipLogged_ = false;

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

    // 2) 绘制：每捕获到一帧就立即呈现一次，让覆盖层与桌面源帧 1:1 锁步。
    //    flip 模型交换链的 2 缓冲队列会在 DWM 合成边界自然限速，无需再用
    //    QPC 软件节流（软件节流会丢帧、并与 AI 补帧/高刷产生错相位 → 果冻）。
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return;
    backBufferTex_ = backBuffer;
    // 按缓冲身份缓存 RTV：flip 模型后缓冲每帧在 2 个缓冲间轮转，
    // 单槽缓存会退化为每帧重建 RTV（4060 上明显卡顿的原因之一）。
    ID3D11RenderTargetView* rtv = GetBackBufferRtv(backBuffer.Get());
    if (!rtv) return;

    // 全屏三角形覆盖全部像素，因此不再需要 ClearRenderTargetView
    //（旧实现每帧清屏两次 ≈ 44MB 显存流量，1440p@144Hz 纯属浪费）。
    if (neutral_.load())
    {
        // 中性直通：走全屏三角形 + 直通像素着色器。
        // 不直接 CopyResource，因为捕获帧与后缓冲格式可能不一致（HDR/格式切换）。
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, (float)width_, (float)height_, 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(rasterizer_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(inputLayout_.Get());
        UINT stride = 24, offset = 0;
        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(psPassthrough_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(1, 1, psModeBuffer_.GetAddressOf());
        context_->PSSetShaderResources(0, 1, frameSrv_.GetAddressOf());
        context_->PSSetSamplers(0, 1, inputSampler_.GetAddressOf());
        context_->Draw(3, 0);
    }
    else
    {
        // 单遍合并：捕获帧 →（可选）线性光邻域后处理 → LUT 采样 → 输出色彩空间 → 后缓冲。
        // 中间纹理（R16G16B16A16_FLOAT）与第二遍 draw 已移除。
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, (float)width_, (float)height_, 0, 1 };
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(rasterizer_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(inputLayout_.Get());
        UINT stride = 24, offset = 0;
        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, paramsBuffer_.GetAddressOf());
        context_->PSSetConstantBuffers(1, 1, psModeBuffer_.GetAddressOf());
        ID3D11ShaderResourceView* srvs[] = { frameSrv_.Get(), lutSrv_.Get() };
        context_->PSSetShaderResources(0, 2, srvs);
        context_->PSSetSamplers(0, 1, inputSampler_.GetAddressOf());
        context_->PSSetSamplers(1, 1, lutSampler_.GetAddressOf());
        context_->Draw(3, 0);
    }

    UINT syncInterval = UseVsync ? 1u : 0u;
    Trace(L"Present 开始");
    swapChain_->Present(syncInterval, 0);
    Trace(L"Present 返回");
}

ID3D11RenderTargetView* LutEngine::GetBackBufferRtv(ID3D11Texture2D* buffer)
{
    if (!buffer) return nullptr;
    for (auto& slot : backBufferRtv_)
    {
        if (slot.tex.Get() == buffer)
            return slot.rtv.Get();
    }
    for (auto& slot : backBufferRtv_)
    {
        if (!slot.tex)
        {
            slot.tex = buffer;
            if (FAILED(device_->CreateRenderTargetView(buffer, nullptr, slot.rtv.GetAddressOf())))
                return nullptr;
            return slot.rtv.Get();
        }
    }
    // 槽已满（BufferCount 被调大时才会发生）：复用最旧的一槽
    backBufferRtv_[0].tex.Reset();
    backBufferRtv_[0].rtv.Reset();
    backBufferRtv_[0].tex = buffer;
    if (FAILED(device_->CreateRenderTargetView(buffer, nullptr, backBufferRtv_[0].rtv.GetAddressOf())))
        return nullptr;
    return backBufferRtv_[0].rtv.Get();
}

void LutEngine::RenderSelfCheck()
{
    // 从 back buffer 读回中心像素，黑屏时记录日志
    // 中性参数时覆盖层可能从未呈现过（backBufferTex_ 为空），直接跳过
    if (!backBufferTex_) return;
    // 后缓冲格式随输出色彩空间变化：SDR=BGRA8 / HDR10=R10G10B10A2 / scRGB=RGBA16F。
    // 三种格式都做非黑校验（旧实现 HDR 直接跳过，等于 HDR 下没有自检）。
    UINT pxBytes = colorMode_ == 1 ? 4u : (colorMode_ == 2 ? 8u : 4u);
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
        const BYTE* px = (const BYTE*)map.pData + (size_t)cy * map.RowPitch + (size_t)cx * pxBytes;
        bool visible = false;
        std::wstring detail;
        if (colorMode_ == 0)
        {
            BYTE b = px[0], g = px[1], r = px[2], a = px[3];
            visible = (r | g | b) > 8;
            detail = Format(L"R=%u G=%u B=%u A=%u", r, g, b, a);
        }
        else if (colorMode_ == 1)
        {
            // R10G10B10A2_UNORM 小端：低 10 位为 R
            DWORD v = 0;
            memcpy(&v, px, 4);
            unsigned r = v & 0x3FF, g = (v >> 10) & 0x3FF, b = (v >> 20) & 0x3FF;
            visible = (r | g | b) > 4;
            detail = Format(L"R10=%u G10=%u B10=%u", r, g, b);
        }
        else
        {
            unsigned acc = 0;
            for (UINT i = 0; i < 8; i++) acc |= px[i];
            visible = acc > 0;
            detail = Format(L"RGBA16F 首像素非零=%d", visible ? 1 : 0);
        }
        context_->Unmap(staging.Get(), 0);
        Log::WriteFmt(L"LutEngine", L"渲染自检(模式%d): 中心像素 %s -> %s",
                      colorMode_, detail.c_str(), visible ? L"画面正常" : L"画面全黑!");
    }
    catch (...)
    {
    }
}

// ---------------- 销毁 ----------------

void LutEngine::ReleaseAll()
{
    duplication_.Reset();
    adapter_.Reset();
    output_.Reset();
    lutUav_.Reset();
    lutSrv_.Reset();
    lutTexture_.Reset();
    frameSrv_.Reset();
    frameTexture_.Reset();
    paramsBuffer_.Reset();
    psModeBuffer_.Reset();
    vertexBuffer_.Reset();
    rasterizer_.Reset();
    inputLayout_.Reset();
    ps_.Reset();
    psPassthrough_.Reset();
    vs_.Reset();
    cs_.Reset();
    inputSampler_.Reset();
    lutSampler_.Reset();
    for (auto& slot : backBufferRtv_) { slot.tex.Reset(); slot.rtv.Reset(); }
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
        Trace(L"等待渲染线程退出");
        renderThread_.join();
        Trace(L"渲染线程已退出");
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
