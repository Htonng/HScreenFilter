#include "lut_engine.h"
#include "render/desktop_capture.h"
#include "log.h"
#include <dxgi.h>
#include <dxgi1_6.h>

namespace hsf {

namespace {

// QPC 频率（进程内恒定）：取一次即可，避免在每帧热路径反复查询。
LONGLONG QpcFreq()
{
    static LONGLONG f = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return v.QuadPart ? v.QuadPart : 1;
    }();
    return f;
}

double QpcMs(LONGLONG from, LONGLONG to)
{
    return (double)(to - from) * 1000.0 / (double)QpcFreq();
}

// 诊断跟踪：设置环境变量 HSF_TRACE=1 后，渲染循环会在关键阶段打点。
// 默认关闭，零开销；用于定位「卡在捕获 / 卡在 Present / 卡在恢复」。
bool TraceEnabled()
{
    static int cached = -1;
    if (cached < 0)
    {
        wchar_t buf[8] = {};
        cached = (GetEnvironmentVariableW(L"HSF_TRACE", buf, 8) > 0 && buf[0] != L'0') ? 1 : 0;
    }
    return cached == 1;
}

void Trace(const wchar_t* stage)
{
    if (TraceEnabled()) Log::WriteFmt(L"LutEngine", L"[trace] %s", stage);
}

// 坐标所在显示器是否处于 HDR（判据：DXGI 输出的色彩空间）。不需要创建 D3D 设备，
// UI 线程可直接调用。用途：HDR 显示器上「放大镜/伽马」引擎对合成面无效，
// 宿主据此自动改用 LUT 引擎（见 app_main.cpp 的 ApplyCurrent）。
bool QueryDisplayHdrUncached(int x, int y)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return false;
    for (UINT ai = 0;; ai++)
    {
        ComPtr<IDXGIAdapter> adapter;
        if (factory->EnumAdapters(ai, adapter.GetAddressOf()) != S_OK) break;
        for (UINT oi = 0;; oi++)
        {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, output.GetAddressOf()) != S_OK) break;
            DXGI_OUTPUT_DESC d{};
            if (FAILED(output->GetDesc(&d))) continue;
            if (x < d.DesktopCoordinates.left || x >= d.DesktopCoordinates.right ||
                y < d.DesktopCoordinates.top || y >= d.DesktopCoordinates.bottom)
                continue;
            ComPtr<IDXGIOutput6> o6;
            if (FAILED(output->QueryInterface(IID_PPV_ARGS(o6.GetAddressOf())))) return false;
            DXGI_OUTPUT_DESC1 d1{};
            if (FAILED(o6->GetDesc1(&d1))) return false;
            return d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                   d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        }
    }
    return false;
}

} // namespace

// 带缓存的 HDR 查询：真实查询要 CreateDXGIFactory1 + 枚举适配器/输出 + GetDesc1，
// 每次几百微秒；UI 拖动滑块时每秒上千次调用，不缓存会烧掉几百毫秒/秒。
bool QueryDisplayHdr(int x, int y)
{
    struct Entry { int x, y; bool hdr; DWORD tick; };
    static std::mutex m;
    static std::vector<Entry> cache;

    std::lock_guard<std::mutex> lock(m);
    DWORD now = GetTickCount();
    for (auto& e : cache)
        if (e.x == x && e.y == y && now - e.tick < 2000) return e.hdr;

    bool hdr = QueryDisplayHdrUncached(x, y);
    for (auto& e : cache)
        if (e.x == x && e.y == y) { e.hdr = hdr; e.tick = now; return hdr; }
    cache.push_back(Entry{ x, y, hdr, now });
    return hdr;
}

LutEngine::~LutEngine()
{
    Dispose();
}

void LutEngine::Apply(const FilterSettings& s)
{
    processor_.Apply(s);
}

bool LutEngine::CreateDevice(int x, int y)
{
    // 笔记本混合显卡（Optimus/双卡）：默认适配器可能是核显或错误的那块 GPU，
    // 导致 DuplicateOutput 失败 → 引擎回退伽马 → 鲜艳度/HSL 全部失效。
    // 先找到真正驱动目标显示器的适配器来创建设备。
    ComPtr<IDXGIAdapter> targetAdapter;
    ComPtr<IDXGIOutput> targetOutput;
    if (DesktopCapture::FindOutputForPoint(x, y, targetAdapter, targetOutput) && targetAdapter)
    {
        HRESULT hr = D3D11CreateDevice(targetAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, device_.GetAddressOf(), nullptr,
                                       context_.GetAddressOf());
        if (SUCCEEDED(hr)) return true;
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

bool LutEngine::Start(int x, int y, int width, int height, int outputIndex)
{
    const DWORD startTick = GetTickCount();
    DWORD lastTick = startTick;
    auto logPhase = [&](const wchar_t* name) {
        DWORD now = GetTickCount();
        Log::WriteFmt(L"LutEngine", L"启动阶段：%s %ums（累计 %ums）",
                      name, (unsigned)(now - lastTick), (unsigned)(now - startTick));
        lastTick = now;
    };

    try
    {
        if (!CreateDevice(x, y)) return false;
        logPhase(L"D3D11 设备");

        if (!presenter_.Start(device_.Get(), x, y, width, height))
        {
            LastError = L"覆盖层创建失败：" + presenter_.LastError;
            return false;
        }
        logPhase(L"覆盖层窗口/交换链");

        if (!processor_.Initialize(device_.Get(), context_.Get(), width, height))
        {
            LastError = L"着色器管线创建失败：" + processor_.LastError;
            return false;
        }
        logPhase(L"着色器管线");

        if (!capture_.Start(device_.Get(), context_.Get(), x, y, width, height, outputIndex))
        {
            LastError = L"DXGI 桌面复制失败：" + capture_.LastError;
            return false;
        }
        logPhase(L"桌面复制");

        running_ = true;
        renderThread_ = std::thread([this] { RenderLoop(); });
        Log::WriteFmt(L"LutEngine", L"Start OK: %dx%d @%d,%d output=%d（启动总耗时 %ums）",
                      width, height, x, y, outputIndex, (unsigned)(GetTickCount() - startTick));
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

void LutEngine::Dispose()
{
    if (disposed_.exchange(true)) return;
    running_ = false;
    presenter_.BeginShutdown();   // 让渲染线程里「等窗口可见」的循环立刻退出
    Log::Write(L"LutEngine", L"Dispose begin");
    if (renderThread_.joinable())
    {
        Log::Write(L"LutEngine", L"等待渲染线程退出");
        renderThread_.join();
        Log::Write(L"LutEngine", L"渲染线程已退出");
    }
    capture_.Stop();
    presenter_.Stop();
    processor_.Shutdown();
    device_.Reset();
    context_.Reset();
    Log::Write(L"LutEngine", L"Dispose end");
}

void LutEngine::SetFpsDetail(bool on)
{
    fpsDetail_.store(on);
    presenter_.SetFpsDetail(on);
}

void LutEngine::SetOsdText(const std::wstring& text)
{
    presenter_.SetOsdText(text);
}

void LutEngine::ApplyOverlayAffinity()
{
    presenter_.SetCapturable(Capturable);
}

void LutEngine::SetOverlayVisible(bool visible)
{
    presenter_.SetOverlayVisible(visible);
}

// ---------------------------------------------------------------------------
// 渲染循环：三层编排
// ---------------------------------------------------------------------------
void LutEngine::RenderLoop()
{
    // 渲染线程优先级：默认 NORMAL 会被满载游戏/驱动线程抢占 10~35ms；
    // ABOVE_NORMAL 仍低于游戏常用的 HIGHEST/MMCSS，只保证每帧那 ~0.2ms 按时跑完。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Log::Write(L"LutEngine", L"渲染线程优先级：ABOVE_NORMAL（避免满载时被抢占、错过合成窗口）");

    startTick_ = GetTickCount();
    bool firstFrameLogged = false;
    bool firstPresentLogged = false;

    while (running_)
    {
        Trace(L"循环开始");
        LARGE_INTEGER qIterStart{};
        QueryPerformanceCounter(&qIterStart);
        double iterMs = 0.0;
        if (prevIterStartQpc_.QuadPart)
            iterMs = QpcMs(prevIterStartQpc_.QuadPart, qIterStart.QuadPart);
        prevIterStartQpc_ = qIterStart;

        if (!capture_.Ready())
        {
            presenter_.SetOverlayVisible(false);
            captureOk_ = false;
            if (!capture_.RecoverWithRetry(5, 500, &running_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        const bool neutralNow = processor_.IsNeutral();
        const bool startupGrace = (DWORD)(GetTickCount() - startTick_) < kStartupGraceMs;

        LARGE_INTEGER qAcqWait0{}, qAcqWait1{};
        QueryPerformanceCounter(&qAcqWait0);
        Trace(L"等待新帧");
        DesktopCapture::Frame frame;
        DesktopCapture::Status status = capture_.Poll(frame, neutralNow ? 500u : 100u);
        QueryPerformanceCounter(&qAcqWait1);

        if (status == DesktopCapture::Status::Frame)
        {
            Trace(L"拿到新帧");
            if (!captureOk_.exchange(true))
            {
                presenter_.SetOverlayVisible(true);
                Log::Write(L"LutEngine", L"捕获已恢复");
            }
            if (frame.desktopImageUpdated) seenDesktopPresentTime_ = true;
            if (frame.accumulatedFrames > 1)
                accumulatedFrames_ += (int)frame.accumulatedFrames - 1;
            if (frame.rectsCoalesced) coalescedRects_++;

            LARGE_INTEGER qWork0{};
            QueryPerformanceCounter(&qWork0);
            FrameTiming timing;
            timing.acquireWaitMs = QpcMs(qAcqWait0.QuadPart, qAcqWait1.QuadPart);

            if (capture_.IsForeign(frame))
            {
                // 启动保护期：静默丢弃，不做阻塞诊断采样，也不记日志。
                if (!startupGrace) capture_.OnForeignFrame(frame);
                capture_.ReleaseFrame(frame);
            }
            else
            {
                capture_.MarkUsableFrame();
                if (!firstFrameLogged)
                {
                    firstFrameLogged = true;
                    Log::WriteFmt(L"LutEngine", L"首帧捕获：启动后 %ums（%ux%u）",
                                  (unsigned)(GetTickCount() - startTick_),
                                  frame.width, frame.height);
                }
                if (pausedForUnusableFrames_)
                {
                    pausedForUnusableFrames_ = false;
                    presenter_.SetOverlayVisible(true);
                    Log::Write(L"LutEngine", L"捕获已回到可用格式，覆盖层恢复");
                }

                bool blackSample = false;
                SceneSample probe = capture_.TakeSceneSample(frame);
                timing.probeMs = probe.costMs;
                if (probe.notReady) probeNotReady_++;
                if (probe.valid)
                {
                    // 详细帧率模式下保留每秒一条的场景探测记录（诊断全黑帧/暗场景链路用）
                    if (fpsDetail_.load())
                        Log::WriteFmt(L"LutEngine",
                                      L"场景探测：5 区最大亮度=%.3f，平均亮度=%.3f（延迟一帧回读）",
                                      probe.maxLuma, probe.mean);
                    if (probe.maxLuma < kBlackLumaThreshold)
                    {
                        // 单帧全黑：丢掉这一帧，保住上一帧画面（不呈现黑屏）。
                        if (!startupGrace && pinnedBlackStreak_ < 100) pinnedBlackStreak_++;
                        blackSample = true;
                    }
                    else
                    {
                        pinnedBlackStreak_ = 0;
                    }
                    // 暗场景权重在这里进入处理层；是否真的重建 LUT 由处理层决定。
                    if (probe.mean >= 0.0)
                    {
                        bool stable = (DWORD)(GetTickCount() - startTick_) >= 2500;
                        processor_.UpdateSceneWeight(probe.mean, GetTickCount(), stable);
                    }
                }

                if (pinnedBlackStreak_ >= kBlackStreakToPause)
                {
                    if (!pausedForBlackFrames_)
                    {
                        pausedForBlackFrames_ = true;
                        presenter_.SetOverlayVisible(false);
                        Log::Write(L"LutEngine",
                                   L"捕获帧整帧全黑（HDR 下 DXGI 会返回全黑帧），已暂停覆盖层避免黑屏");
                    }
                    capture_.ReleaseFrame(frame);
                }
                else
                {
                    if (pausedForBlackFrames_)
                    {
                        pausedForBlackFrames_ = false;
                        presenter_.SetOverlayVisible(true);
                        Log::Write(L"LutEngine", L"捕获帧不再全黑，覆盖层恢复");
                    }
                    if (blackSample)
                    {
                        capture_.ReleaseFrame(frame);
                    }
                    else if (seenDesktopPresentTime_ && !frame.desktopImageUpdated &&
                             !frame.newPointerShape && !processor_.HasPendingParams())
                    {
                        // 重复帧护栏：桌面位图没变、也没有新指针形状 → 不重绘不呈现。
                        dupFrames_++;
                        if (frame.pointerUpdated) pointerOnlyFrames_++;
                        capture_.ReleaseFrame(frame);
                    }
                    else if (!presenter_.IsVisible())
                    {
                        // 覆盖层隐藏期间不必拷整屏纹理；隐藏的 flip 交换链也不能 Present。
                        presentsSuppressed_ = true;
                        capture_.ReleaseFrame(frame);
                    }
                    else
                    {
                        // 分辨率变化：重建后缓冲/窗口几何与处理层 texel 参数。
                        presenter_.SetSize((int)frame.width, (int)frame.height);
                        processor_.SetSize((int)frame.width, (int)frame.height);

                        LARGE_INTEGER qPrep0{}, qPrep1{};
                        QueryPerformanceCounter(&qPrep0);
                        bool copied = capture_.CopyToWorkTexture(frame);
                        QueryPerformanceCounter(&qPrep1);
                        timing.prepMs = QpcMs(qPrep0.QuadPart, qPrep1.QuadPart);
                        if (copied)
                        {
                            Trace(L"绘制并呈现");
                            const bool presented = DrawAndPresent();
                            LARGE_INTEGER qWork1{};
                            QueryPerformanceCounter(&qWork1);
                            if (presented)
                            {
                                if (!firstPresentLogged)
                                {
                                    firstPresentLogged = true;
                                    Log::WriteFmt(L"LutEngine",
                                                  L"首帧呈现：启动后 %ums（取帧等待 %.1fms，准备 %.1fms）",
                                                  (unsigned)(GetTickCount() - startTick_),
                                                  timing.acquireWaitMs, timing.prepMs);
                                }
                                timing.iterMs = iterMs;
                                timing.vsyncWaitMs = lastVsyncWaitMs_;
                                timing.drawMs = QpcMs(qPrep1.QuadPart, qWork1.QuadPart) -
                                                timing.vsyncWaitMs;
                                if (timing.drawMs < 0) timing.drawMs = 0;
                                NotePresentedFrame(timing);
                            }
                        }
                    }
                }
            }

            // 自检只做一次，而且放到启动 5 秒之后（避免在冷启动窗口插入完整 GPU 同步）。
            if (!selfChecked_ && presenter_.IsVisible() &&
                (DWORD)(GetTickCount() - startTick_) >= 5000)
            {
                selfChecked_ = true;
                RenderSelfCheck();
            }
        }
        else if (status == DesktopCapture::Status::NoFrame)
        {
            noFramePolls_++;
            // 桌面无新帧：参数刚变化（静态画面下调滑块）时用最近一帧补一次重绘。
            if (processor_.HasPendingParams())
            {
                LARGE_INTEGER qWork0{}, qWork1{};
                QueryPerformanceCounter(&qWork0);
                const bool presented = DrawAndPresent();
                QueryPerformanceCounter(&qWork1);
                if (presented)
                {
                    FrameTiming t;
                    t.drawMs = QpcMs(qWork0.QuadPart, qWork1.QuadPart);
                    NotePresentedFrame(t);
                }
            }
        }
        else if (status == DesktopCapture::Status::AccessLost)
        {
            Log::Write(L"LutEngine", L"捕获丢失（ACCESS_LOST），开始重建");
            presenter_.SetOverlayVisible(false);
            captureOk_ = false;
            if (!capture_.RecoverWithRetry(10, 400, &running_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            presenter_.SetOverlayVisible(true);
            captureOk_ = true;
        }
        else
        {
            Log::Write(L"LutEngine", L"DXGI 捕获错误，尝试恢复");
            presenter_.SetOverlayVisible(false);
            captureOk_ = false;
            if (!capture_.RecoverWithRetry(5, 500, &running_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            presenter_.SetOverlayVisible(true);
            captureOk_ = true;
        }

        // 固定工作格式长期收不到可用帧（但捕获仍持续出帧）：继续留在屏幕上只会是
        // 一张冻结的旧画面。隐藏覆盖层让用户看到真实桌面，恢复后自动重新显示。
        DWORD tickNow = GetTickCount();
        if (!pausedForUnusableFrames_ && running_ && capture_.LastUsableFrameTick() != 0 &&
            tickNow - startTick_ > kStartupGraceMs &&
            tickNow - capture_.LastUsableFrameTick() > kNoUsableFramePauseMs &&
            tickNow - capture_.LastFrameTick() < 300)
        {
            pausedForUnusableFrames_ = true;
            presenter_.SetOverlayVisible(false);
            Log::WriteFmt(L"LutEngine",
                          L"捕获持续只返回不可用格式（实测全黑）超过 %u 秒，已暂停覆盖层（看到的是未滤镜桌面）",
                          kNoUsableFramePauseMs / 1000);
        }
    }

    presenter_.SetOverlayVisible(false);
    running_ = false;
    Log::Write(L"LutEngine", L"RenderLoop ended");
}

bool LutEngine::DrawAndPresent()
{
    lastVsyncWaitMs_ = 0.0;

    // 硬护栏：覆盖层不可见时绝不能 Present —— flip 模型交换链向未显示的窗口呈现会
    // 永久阻塞等待 DWM 取帧（v2.0.0-beta 卡死根因）。
    ID3D11RenderTargetView* rtv = nullptr;
    if (!presenter_.BeginFrame(&rtv))
    {
        presentsSuppressed_ = true;
        return false;
    }

    bool rebuilt = false;
    if (!processor_.Draw(capture_.WorkSrv(), rtv, &rebuilt))
        return false;
    if (rebuilt) lutRebuilds_++;

    Trace(L"Present 开始");
    presenter_.Present(&lastVsyncWaitMs_);
    Trace(L"Present 返回");
    return true;
}

bool LutEngine::RenderSelfCheck()
{
    // 从后缓冲读回中心 16x16，并比对输入帧（捕获）中心像素：确认 SDR 链路 1:1。
    ID3D11Texture2D* backBuffer = presenter_.BackBufferTexture();
    ID3D11Texture2D* frameTexture = capture_.WorkTexture();
    if (!backBuffer || !frameTexture || !device_ || !context_) return false;
    const UINT pxBytes = 4u;   // 后缓冲与捕获帧都是 B8G8R8A8_UNORM
    try
    {
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        const UINT fullW = desc.Width;
        const UINT fullH = desc.Height;
        desc.Width = 16;
        desc.Height = 16;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
        D3D11_BOX box{};
        box.left = (UINT)(fullW / 2 - 8);
        box.top = (UINT)(fullH / 2 - 8);
        box.right = box.left + 16;
        box.bottom = box.top + 16;
        box.back = 1;
        context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, backBuffer, 0, &box);
        context_->Flush();
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
        const BYTE* px = (const BYTE*)map.pData + (size_t)8 * map.RowPitch + (size_t)8 * pxBytes;
        BYTE b = px[0], g = px[1], r = px[2];
        bool visible = (r | g | b) > 8;
        std::wstring detail = Format(L"R=%u G=%u B=%u", r, g, b);
        context_->Unmap(staging.Get(), 0);
        Log::WriteFmt(L"LutEngine", L"渲染自检: 输出中心像素 %s -> %s",
                      detail.c_str(), visible ? L"画面正常" : L"画面全黑!");

        D3D11_TEXTURE2D_DESC fd{};
        frameTexture->GetDesc(&fd);
        ComPtr<ID3D11Texture2D> inStage;
        D3D11_TEXTURE2D_DESC ins{};
        ins.Width = 1;
        ins.Height = 1;
        ins.MipLevels = 1;
        ins.ArraySize = 1;
        ins.Format = fd.Format;
        ins.SampleDesc.Count = 1;
        ins.Usage = D3D11_USAGE_STAGING;
        ins.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(device_->CreateTexture2D(&ins, nullptr, inStage.GetAddressOf())))
        {
            D3D11_BOX ibox{};
            ibox.left = (UINT)(fd.Width / 2);
            ibox.top = (UINT)(fd.Height / 2);
            ibox.right = ibox.left + 1;
            ibox.bottom = ibox.top + 1;
            ibox.back = 1;
            context_->CopySubresourceRegion(inStage.Get(), 0, 0, 0, 0, frameTexture, 0, &ibox);
            D3D11_MAPPED_SUBRESOURCE im{};
            if (SUCCEEDED(context_->Map(inStage.Get(), 0, D3D11_MAP_READ, 0, &im)))
            {
                const BYTE* ip = (const BYTE*)im.pData;
                BYTE ib = ip[0], ig = ip[1], ir = ip[2];
                context_->Unmap(inStage.Get(), 0);
                Log::WriteFmt(L"LutEngine", L"输入中心像素 R=%u G=%u B=%u（%s），用于比对是否直通",
                              ir, ig, ib, DesktopCapture::FormatName(fd.Format));
            }
        }
        return visible;
    }
    catch (...)
    {
        return false;
    }
}

// 每呈现一帧调用一次。三件事：更新实时帧率、累积帧节奏统计、按节拍写帧率日志。
void LutEngine::NotePresentedFrame(const FrameTiming& t)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    DWORD nowTick = GetTickCount();
    if (statsWarmupUntil_ == 0) statsWarmupUntil_ = nowTick + kStatsWarmupMs;
    if ((int)(statsWarmupUntil_ - nowTick) > 0)
    {
        lastPresentQpc_ = now;
        lastPresentTick_ = nowTick;
        return;
    }

    if (presentsSuppressed_)
    {
        presentsSuppressed_ = false;
        lastPresentQpc_ = now;
        lastPresentTick_ = nowTick;
        fpsWindowStart_ = now;
        fpsWindowCount_ = 0;
        ResetStatsWindow();
        return;
    }

    if (fpsWindowStart_.QuadPart == 0) fpsWindowStart_ = now;
    fpsWindowCount_++;
    double winMs = QpcMs(fpsWindowStart_.QuadPart, now.QuadPart);
    if (winMs >= 500.0)
    {
        presentFps_.store(fpsWindowCount_ * 1000.0 / winMs);
        fpsWindowStart_ = now;
        fpsWindowCount_ = 0;
    }

    if (lastPresentQpc_.QuadPart)
    {
        double gap = QpcMs(lastPresentQpc_.QuadPart, now.QuadPart);
        sumIntervalMs_ += gap;
        if (gap > maxIntervalMs_) maxIntervalMs_ = gap;
        if (minIntervalMs_ <= 0.0 || gap < minIntervalMs_) minIntervalMs_ = gap;
        if (gap > 20.0) longGapCount_++;
        intervalCount_++;
        sumAcquireMs_ += t.acquireWaitMs;
        if (t.acquireWaitMs > maxAcquireMs_) maxAcquireMs_ = t.acquireWaitMs;
        sumProbeMs_ += t.probeMs;
        if (t.probeMs > maxProbeMs_) maxProbeMs_ = t.probeMs;
        sumPrepMs_ += t.prepMs;
        if (t.prepMs > maxPrepMs_) maxPrepMs_ = t.prepMs;
        double workMs = t.probeMs + t.prepMs + t.drawMs;
        sumWorkMs_ += workMs;
        if (workMs > maxWorkMs_) maxWorkMs_ = workMs;
        sumDrawMs_ += t.drawMs;
        if (t.drawMs > maxDrawMs_) maxDrawMs_ = t.drawMs;
        sumVsyncWaitMs_ += t.vsyncWaitMs;
        if (t.vsyncWaitMs > maxVsyncWaitMs_) maxVsyncWaitMs_ = t.vsyncWaitMs;
        if (t.iterMs > 0.0)
        {
            sumIterMs_ += t.iterMs;
            if (t.iterMs > maxIterMs_) maxIterMs_ = t.iterMs;
            double unaccounted = t.iterMs - (t.acquireWaitMs + t.probeMs + t.prepMs +
                                             t.drawMs + t.vsyncWaitMs);
            if (unaccounted < 0) unaccounted = 0;
            sumUnaccountedMs_ += unaccounted;
            if (unaccounted > maxUnaccountedMs_) maxUnaccountedMs_ = unaccounted;
        }
    }
    lastPresentQpc_ = now;
    lastPresentTick_ = GetTickCount();

    if (statsStartTick_ == 0) statsStartTick_ = lastPresentTick_;
    bool detail = fpsDetail_.load();
    DWORD period = statsLogged_ ? (detail ? 1000u : 10000u) : 1000u;
    DWORD elapsed = lastPresentTick_ - statsStartTick_;
    if (elapsed < period) return;

    double fps = intervalCount_ > 0 ? intervalCount_ * 1000.0 / (double)elapsed : 0.0;
    double avgInterval = intervalCount_ > 0 ? sumIntervalMs_ / intervalCount_ : 0.0;
    double avgWork = intervalCount_ > 0 ? sumWorkMs_ / intervalCount_ : 0.0;
    if (detail)
        Log::WriteFmt(L"LutEngine",
                      L"帧率明细：呈现 %.1f 帧/秒（%ums 窗口 %d 帧）| 帧间隔 最小 %.1f / 均值 %.2f / 最大 %.0f ms（>20ms %d 次）"
                      L"| 取帧等待 均值 %.2f 最大 %.1f | 探测回读 均值 %.2f 最大 %.1f | 准备 均值 %.2f 最大 %.1f | 绘制+呈现 均值 %.2f 最大 %.1f | 单帧合计(我们) 均值 %.2f 最大 %.1f ms"
                      L"| 循环周期 均值 %.2f 最大 %.1f | 未计时间(线程被抢占/循环外) 均值 %.2f 最大 %.1f"
                      L"| Present 阻塞 均值 %.2f 最大 %.1f | 重复帧未重绘 %d（其中仅指针 %d）| 源帧累积跳过 %d | 脏区合并 %d | 无新帧轮询 %d | 探测未就绪 %d | LUT 重建 %d",
                      fps, (unsigned)elapsed, intervalCount_, minIntervalMs_, avgInterval, maxIntervalMs_, longGapCount_,
                      intervalCount_ > 0 ? sumAcquireMs_ / intervalCount_ : 0.0, maxAcquireMs_,
                      intervalCount_ > 0 ? sumProbeMs_ / intervalCount_ : 0.0, maxProbeMs_,
                      intervalCount_ > 0 ? sumPrepMs_ / intervalCount_ : 0.0, maxPrepMs_,
                      intervalCount_ > 0 ? sumDrawMs_ / intervalCount_ : 0.0, maxDrawMs_,
                      avgWork, maxWorkMs_,
                      intervalCount_ > 0 ? sumIterMs_ / intervalCount_ : 0.0, maxIterMs_,
                      intervalCount_ > 0 ? sumUnaccountedMs_ / intervalCount_ : 0.0, maxUnaccountedMs_,
                      intervalCount_ > 0 ? sumVsyncWaitMs_ / intervalCount_ : 0.0, maxVsyncWaitMs_,
                      dupFrames_, pointerOnlyFrames_, accumulatedFrames_, coalescedRects_,
                      noFramePolls_, probeNotReady_, lutRebuilds_);
    else
        Log::WriteFmt(L"LutEngine",
                      L"帧率：呈现 %.1f 帧/秒 | 帧间隔 均值 %.2f 最大 %.0fms（长间隔 %d 次）| 单帧合计 均值 %.2f 最大 %.1fms",
                      fps, avgInterval, maxIntervalMs_, longGapCount_, avgWork, maxWorkMs_);

    statsLogged_ = true;
    ResetStatsWindow();
    statsStartTick_ = lastPresentTick_;
}

void LutEngine::ResetStatsWindow()
{
    statsStartTick_ = 0;
    sumIntervalMs_ = maxIntervalMs_ = minIntervalMs_ = 0;
    sumWorkMs_ = maxWorkMs_ = 0;
    sumAcquireMs_ = maxAcquireMs_ = 0;
    sumProbeMs_ = maxProbeMs_ = 0;
    sumPrepMs_ = maxPrepMs_ = 0;
    sumDrawMs_ = maxDrawMs_ = 0;
    sumVsyncWaitMs_ = maxVsyncWaitMs_ = 0;
    sumIterMs_ = maxIterMs_ = 0;
    sumUnaccountedMs_ = maxUnaccountedMs_ = 0;
    dupFrames_ = pointerOnlyFrames_ = accumulatedFrames_ = coalescedRects_ = 0;
    intervalCount_ = 0;
    longGapCount_ = 0;
    noFramePolls_ = 0;
    probeNotReady_ = 0;
    lutRebuilds_ = 0;
}

} // namespace hsf
