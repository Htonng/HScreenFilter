#include "overlay_presenter.h"
#include "log.h"
#include <dxgi1_6.h>

namespace hsf {

OverlayPresenter::~OverlayPresenter()
{
    Stop();
}

bool OverlayPresenter::Start(ID3D11Device* device, int x, int y, int width, int height)
{
    device_ = device;
    x_ = x;
    y_ = y;
    width_ = width;
    height_ = height;
    LastError.clear();
    overlayVisible_.store(true);
    shuttingDown_.store(false);
    presentSkipLogged_ = false;
    presentFailLogged_ = false;

    CreateOverlayWindow();
    if (!hwnd_) return false;
    if (!CreateSwapChain()) return false;
    CreateOsdWindow();
    return true;
}

void OverlayPresenter::BeginShutdown()
{
    shuttingDown_.store(true);
}

void OverlayPresenter::Stop()
{
    shuttingDown_.store(true);
    ReleaseAll();
    if (osd_)
    {
        DestroyWindow(osd_);
        osd_ = nullptr;
    }
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    device_ = nullptr;
}

void OverlayPresenter::ReleaseAll()
{
    for (auto& slot : backBufferRtv_) { slot.tex.Reset(); slot.rtv.Reset(); }
    backBufferTex_.Reset();
    swapChain_.Reset();
}

HRESULT OverlayPresenter::Present(double* waitMs)
{
    if (waitMs) *waitMs = 0.0;
    if (!swapChain_) return E_FAIL;
    LARGE_INTEGER q0{}, q1{}, freq{};
    QueryPerformanceCounter(&q0);
    HRESULT hr = swapChain_->Present(1, 0);
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&freq);
    if (waitMs && freq.QuadPart)
        *waitMs = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    // DXGI_STATUS_OCCLUDED 是成功码（窗口被遮挡，属正常），其余失败只记一条避免刷屏。
    if (FAILED(hr) && !presentFailLogged_)
    {
        presentFailLogged_ = true;
        Log::WriteFmt(L"OverlayPresenter", L"Present 失败 (0x%08X)，呈现可能中断", (unsigned)hr);
    }
    return hr;
}

bool OverlayPresenter::BeginFrame(ID3D11RenderTargetView** rtv)
{
    if (rtv) *rtv = nullptr;
    if (!swapChain_) return false;
    // 硬护栏：flip 模型交换链向未显示的窗口 Present 会永久阻塞（v2.0.0-beta 卡死根因）。
    if (hwnd_ && !IsWindowVisible(hwnd_))
    {
        if (!presentSkipLogged_)
        {
            presentSkipLogged_ = true;
            Log::Write(L"OverlayPresenter", L"覆盖层不可见，暂停呈现（等待恢复）");
        }
        return false;
    }
    presentSkipLogged_ = false;

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return false;
    backBufferTex_ = backBuffer;
    ID3D11RenderTargetView* view = GetBackBufferRtv(backBuffer.Get());
    if (!view) return false;
    if (rtv) *rtv = view;
    return true;
}

ID3D11RenderTargetView* OverlayPresenter::GetBackBufferRtv(ID3D11Texture2D* buffer)
{
    if (!buffer || !device_) return nullptr;
    for (auto& slot : backBufferRtv_)
        if (slot.tex.Get() == buffer) return slot.rtv.Get();
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
    if (FAILED(device_->CreateRenderTargetView(buffer, nullptr,
                                               backBufferRtv_[0].rtv.GetAddressOf())))
        return nullptr;
    return backBufferRtv_[0].rtv.Get();
}

void OverlayPresenter::SetSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    backBufferTex_.Reset();
    for (auto& slot : backBufferRtv_) { slot.tex.Reset(); slot.rtv.Reset(); }
    if (swapChain_)
        swapChain_->ResizeBuffers(2, (UINT)width, (UINT)height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    // 覆盖层隐藏期间（等待捕获恢复）不要顺手把它显示出来
    if (hwnd_ && overlayVisible_.load())
        SetWindowPos(hwnd_, HWND_TOPMOST, x_, y_, width_, height_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
}

bool OverlayPresenter::WindowVisible() const
{
    return hwnd_ && IsWindowVisible(hwnd_);
}

void OverlayPresenter::SetOverlayVisible(bool visible)
{
    if (!hwnd_) return;
    if (overlayVisible_.exchange(visible) == visible) return;
    // 覆盖层窗口属于调用 Start 的线程（UI 线程），本函数在渲染线程里调用：
    // 必须用异步窗口操作，否则一旦 UI 线程阻塞在 join/模态循环里会互相死等。
    if (visible)
    {
        ShowWindowAsync(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, x_, y_, width_, height_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
        for (int i = 0; i < 50 && !shuttingDown_.load(); i++)
        {
            if (IsWindowVisible(hwnd_)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!IsWindowVisible(hwnd_))
            Log::Write(L"OverlayPresenter", L"覆盖层显示尚未生效，本帧跳过呈现");
    }
    else
    {
        ShowWindowAsync(hwnd_, SW_HIDE);
    }
    Log::WriteFmt(L"OverlayPresenter", L"覆盖层%s",
                  visible ? L"已显示" : L"已隐藏（等待捕获恢复）");
}

void OverlayPresenter::CreateOverlayWindow()
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
    // 显示过的隐藏窗口会永久阻塞。分层窗口 alpha 必须显式初始化。
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

    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    SetCapturable(capturable_);

    // 置顶必须改 WS_EX_TOPMOST 样式位（对分层窗口仅 SetWindowPos 无效）
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex |= kWsExTopmost;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

bool OverlayPresenter::CreateSwapChain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))))
        return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = (UINT)width_;
    desc.Height = (UINT)height_;
    // 交换链固定为 B8G8R8A8：捕获内容就是 SDR 桌面，原样呈现，不做 HDR10/scRGB 输出转换。
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // 不用帧延迟等待对象：节拍完全交给 Present(1)，避免两套同步互相打架。
    desc.Flags = 0;

    HRESULT hr = factory->CreateSwapChainForHwnd(device_, hwnd_, &desc, nullptr, nullptr,
                                                 swapChain_.GetAddressOf());
    if (FAILED(hr) && desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
    {
        // 部分驱动/显卡不支持分层窗口 + flip 模型 → 回退传统 DISCARD
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(device_, hwnd_, &desc, nullptr, nullptr,
                                             swapChain_.GetAddressOf());
        if (SUCCEEDED(hr))
            Log::Write(L"OverlayPresenter", L"交换链回退为 DISCARD（分层窗口 + flip 不可用）");
    }
    if (FAILED(hr))
    {
        LastError = Format(L"CreateSwapChainForHwnd 失败 (0x%08X)", (unsigned)hr);
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    // 最多 1 帧在飞：Present(1) 负责等 vblank，队列里再压 2~3 帧只会平白多延迟。
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swapChain_->QueryInterface(IID_PPV_ARGS(sc2.GetAddressOf()))) && sc2)
        sc2->SetMaximumFrameLatency(1);
    Log::Write(L"OverlayPresenter",
               L"强制垂直同步：Present(1) 由 DWM 对齐 vblank（队列最多 1 帧在飞）");
    return true;
}

void OverlayPresenter::CreateOsdWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOsdWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    osd_ = CreateWindowExW(exStyle, kOsdWindowClass, L"HScreenFilter FPS OSD",
                           WS_POPUP, x_, y_, 120, 24, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    if (!osd_) return;

    // 关键：OSD 必须对捕获不可见。否则它会被覆盖层自己的桌面复制捕获抓进画面，
    // 再跟着滤镜重新呈现一次 —— 表现为帧率数字重影 / 慢一帧的副本。
    if (!SetWindowDisplayAffinity(osd_, WDA_EXCLUDEFROMCAPTURE))
        Log::WriteFmt(L"OverlayPresenter",
                      L"警告：帧率 OSD 未能设为捕获排除 (err=%u)，可能出现重影",
                      (unsigned)GetLastError());
    ShowWindow(osd_, SW_HIDE);
}

void OverlayPresenter::SetFpsDetail(bool on)
{
    showFpsOn_.store(on);
    if (!on)
    {
        std::lock_guard<std::mutex> lock(osdMutex_);
        osdText_.clear();
    }
    UpdateOsd();
}

void OverlayPresenter::SetOsdText(const std::wstring& text)
{
    {
        std::lock_guard<std::mutex> lock(osdMutex_);
        osdText_ = text;
    }
    UpdateOsd();
}

void OverlayPresenter::UpdateOsd()
{
    if (!osd_) return;

    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(osdMutex_);
        text = osdText_;
    }

    bool show = showFpsOn_.load() && overlayVisible_.load() &&
                hwnd_ && IsWindowVisible(hwnd_) && !text.empty();
    if (!show)
    {
        if (IsWindowVisible(osd_)) { ShowWindow(osd_, SW_HIDE); osdShown_ = false; }
        osdDrawnText_.clear();
        return;
    }

    // 右上角：留 12px 边距（外框位置随覆盖层几何实时变化）
    const int padX = 9, padY = 4, margin = 12;
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return;
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    HGDIOBJ oldFont = font ? SelectObject(screenDc, font) : nullptr;

    SIZE ts{};
    GetTextExtentPoint32W(screenDc, text.c_str(), (int)text.size(), &ts);
    int w = ts.cx + padX * 2;
    int h = ts.cy + padY * 2;
    int dstX = x_ + width_ - w - margin;
    int dstY = y_ + margin;

    if (text == osdDrawnText_ && dstX == osdDrawnX_ && dstY == osdDrawnY_ &&
        IsWindowVisible(osd_))
    {
        if (font) { SelectObject(screenDc, oldFont); DeleteObject(font); }
        ReleaseDC(nullptr, screenDc);
        return;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // 负数 = 自上而下，与 DIB/ULW 的取样方向一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* maskBits = nullptr;
    void* outBits = nullptr;
    HBITMAP maskBmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    HBITMAP outBmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &outBits, nullptr, 0);
    if (!maskBmp || !outBmp || !maskBits || !outBits)
    {
        if (maskBmp) DeleteObject(maskBmp);
        if (outBmp) DeleteObject(outBmp);
        if (font) { SelectObject(screenDc, oldFont); DeleteObject(font); }
        ReleaseDC(nullptr, screenDc);
        return;
    }

    // 1) 黑底白字画进 mask（R 通道 = 文字覆盖度，抗锯齿由系统字体给出）
    HDC maskDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldMask = SelectObject(maskDc, maskBmp);
    RECT full{ 0, 0, w, h };
    FillRect(maskDc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(maskDc, TRANSPARENT);
    SetTextColor(maskDc, RGB(255, 255, 255));
    TextOutW(maskDc, padX, padY, text.c_str(), (int)text.size());

    // 2) 合成出预乘 RGBA：白字 + 半透明黑底（UpdateLayeredWindow 要求预乘 alpha）
    const int boxA = 150;
    const BYTE* m = (const BYTE*)maskBits;
    BYTE* o = (BYTE*)outBits;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4;
            int cov = m[i + 2];                       // 白字的红通道
            int a = cov + boxA * (255 - cov) / 255;   // 文字覆盖 + 底色残留
            o[i + 0] = (BYTE)cov;                     // 预乘白：R=G=B=覆盖度
            o[i + 1] = (BYTE)cov;
            o[i + 2] = (BYTE)cov;
            o[i + 3] = (BYTE)a;
        }
    }

    HDC outDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldOut = SelectObject(outDc, outBmp);
    POINT dst{ dstX, dstY }, src{ 0, 0 };
    SIZE size{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (UpdateLayeredWindow(osd_, screenDc, &dst, &size, outDc, &src, 0, &bf, ULW_ALPHA))
    {
        osdDrawnText_ = text;
        osdDrawnX_ = dstX;
        osdDrawnY_ = dstY;
        // 只在「从隐藏到显示」时置顶+显示：每 500ms 重新置顶会打扰 DWM 合成/平面判定。
        if (!osdShown_)
        {
            osdShown_ = true;
            SetWindowPos(osd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(osd_, SW_SHOWNOACTIVATE);
        }
    }

    SelectObject(outDc, oldOut);
    DeleteDC(outDc);
    SelectObject(maskDc, oldMask);
    DeleteDC(maskDc);
    DeleteObject(maskBmp);
    DeleteObject(outBmp);
    if (font) { SelectObject(screenDc, oldFont); DeleteObject(font); }
    ReleaseDC(nullptr, screenDc);
}

void OverlayPresenter::SetCapturable(bool capturable)
{
    if (capturable)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Log::Write(L"OverlayPresenter",
                       L"忽略「允许捕获」：覆盖层一旦对桌面复制可见，滤镜会捕获到自己的输出，"
                       L"形成自反馈并迅速变黑；LUT 覆盖层必须固定 WDA_EXCLUDEFROMCAPTURE");
        }
    }
    // Captureable 是旧 JSON 字段（保留兼容），但 LUT 覆盖层不能参与捕获。
    // WDA_MONITOR 允许第三方录制的同时也会被本进程的 DXGI 桌面复制抓到 ——
    // 实测就是「打开滤镜全黑屏」：工作纹理里是覆盖层自己的黑色输出。
    capturable_ = false;
    ApplyOverlayAffinity();
}

void OverlayPresenter::ApplyOverlayAffinity()
{
    if (!hwnd_) return;
    capturable_ = false;
    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    Log::Write(L"OverlayPresenter",
               L"覆盖层捕获亲和性: WDA_EXCLUDEFROMCAPTURE（强制；允许捕获会自反馈全黑）");
}

LRESULT CALLBACK OverlayPresenter::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 点击穿透
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace hsf
