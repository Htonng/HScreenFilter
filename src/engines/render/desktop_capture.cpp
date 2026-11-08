#include "desktop_capture.h"
#include "log.h"
#include <dxgi1_6.h>
#include <cmath>

namespace hsf {

namespace {
constexpr uint32_t kDxgiErrorWaitTimeout = 0x887A0027;
constexpr uint32_t kDxgiErrorAccessLost = 0x887A0026;
constexpr uint32_t kDxgiErrorNotCurrentlyAvailable = 0x887A0021;

// 0..1 平滑阶跃（场景探测诊断用；与处理层的暗场景权重无关）
int FormatProbeIndex(DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) return 1;
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) return 2;
    return 0;
}

// 半精度 → float（位运算实现，只用于诊断采样，避免引入数学库依赖）
float HalfToFloatForProbe(unsigned short h)
{
    unsigned sign = (unsigned)(h & 0x8000u) << 16;
    unsigned exp = (h >> 10) & 0x1Fu;
    unsigned man = h & 0x3FFu;
    unsigned bits = 0;
    if (exp == 0)
    {
        if (man == 0)
        {
            bits = sign;
        }
        else
        {
            unsigned e = 127 - 15 + 1;
            while (!(man & 0x400u)) { man <<= 1; e--; }
            man &= 0x3FFu;
            bits = sign | (e << 23) | (man << 13);
        }
    }
    else if (exp == 31)
    {
        bits = sign | 0x7F800000u | (man << 13);
    }
    else
    {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
} // namespace

DesktopCapture::~DesktopCapture()
{
    Stop();
}

bool DesktopCapture::FindOutputForPoint(int x, int y, ComPtr<IDXGIAdapter>& adapter,
                                        ComPtr<IDXGIOutput>& output)
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
                x >= d.DesktopCoordinates.left && x < d.DesktopCoordinates.right &&
                y >= d.DesktopCoordinates.top && y < d.DesktopCoordinates.bottom)
            {
                adapter = a;
                output = o;
                return true;
            }
        }
    }
    return false;
}

const wchar_t* DesktopCapture::FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return L"B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return L"B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return L"R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return L"R16G16B16A16_FLOAT";
    default: return L"其他格式";
    }
}

bool DesktopCapture::Start(ID3D11Device* device, ID3D11DeviceContext* context,
                           int x, int y, int width, int height, int outputIndex)
{
    device_ = device;
    context_ = context;
    x_ = x;
    y_ = y;
    (void)width;
    (void)height;
    (void)outputIndex;
    LastError.clear();
    ResetProbeState();
    lastFrameTick_ = 0;
    lastUsableFrameTick_ = 0;
    foreignLogged_ = false;
    captureFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!CreateCapture()) return false;

    // 工作格式固定为 BGRA8；工作纹理在首帧到达时按实际尺寸创建。
    Log::WriteFmt(L"DesktopCapture", L"捕获工作格式=%s（DXGI 桌面复制）",
                  FormatName(captureFormat_));
    return true;
}

void DesktopCapture::Stop()
{
    duplication_.Reset();
    adapter_.Reset();
    output_.Reset();
    frameSrv_.Reset();
    frameTexture_.Reset();
    probeStaging_.Reset();
    probeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    probePending_ = false;
    device_ = nullptr;
    context_ = nullptr;
}

bool DesktopCapture::FindOutput(ComPtr<IDXGIAdapter>& adapter, ComPtr<IDXGIOutput>& output)
{
    return FindOutputForPoint(x_, y_, adapter, output);
}

bool DesktopCapture::CreateCapture()
{
    // 按坐标匹配输出而不是按索引：EnumDisplayMonitors 与 EnumOutputs 的顺序
    // 不一定一致，多显示器时按索引可能捕获到错误的屏幕（导致滤镜作用错屏）。
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    if (!FindOutput(adapter, output))
    {
        LastError = Format(L"未找到坐标 (%d,%d) 对应的 DXGI 输出", x_, y_);
        return false;
    }
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(output1.GetAddressOf()))))
    {
        LastError = L"IDXGIOutput1 不可用";
        return false;
    }
    // 固定捕获格式为 B8G8R8A8_UNORM：本引擎只做 SDR 工作空间（滤镜输入输出都是 sRGB
    // 编码的 BGRA8），DuplicateOutput1 支持指定格式，避免驱动在多种格式间逐帧切换。
    HRESULT hr = E_FAIL;
    captureFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output5.GetAddressOf()))))
    {
        DXGI_FORMAT fmt = captureFormat_;
        hr = output5->DuplicateOutput1(device_, 0, 1, &fmt, duplication_.GetAddressOf());
        if (SUCCEEDED(hr))
            Log::WriteFmt(L"DesktopCapture", L"DuplicateOutput1 固定捕获格式 %s",
                          FormatName(captureFormat_));
    }
    if (FAILED(hr))
        hr = output1->DuplicateOutput(device_, duplication_.GetAddressOf());
    // 启动瞬间显卡可能暂时不可用（显示器刚切换/驱动忙），短暂重试几次
    for (int attempt = 0; FAILED(hr) && attempt < 3; attempt++)
    {
        if (hr != kDxgiErrorNotCurrentlyAvailable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        hr = output1->DuplicateOutput(device_, duplication_.GetAddressOf());
    }
    if (FAILED(hr))
    {
        LastError = Format(L"DuplicateOutput 失败 (0x%08X)", (unsigned)hr);
        duplication_.Reset();
        return false;
    }
    adapter_ = adapter;
    output_ = output;
    ResetProbeState();
    lastFrameTick_ = 0;
    lastUsableFrameTick_ = 0;
    return true;
}

bool DesktopCapture::Recover()
{
    duplication_.Reset();
    adapter_.Reset();
    output_.Reset();
    probeStaging_.Reset();
    probeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    probePending_ = false;
    if (!CreateCapture())
    {
        Log::WriteFmt(L"DesktopCapture", L"RecoverCapture FAILED: %s", LastError.c_str());
        return false;
    }
    LastError.clear();
    Log::Write(L"DesktopCapture", L"RecoverCapture OK");
    return true;
}

bool DesktopCapture::RecoverWithRetry(int attempts, int delayMs, const std::atomic<bool>* running)
{
    for (int i = 0; i < attempts && (!running || running->load()); i++)
    {
        if (Recover()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    return duplication_.Get() != nullptr;
}

DesktopCapture::Status DesktopCapture::Poll(Frame& frame, UINT timeoutMs)
{
    frame = Frame{};
    if (!duplication_) return Status::Error;

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, resource.GetAddressOf());
    if (hr == kDxgiErrorWaitTimeout) return Status::NoFrame;
    if (hr == kDxgiErrorAccessLost) return Status::AccessLost;
    if (FAILED(hr)) return Status::Error;

    frame.held = true;
    frame.desktopImageUpdated = info.LastPresentTime.QuadPart != 0;
    frame.pointerUpdated = info.LastMouseUpdateTime.QuadPart != 0;
    frame.newPointerShape = info.PointerShapeBufferSize != 0;
    frame.accumulatedFrames = info.AccumulatedFrames;
    frame.rectsCoalesced = info.RectsCoalesced != FALSE;
    frame.tick = GetTickCount();
    lastFrameTick_ = frame.tick;

    if (!resource) { ReleaseFrame(frame); return Status::NoFrame; }
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(tex.GetAddressOf()))))
    {
        ReleaseFrame(frame);
        return Status::NoFrame;
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    frame.texture = tex;
    frame.format = desc.Format;
    frame.width = desc.Width;
    frame.height = desc.Height;
    return Status::Frame;
}

bool DesktopCapture::IsForeign(const Frame& frame) const
{
    return frame.format != captureFormat_;
}

void DesktopCapture::MarkUsableFrame()
{
    lastUsableFrameTick_ = lastFrameTick_;
}

void DesktopCapture::ReleaseFrame(Frame& frame)
{
    if (frame.held && duplication_)
        duplication_->ReleaseFrame();
    frame.held = false;
    frame.texture.Reset();
}

bool DesktopCapture::EnsureWorkTexture(ID3D11Texture2D* src)
{
    if (!src || !device_) return false;
    if (frameTexture_)
    {
        D3D11_TEXTURE2D_DESC cur{}, srcDesc{};
        frameTexture_->GetDesc(&cur);
        src->GetDesc(&srcDesc);
        if (cur.Width == srcDesc.Width && cur.Height == srcDesc.Height &&
            cur.Format == srcDesc.Format &&
            cur.Usage == D3D11_USAGE_DEFAULT &&
            cur.BindFlags == D3D11_BIND_SHADER_RESOURCE &&
            cur.MiscFlags == 0 && cur.CPUAccessFlags == 0)
            return true;
        frameSrv_.Reset();
        frameTexture_.Reset();
    }
    D3D11_TEXTURE2D_DESC texDesc{};
    src->GetDesc(&texDesc);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // 关键：DXGI 桌面复制的源纹理带共享/键控互斥标志（实测 MiscFlags=0x2900）。
    // 旧实现的工作纹理是固定 desc（MiscFlags=0）；本次分层重构一开始直接继承了
    // 源 desc，导致目标工作纹理也变成共享键控互斥纹理，CopyResource 在未获取
    // keyed mutex 时静默失败 —— 表现就是「打开滤镜全黑屏」，而场景探测读源帧正常。
    // 工作纹理只是本进程内的渲染中间件，绝不能继承源纹理的共享/键控互斥标志。
    texDesc.MiscFlags = 0;
    texDesc.CPUAccessFlags = 0;
    if (FAILED(device_->CreateTexture2D(&texDesc, nullptr, frameTexture_.GetAddressOf())))
        return false;
    return SUCCEEDED(device_->CreateShaderResourceView(frameTexture_.Get(), nullptr,
                                                       frameSrv_.GetAddressOf()));
}

bool DesktopCapture::CopyToWorkTexture(Frame& frame)
{
    if (!frame.held || !frame.texture || !context_) return false;
    if (!EnsureWorkTexture(frame.texture.Get()))
    {
        ReleaseFrame(frame);
        return false;
    }
    context_->CopyResource(frameTexture_.Get(), frame.texture.Get());
    ReleaseFrame(frame);
    return true;
}

void DesktopCapture::ResetProbeState()
{
    probeStaging_.Reset();
    probeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    probePending_ = false;
    lastSceneSampleTick_ = 0;
    for (auto& p : formatProbe_) p = FormatProbe{};
}

bool DesktopCapture::EnsureProbeStaging(DXGI_FORMAT format)
{
    if (probeStaging_ && probeStagingFormat_ == format) return true;
    probeStaging_.Reset();
    probeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = 16;
    sd.Height = 16 * 5;   // 5 个采样块，纵向排列
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, probeStaging_.GetAddressOf())))
        return false;
    probeStagingFormat_ = format;
    return true;
}

void DesktopCapture::BeginSceneProbe(ID3D11Texture2D* tex, DXGI_FORMAT format)
{
    probePending_ = false;
    if (!tex || !context_ || !device_) return;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (d.Width < 128 || d.Height < 128) return;
    if (!EnsureProbeStaging(format)) return;

    // 采样点：中心 + 四象限中心（按比例定位，分辨率无关）
    const double kU[5] = { 0.50, 0.25, 0.75, 0.25, 0.75 };
    const double kV[5] = { 0.50, 0.25, 0.25, 0.75, 0.75 };
    for (int i = 0; i < 5; i++)
    {
        LONG cx = (LONG)(d.Width * kU[i]);
        LONG cy = (LONG)(d.Height * kV[i]);
        D3D11_BOX box{};
        box.left = cx - 8;
        box.top = cy - 8;
        box.right = box.left + 16;
        box.bottom = box.top + 16;
        box.back = 1;
        context_->CopySubresourceRegion(probeStaging_.Get(), 0, 0, i * 16, 0, tex, 0, &box);
    }
    probePending_ = true;
}

bool DesktopCapture::ReadProbeStaging(SceneSample& out, bool nonBlocking)
{
    out = SceneSample{};
    if (!probePending_ || !probeStaging_ || !context_) return false;

    // DO_NOT_WAIT：GPU 还没把这一轮拷贝做完就返回「还在画」，本轮直接放弃、下一拍再读。
    D3D11_MAPPED_SUBRESOURCE m{};
    UINT mapFlags = nonBlocking ? (UINT)D3D11_MAP_FLAG_DO_NOT_WAIT : 0u;
    if (FAILED(context_->Map(probeStaging_.Get(), 0, D3D11_MAP_READ, mapFlags, &m)))
    {
        if (nonBlocking) out.notReady = true;
        probePending_ = false;
        return false;
    }
    probePending_ = false;
    DXGI_FORMAT format = probeStagingFormat_;

    double maxLuma = 0.0;
    double sumEnc = 0.0;
    int encCount = 0;
    for (int y = 0; y < 16 * 5; y++)
    {
        const BYTE* row = (const BYTE*)m.pData + (size_t)y * m.RowPitch;
        for (int x = 0; x < 16; x++)
        {
            double enc = 0.0;
            if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                const unsigned short* p = (const unsigned short*)(row + (size_t)x * 8);
                double lin = 0.0;
                for (int c = 0; c < 3; c++)
                {
                    float v = HalfToFloatForProbe(p[c]);
                    if (v > maxLuma) maxLuma = v;
                    lin += (c == 0 ? 0.2126 : (c == 1 ? 0.7152 : 0.0722)) * v;
                }
                double n = lin;
                if (n < 0.0) n = 0.0;
                if (n > 1.0) n = 1.0;
                enc = n <= 0.0031308 ? n * 12.92 : 1.055 * pow(n, 1.0 / 2.4) - 0.055;
            }
            else if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
            {
                DWORD v = 0;
                memcpy(&v, row + (size_t)x * 4, 4);
                double r = (double)(v & 0x3FF) / 1023.0;
                double g = (double)((v >> 10) & 0x3FF) / 1023.0;
                double b = (double)((v >> 20) & 0x3FF) / 1023.0;
                double vmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
                if (vmax > maxLuma) maxLuma = vmax;
                enc = vmax;
            }
            else
            {
                BYTE b = row[(size_t)x * 4 + 0], g = row[(size_t)x * 4 + 1], r = row[(size_t)x * 4 + 2];
                BYTE vmax = r > g ? (r > b ? r : b) : (g > b ? g : b);
                double v = (double)vmax / 255.0;
                if (v > maxLuma) maxLuma = v;
                enc = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
            }
            sumEnc += enc;
            encCount++;
        }
    }
    context_->Unmap(probeStaging_.Get(), 0);
    out.valid = true;
    out.maxLuma = maxLuma;
    out.mean = encCount > 0 ? sumEnc / encCount : -1.0;
    return true;
}

SceneSample DesktopCapture::TakeSceneSample(const Frame& frame)
{
    SceneSample result;
    if (!frame.texture) return result;
    DWORD now = GetTickCount();
    if (now - lastSceneSampleTick_ < 1000) return result;
    lastSceneSampleTick_ = now;

    LARGE_INTEGER q0{}, q1{};
    QueryPerformanceCounter(&q0);
    ReadProbeStaging(result, true);   // 非阻塞：绝不在这里等 GPU
    QueryPerformanceCounter(&q1);
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    result.costMs = freq.QuadPart ? (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;

    // 再为当前帧发起本轮拷贝（GPU 排队，不产生 CPU 等待）
    BeginSceneProbe(frame.texture.Get(), frame.format);
    return result;
}

double DesktopCapture::SampleCaptureMaxLuma(ID3D11Texture2D* tex, DXGI_FORMAT format, double* meanOut)
{
    if (meanOut) *meanOut = -1.0;
    if (!tex || !context_ || !device_) return -1.0;
    BeginSceneProbe(tex, format);
    SceneSample sample;
    if (!ReadProbeStaging(sample, false)) return -1.0;
    if (meanOut) *meanOut = sample.mean;
    return sample.maxLuma;
}

void DesktopCapture::OnForeignFrame(const Frame& frame)
{
    if (!frame.texture) return;
    if (!foreignLogged_)
    {
        foreignLogged_ = true;
        Log::WriteFmt(L"DesktopCapture",
                      L"捕获帧格式 %s 与固定工作格式 %s 不一致，丢弃该帧（避免切换色彩映射导致闪烁）",
                      FormatName(frame.format), FormatName(captureFormat_));
    }

    // 采样该格式的内容（每格式每秒最多一次），只用于诊断日志
    int idx = FormatProbeIndex(frame.format);
    DWORD now = GetTickCount();
    if (now - formatProbe_[idx].tick >= 1000)
    {
        formatProbe_[idx].tick = now;
        double luma = SampleCaptureMaxLuma(frame.texture.Get(), frame.format);
        if (luma >= 0.0)
        {
            bool black = luma < 0.004;
            if (!formatProbe_[idx].known || formatProbe_[idx].black != black)
            {
                formatProbe_[idx].known = true;
                formatProbe_[idx].black = black;
                Log::WriteFmt(L"DesktopCapture", L"异格式帧 %s 采样：5 区最大亮度=%.3f（%s）",
                              FormatName(frame.format), luma, black ? L"整帧全黑" : L"有内容");
            }
        }
    }
}

} // namespace hsf
