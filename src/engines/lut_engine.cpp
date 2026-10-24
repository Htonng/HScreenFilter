#include "lut_engine.h"
#include "hlsl.h"
#include "log.h"
#include <dxgi.h>
#include <dxgi1_6.h>
#include <dwmapi.h>

namespace hsf {

static constexpr uint32_t kDxgiErrorWaitTimeout = 0x887A0027;
static constexpr uint32_t kDxgiErrorAccessLost = 0x887A0026;
static constexpr uint32_t kDxgiErrorNotCurrentlyAvailable = 0x887A0021;

// 固定工作格式连续多久没有可用帧（但捕获仍在出帧）就暂停覆盖层：
// 此时继续留在屏幕上会是一张冻结的旧画面，隐藏覆盖层让用户看到真实桌面更好。
static constexpr DWORD kNoUsableFramePauseMs = 2500;

// 采样区最大分量低于该值即视为「整帧全黑」（≈1/255）
static constexpr double kBlackLumaThreshold = 0.004;

// 0..1 平滑阶跃（用于暗场景优化的自适应权重）
static double SmoothStep01(double e0, double e1, double x)
{
    double t = (x - e0) / (e1 - e0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return t * t * (3.0 - 2.0 * t);
}

// 固定格式连续多少次采样全黑后暂停覆盖层。
// 取 3（≥3 秒）：新建复制后的头几帧本来就是空的（实测 FP16 帧开头为全黑），
// 阈值太小会在启动瞬间误判并把覆盖层藏起来。
static constexpr int kBlackStreakToPause = 3;

static const wchar_t* FormatName(DXGI_FORMAT format)
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

// 捕获纹理格式 → 着色器输入模式（0 sRGB 编码 / 1 HDR10 PQ / 2 scRGB 线性）

// 格式 → 采样槽（用于「该格式实测是否全黑」）
static int FormatProbeIndex(DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) return 1;
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) return 2;
    return 0;
}

// 半精度 → float（位运算实现，只用于诊断采样，避免引入数学库依赖）
static float HalfToFloatForProbe(unsigned short h)
{
    unsigned sign = (unsigned)(h & 0x8000u) << 16;
    unsigned exp = (h >> 10) & 0x1Fu;
    unsigned man = h & 0x3FFu;
    unsigned bits = 0;
    if (exp == 0)
    {
        if (man == 0)
        {
            bits = sign;                       // ±0
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

// QPC 频率（进程内恒定）：取一次即可，避免在每帧的热路径上反复查询。
static LONGLONG QpcFreq()
{
    static LONGLONG f = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return v.QuadPart ? v.QuadPart : 1;
    }();
    return f;
}
// 两个 QPC 打点之间的毫秒数
static double QpcMs(LONGLONG from, LONGLONG to)
{
    return (double)(to - from) * 1000.0 / (double)QpcFreq();
}

// 坐标所在显示器是否处于 HDR（判据：DXGI 输出的色彩空间）。不需要创建 D3D 设备，
// UI 线程可直接调用。用途只剩一个：HDR 显示器上「放大镜/伽马」引擎对合成面无效，
// 宿主据此自动改用 LUT 引擎（见 app_main.cpp 的 ApplyCurrent）。
static bool QueryDisplayHdrUncached(int x, int y)
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

// 带缓存的 HDR 查询：真实查询要 CreateDXGIFactory1 + 枚举适配器/输出 + GetDesc1，
// 每次几百微秒；而调用方（UI 拖动滑块时每秒上千次）根本不关心 2 秒内的重复结果。
// 不缓存的话，UI 线程光在这里就能烧掉几百毫秒/秒 —— 那正是「拖滑块卡顿」的一部分。
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
    float next[kParamsFloatCount] = {};
    float* p = next;
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
    // DarkScene（槽 41）：**只写用户滑块值**，不含场景自适应权重。
    // 权重是渲染线程按场景平均亮度算出来的（槽 42），在 DrawAndPresent 上传常量缓冲时合并 ——
    // 旧实现把「滑块 × 权重」的乘积写在这里，而本函数只在界面来消息时被调用：
    // 场景一亮乘积就落到 0，此后场景再暗也没人重算它（探测那边的开关判据用的又是这个乘积），
    // 于是「一调参数暗场景优化就失效、重启滤镜才恢复」。滑块与权重分槽传递后这类自锁不复存在。
    p[41] = (float)(s.DarkSceneBoost / 100.0);

    // 参数没变就不要重新标脏：ApplyCurrent 在窗口/显示器/监听事件里会被反复调用，
    // 每次无条件重建 64³ LUT 会凭空制造 GPU 重建风暴（拖动滑块时本来就有 30ms 限速，
    // 但「同一个参数重复 apply」不该占这个额度）。
    bool changed = memcmp(next, params_, sizeof(next)) != 0;
    if (changed)
    {
        memcpy(params_, next, sizeof(next));
    }
    neutral_.store(s.IsNeutral());
    if (changed) paramsDirty_.store(true);
}

// ---------------- 生命周期 ----------------

bool LutEngine::Start(int x, int y, int width, int height, int outputIndex)
{
    x_ = x; y_ = y; width_ = width; height_ = height; outputIndex_ = outputIndex;
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
        if (!CreateDevice()) return false;
        logPhase(L"D3D11 设备");
        CreateOverlayWindow();
        if (!hwnd_) return false;
        logPhase(L"覆盖层窗口");
        CreateSwapChain();
        if (!swapChain_) return false;
        logPhase(L"交换链");
        if (!CreatePipeline()) return false;
        logPhase(L"着色器管线");
        CreateCapture();
        if (!duplication_) return false;
        logPhase(L"桌面复制");

        // 初始参数（中性），渲染线程首帧会重建 LUT
        {
            std::lock_guard<std::mutex> lock(paramsMutex_);
            for (int i = 0; i < kParamsFloatCount; i++) params_[i] = 0.0f;
            params_[1] = params_[3] = 1.0f; // MasterSat / GlobalSat 中性
            neutral_.store(true);
            paramsDirty_.store(true);
        }
        // 还没测到场景亮度 → 场景自适应权重按 1.0（用户滑块全额生效）。
        // 若留着构造时的初值不动也没问题，这里显式复位是为了让「重启滤镜」与
        // 「首次 Apply」的起点完全一致（重启后立刻生效，不等 1 秒后的第一次探测）。
        sceneWeight_.store(1.0);

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

    // 帧率 OSD（覆盖层右上角）：一起创建，默认隐藏，等 UI 线程把帧率文字推过来再显示
    CreateOsdWindow();
}

// 创建帧率 OSD 窗口：一个独立的置顶分层小窗（GDI 画字 → UpdateLayeredWindow）。
// 为什么不直接画进 D3D 交换链？—— 一是零风险（完全不碰滤镜渲染管线），
// 二是能直接复用系统字体。窗口属 UI 线程（与覆盖层同线程）。
void LutEngine::CreateOsdWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;   // 只要点击穿透（HTTRANSPARENT），与覆盖层同一套处理
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOsdWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    // 分层 + 点击穿透 + 置顶 + 不激活；TOOLWINDOW 让它不出现在 Alt-Tab/窗口捕获里
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    osd_ = CreateWindowExW(exStyle, kOsdWindowClass, L"HScreenFilter FPS OSD",
                           WS_POPUP, x_, y_, 120, 24, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    if (!osd_) return;

// 关键：OSD 必须对捕获不可见。否则它会被覆盖层自己的桌面复制捕获抓进画面，
    // 再跟着滤镜重新呈现一次 —— 表现为帧率数字重影 / 慢一帧的副本。
    if (!SetWindowDisplayAffinity(osd_, WDA_EXCLUDEFROMCAPTURE))
        Log::WriteFmt(L"LutEngine", L"警告：帧率 OSD 未能设为捕获排除 (err=%u)，可能出现重影",
                      (unsigned)GetLastError());
    ShowWindow(osd_, SW_HIDE);
}

// 刷新覆盖层右上角的帧率 OSD。只在文字或位置变化时重绘（GDI 画字 + ULW，微秒级）。
// 由 UI 线程调用（见 FilterEngine::UpdateFpsOsd ← 500ms 定时器），因此所有窗口操作
// 都发生在拥有该窗口的线程上，不存在跨线程同步窗口调用的死锁风险。
void LutEngine::UpdateOsd()
{
    if (!osd_) return;

    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(osdMutex_);
        text = osdText_;
    }

    // 覆盖层不可见（捕获中断/独占全屏）时不显示：屏幕上不该留着一块悬空的数字
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

    // 文字/位置都没变就跳过重绘
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
        // 只在「从隐藏到显示」时置顶+显示：每 500ms 重新置顶会打扰 DWM 的合成/平面判定
        //（GPU 满载时这种多余动作会连累整屏合成），内容本身用 UpdateLayeredWindow 就够了。
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

void LutEngine::SetFpsDetail(bool on)
{
    fpsDetail_.store(on);    // 渲染线程据此切换日志节拍
    showFpsOn_.store(on);    // UI 线程据此显示/隐藏 OSD
    if (!on)
    {
        std::lock_guard<std::mutex> lock(osdMutex_);
        osdText_.clear();
    }
    UpdateOsd();             // 立即生效（关闭时马上隐藏，打开时等 UI 定时器推文字）
}

void LutEngine::SetOsdText(const std::wstring& text)
{
    {
        std::lock_guard<std::mutex> lock(osdMutex_);
        osdText_ = text;
    }
    UpdateOsd();
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
    // 交换链固定为 sRGB 的 B8G8R8A8：捕获内容就是 SDR 桌面，直接原样呈现即可，
    // 不做 HDR10/scRGB 输出转换（HDR 显示器上的映射由 Windows 按「SDR 内容亮度」处理）。
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // 不用任何交换链标志：节拍完全交给 Present(1) —— 窗口模式下由 DWM 对齐显示器 vblank。
    // 换成「帧延迟等待对象 + Present(SyncInterval 0)」等于让应用自己猜合成相位：一旦错过
    // 一次合成窗口就掉到 1/2、1/3 节拍（180 → 90/60），实测比 Present(1) 更容易抖。
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

    // 最多 1 帧在飞：Present(1) 负责等 vblank，队列里再压 2~3 帧只会平白多出延迟。
    // 注意这是「每交换链的帧延迟上限」，与已经弃用的帧延迟等待对象无关。
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swapChain_->QueryInterface(IID_PPV_ARGS(sc2.GetAddressOf()))) && sc2)
        sc2->SetMaximumFrameLatency(1);
    Log::Write(L"LutEngine", L"强制垂直同步：Present(1) 由 DWM 对齐 vblank（队列最多 1 帧在飞）");
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
    // 固定捕获格式为 B8G8R8A8_UNORM：本引擎只做 SDR 工作空间（滤镜输入输出都是 sRGB
    // 编码的 BGRA8），DuplicateOutput1 支持指定格式，避免驱动在多种格式间逐帧切换
    //（HDR 输出下曾出现 scRGB FP16 与 BGRA8 交替返回帧，表现为全屏闪烁）。
    // 万一仍返回别的格式，渲染循环会丢弃该帧（见 OnForeignFrameFormat）。
    HRESULT hr = E_FAIL;
    captureFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output5.GetAddressOf()))))
    {
        DXGI_FORMAT fmt = captureFormat_;
        hr = output5->DuplicateOutput1(device_.Get(), 0, 1, &fmt, duplication_.GetAddressOf());
        if (SUCCEEDED(hr))
            Log::WriteFmt(L"LutEngine", L"DuplicateOutput1 固定捕获格式 %s", FormatName(captureFormat_));
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

    // 工作格式在本引擎生命周期内固定为 BGRA8，不再逐帧跟随捕获纹理格式
    foreignLogged_ = false;
    // 重建捕获后重新学习各格式的健康状况（全黑帧识别）
    probeStaging_.Reset();
    probeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    for (auto& p : formatProbe_) p = FormatProbe{};
    lastPinnedProbeTick_ = 0;
    pinnedBlackStreak_ = 0;
    lastFrameTick_ = 0;
    lastUsableFrameTick_ = 0;
    Log::WriteFmt(L"LutEngine", L"捕获工作格式=%s（DXGI 桌面复制）", FormatName(captureFormat_));
}

void LutEngine::EnsureFrameTexture(const ComPtr<ID3D11Texture2D>& src)
{
    if (!src) return;
    if (frameTexture_)
    {
        D3D11_TEXTURE2D_DESC cur{}, srcDesc{};
        frameTexture_->GetDesc(&cur);
        src->GetDesc(&srcDesc);
        if (cur.Width == srcDesc.Width && cur.Height == srcDesc.Height && cur.Format == srcDesc.Format)
            return;
        // 分辨率/格式（如 HDR）变化：重建帧纹理，否则 CopyResource 失败 → 黑屏/花屏
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
    if (FAILED(device_->CreateTexture2D(&texDesc, nullptr, frameTexture_.GetAddressOf()))) return;
    device_->CreateShaderResourceView(frameTexture_.Get(), nullptr, frameSrv_.GetAddressOf());
}

// 暂存纹理（16x16 × 5 块）：场景探测的落点。全 GPU 链路下它只在 GPU 命令队列上被写入，
// 读取被推迟到下一轮探测节拍 —— 若在同一拍上「刚提交拷贝就 Map」，CPU 会等待 GPU 排空
// （一次完整的同步点），实测表现为每秒一次的顿挫，也就是用户反馈的「一顿一顿」。
bool LutEngine::EnsureProbeStaging(DXGI_FORMAT format)
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
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, probeStaging_.GetAddressOf()))) return false;
    probeStagingFormat_ = format;
    return true;
}

// 发起一次场景探测：把捕获帧上 5 块 16x16 拷进暂存纹理。整段只是往 GPU 命令队列里排队，
// 调用方不会被阻塞（"全 GPU 链路" 在这里体现为：拷贝 → 采样 → 绘制 → 呈现全在 GPU 上）。
void LutEngine::BeginSceneProbe(ID3D11Texture2D* tex, DXGI_FORMAT format)
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

// 读取上一轮探测的结果：maxOut = 5 块采样点的最大分量值（0..1），
// meanOut = 同一批采样点的平均亮度（sRGB 编码口径）。返回 false 表示本轮无有效结果。
// 因为拷贝是**上一拍**发起的（中间至少隔了一帧的呈现），这里的 Map 基本不等待 GPU，
// 不会在呈现链路上留下气泡。整条滤镜链路里只有这一处 GPU→CPU 回读。
bool LutEngine::ReadProbeStaging(double* maxOut, double* meanOut, bool nonBlocking)
{
    if (maxOut) *maxOut = -1.0;
    if (meanOut) *meanOut = -1.0;
    if (!probePending_ || !probeStaging_ || !context_) return false;

    // DO_NOT_WAIT：GPU 还没把这一轮拷贝做完就返回「还在画」，本轮直接放弃、下一拍再读。
    // 有了它，即使游戏把 GPU 占满、队列排得很深，我们也绝不会在这里等 —— 原来的
    // Map(READ) 会一路等到队列排空，实测在游戏里打出 10~53ms 的顿挫。
    D3D11_MAPPED_SUBRESOURCE m{};
    UINT mapFlags = nonBlocking ? (UINT)D3D11_MAP_FLAG_DO_NOT_WAIT : 0u;
    if (FAILED(context_->Map(probeStaging_.Get(), 0, D3D11_MAP_READ, mapFlags, &m)))
    {
        if (nonBlocking) probeNotReady_++;   // 计入日志（正常应极少）
        probePending_ = false;
        return false;
    }
    probePending_ = false;
    DXGI_FORMAT format = probeStagingFormat_;

    double maxLuma = 0.0;
    double sumEnc = 0.0;
    int encCount = 0;
    // FP16 分支只用于诊断「异格式帧」：按 scRGB 定义（1.0 = 80nit）归一到 SDR 口径，
    // 数值才能与 BGRA8 的均值相互比较。
    const double sdrScale = 1.0;
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
                double n = lin / sdrScale;
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
    if (maxOut) *maxOut = maxLuma;
    if (meanOut && encCount > 0) *meanOut = sumEnc / encCount;
    return true;
}

// 同步采样（发起 + 立即读取）：只给「异格式帧」这类每秒最多一次的诊断路径用，
// 那种帧本来就被丢弃、不参与呈现，阻塞它不会影响画面节奏。
double LutEngine::SampleCaptureMaxLuma(ID3D11Texture2D* tex, DXGI_FORMAT format, double* meanOut)
{
    if (!tex || !context_ || !device_) return -1.0;
    if (meanOut) *meanOut = -1.0;
    BeginSceneProbe(tex, format);
    double maxLuma = -1.0, mean = -1.0;
    if (!ReadProbeStaging(&maxLuma, &mean)) return -1.0;
    if (meanOut) *meanOut = mean;
    return maxLuma;
}

// 每秒一次的探测节拍：先读上一轮结果（几乎不等待），再为当前帧发起本轮拷贝。
// 返回值同时供 DXGI 路径做「整帧全黑」判定；场景平均亮度进 sceneLuma_ 供暗场景优化使用。
// 变化明显且功能开启时触发 LUT 重建，让提亮幅度跟着场景走。
LutEngine::SceneProbe LutEngine::UpdateSceneLuma(ID3D11Texture2D* tex, DXGI_FORMAT format)
{
    SceneProbe result;
    if (!tex) return result;
    DWORD now = GetTickCount();
    if (now - lastSceneSampleTick_ < 1000) return result;
    lastSceneSampleTick_ = now;

    // 1) 先读上一轮（拷贝早已在 GPU 上完成 → Map 不等待）
    LARGE_INTEGER qProbe0{}, qProbe1{};
    QueryPerformanceCounter(&qProbe0);
    double maxLuma = -1.0, mean = -1.0;
    if (ReadProbeStaging(&maxLuma, &mean, true))   // 非阻塞：绝不在这里等 GPU
    {
        QueryPerformanceCounter(&qProbe1);
        result.costMs = QpcMs(qProbe0.QuadPart, qProbe1.QuadPart);
        result.valid = true;
        result.maxLuma = maxLuma;
        result.mean = mean;
        // 详细日志模式（界面「显示滤镜实时帧率」）下顺带记一条探测结果：
        // 它证明延迟一帧的回读路径正常出数（全黑帧护栏 + 暗场景优化都依赖它）。
        if (fpsDetail_.load())
            Log::WriteFmt(L"LutEngine", L"场景探测：5 区最大亮度=%.3f，平均亮度=%.3f（延迟一帧回读）",
                          maxLuma, mean);
        if (mean >= 0.0)
        {
            // 暗场景优化的场景自适应权重（0.5~1）：场景越亮越弱，但**最低保留 50%** ——
            // 用户明确拖动的滑块绝不能被场景采样静默清零（这里只采样 5 块 16×16，占整屏万分之一，
            // 代表性不足以否决用户意图；旧实现自适应到 0，正是「一调参数暗场景优化就失效」的成因）。
            double weight = kDarkSceneMinWeight +
                            (1.0 - kDarkSceneMinWeight) * (1.0 - SmoothStep01(0.08, 0.35, mean));
            // 判据必须是**用户滑块**（槽 41），不能是「滑块 × 权重」的有效值：
            // 用有效值当开关时，亮场景下它落到 0 就等于把自适应永久关掉 —— 场景再暗也不重建。
            bool sliderOn = false;
            {
                std::lock_guard<std::mutex> lock(paramsMutex_);
                sliderOn = params_[41] > 0.001f;
            }
            // 只有「权重变化」才需要重建 LUT。否则按原始场景亮度触发会刷爆：
            // 权重在两端是饱和的（亮场景里亮度从 0.33 抖到 0.42、暗场景里从 0.08 抖到 0.04，
            // 画面其实一模一样），却每次都重跑一遍 64³ 计算着色器 + 常量缓冲上传 ——
            // 实测游戏里打到 41~44 次/秒，是顿挫主因。再加 500ms 最小重建间隔兜底。
            bool weightMoved = (lastSceneWeight_ < 0.0) || fabs(weight - lastSceneWeight_) > 0.03;
            DWORD nowTick = GetTickCount();
            // 启动后的前 2.5 秒先不因为场景亮度改 LUT：这段时间捕获/flip 池/合成器都还在
            // 建立，场景采样本身也可能读到全黑帧；等节奏稳定后再按真实场景重建一次。
            const bool startupStable = (DWORD)(nowTick - startTick_) >= 2500;
            if (sliderOn && weightMoved && startupStable && nowTick - lastSceneRebuildTick_ >= 500)
            {
                lastSceneRebuildTick_ = nowTick;
                lastSceneWeight_ = weight;
                // 权重由渲染线程维护（槽 42 的唯一写入者）：DrawAndPresent 上传常量缓冲时合并进来，
                // 于是 paramsDirty_ 触发的这次重建才真的带上新权重（旧实现重建时槽 41 没变，重建等于白跑）。
                sceneWeight_.store(weight);
                paramsDirty_.store(true);
                Log::WriteFmt(L"LutEngine", L"暗场景优化：场景平均亮度=%.2f → 自适应权重=%.2f（越暗越强）",
                              mean, weight);
            }
            // 没开暗场景优化时清掉记录：这样用户一打开滑块，下一拍就会立刻按当前场景重建一次
            else if (!sliderOn)
            {
                lastSceneWeight_ = -1.0;
            }
        }
    }
    else
    {
        QueryPerformanceCounter(&qProbe1);
        result.costMs = QpcMs(qProbe0.QuadPart, qProbe1.QuadPart);
    }

    // 2) 再为当前帧发起本轮拷贝（GPU 排队，不产生 CPU 等待）
    BeginSceneProbe(tex, format);
    return result;
}

// 异格式捕获帧（HDR 输出下 DXGI 会在 scRGB FP16 与 BGRA8 之间逐帧交替返回帧）：
// 不能跟着它逐帧切换输入模式 —— 那会让同一画面在两套色彩映射之间来回跳，表现为
// 「画面一直闪烁 / 观感突变」。这里一律丢弃该帧（工作格式固定为 BGRA8），
// 只做一次采样把内容健康状况写进日志，便于事后诊断。
void LutEngine::OnForeignFrameFormat(ID3D11Texture2D* tex, DXGI_FORMAT format)
{
    if (!foreignLogged_)
    {
        foreignLogged_ = true;
        Log::WriteFmt(L"LutEngine",
                      L"捕获帧格式 %s 与固定工作格式 %s 不一致，丢弃该帧（避免切换色彩映射导致闪烁）",
                      FormatName(format), FormatName(captureFormat_));
    }

    // 采样该格式的内容（每格式每秒最多一次），只用于诊断日志
    int idx = FormatProbeIndex(format);
    DWORD now = GetTickCount();
    if (now - formatProbe_[idx].tick >= 1000)
    {
        formatProbe_[idx].tick = now;
        double luma = SampleCaptureMaxLuma(tex, format);
        if (luma >= 0.0)
        {
            bool black = luma < kBlackLumaThreshold;
            if (!formatProbe_[idx].known || formatProbe_[idx].black != black)
            {
                formatProbe_[idx].known = true;
                formatProbe_[idx].black = black;
                Log::WriteFmt(L"LutEngine", L"异格式帧 %s 采样：5 区最大亮度=%.3f（%s）",
                              FormatName(format), luma, black ? L"整帧全黑" : L"有内容");
            }
        }
        else
        {
            Trace(L"异格式帧采样失败");
        }
    }

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
        swapChain_->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
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
    // 渲染线程优先级：默认是 NORMAL，满载的游戏/显卡驱动线程可以把我们抢占 10~35ms；
    // 只要错过一次合成窗口，那一拍就掉到 1/2、1/3 节拍（桌面帧被操作系统合并，日志里表现为
    // 「取帧等待 10~35ms + 源帧累积跳过 猛增」），观感就是帧率在 180/90/60 之间来回跳。
    // ABOVE_NORMAL 仍低于游戏通常使用的 HIGHEST / MMCSS，只保证我们每帧那 ~0.2ms 按时跑完。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Log::Write(L"LutEngine", L"渲染线程优先级：ABOVE_NORMAL（避免满载时被抢占、错过合成窗口）");

    // 启动保护期（1.5s）：捕获刚建立时 DXGI 还没有把 flip 池分配好，
    // 头几帧经常是整帧全黑、或格式还没落到固定工作格式上。启动保护期内：
    //   * 不做「全黑帧」判定（不累计连续全黑、不因此隐藏覆盖层）；
    //   * 不为异格式帧做诊断采样（那次读回是阻塞的，启动时最不该做）。
    // 目的就是别在这个窗口里反复 Show/Hide 覆盖层 —— 显示状态抖动比黑一帧难看得多。
    startTick_ = GetTickCount();
    bool firstFrameLogged = false;
    bool firstPresentLogged = false;

    while (running_)
    {
        Trace(L"循环开始");

        // 循环周期（上一拍到这一拍，含等待）：配合下面各分段耗时，用来看「没被任何分段解释掉的时间」
        // 有多少 —— 如果它明显不为 0，说明渲染线程被调度出局了，而不是取帧本身慢。
        LARGE_INTEGER qIterStart{};
        QueryPerformanceCounter(&qIterStart);
        double iterMs = 0.0;
        if (prevIterStartQpc_.QuadPart)
            iterMs = QpcMs(prevIterStartQpc_.QuadPart, qIterStart.QuadPart);
        prevIterStartQpc_ = qIterStart;

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

        // 启动保护期：见上面注释。这里只取一次判定，后面黑帧/异格式分支都用它。
        const bool startupGrace = (DWORD)(GetTickCount() - startTick_) < kStartupGraceMs;

        // 帧节拍：完全交给 DrawAndPresent 里的 Present(1)（窗口模式下 DWM 负责对齐 vblank）。
        // 这里不再有任何等待点 —— 取到了新帧就画、就呈现，Present(1) 自己会把我们按在刷新节拍上。
        LARGE_INTEGER qAcqWait0{}, qAcqWait1{};
        QueryPerformanceCounter(&qAcqWait0);
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(neutralNow ? 500u : 100u,
                                                    &frameInfo, resource.GetAddressOf());
        QueryPerformanceCounter(&qAcqWait1);
        if (SUCCEEDED(hr))
        {
            if (!captureOk_.exchange(true))
            {
                SetOverlayVisible(true);
                Log::Write(L"LutEngine", L"捕获已恢复");
            }
            // 帧元数据：判「重复帧 / 源帧被跳过」的唯一依据（DXGI_OUTDUPL_FRAME_INFO）。
            //   LastPresentTime    —— 桌面位图最后一次更新的时间戳；**0 = 自我们上次取帧以来位图没变**
            //                        （见官方文档，不是「没有帧」：指针移动也算一次新帧）。
            //   LastMouseUpdateTime/PointerShapeBufferSize —— 指针位置/形状是否变了。
            //   AccumulatedFrames  —— 操作系统把多少次桌面更新累积进了这一帧，也就是我们上次处理
            //                        之后漏掉的源帧数。>1 即「源帧堆积」，而中间那几帧已经被操作系统
            //                        合并掉了，拿不回来也补渲不了 —— 只能计数并保证不再重复渲染同一帧。
            const bool desktopImageUpdated = frameInfo.LastPresentTime.QuadPart != 0;
            const bool pointerUpdated = frameInfo.LastMouseUpdateTime.QuadPart != 0;
            const bool newPointerShape = frameInfo.PointerShapeBufferSize != 0;
            if (desktopImageUpdated) seenDesktopPresentTime_ = true;
            if (frameInfo.AccumulatedFrames > 1)
                accumulatedFrames_ += (int)frameInfo.AccumulatedFrames - 1;
            if (frameInfo.RectsCoalesced) coalescedRects_++;
            if (resource)
            {
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(tex.GetAddressOf()))))
                {
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    lastFrameTick_ = GetTickCount();
                    // 本帧的耗时打点（取到帧 → 呈现完成）：用于日志里的分段耗时统计
                    LARGE_INTEGER qWork0{};
                    QueryPerformanceCounter(&qWork0);
                    FrameTiming timing;
                    // 取到帧之前等了多久（源帧率不够时这段会接近帧间隔 —— 一眼看出是不是在等桌面出帧）
                    timing.acquireWaitMs = QpcMs(qAcqWait0.QuadPart, qAcqWait1.QuadPart);

                    // 工作格式固定：异格式帧直接丢弃。HDR 输出下 DXGI 可能在 scRGB FP16 与
                    // BGRA8 之间逐帧交替返回帧，若逐帧跟着切换输入模式，同一画面会在两套
                    // 色彩映射间来回跳 —— 这就是 HDR 下「画面一直闪烁」的根因。
                    if (d.Format != captureFormat_)
                    {
                        // 启动保护期：静默丢弃，不做诊断（采样那一步是阻塞读回，启动时最不该做），
                        // 也不记日志 —— 免得把「flip 池还没分配好」误报成格式问题。
                        if (!startupGrace) OnForeignFrameFormat(tex.Get(), d.Format);
                        duplication_->ReleaseFrame();
                    }
                    else
                    {
                        lastUsableFrameTick_ = lastFrameTick_;
                        if (!firstFrameLogged)
                        {
                            firstFrameLogged = true;
                            Log::WriteFmt(L"LutEngine", L"首帧捕获：启动后 %ums（%ux%u）",
                                          (unsigned)(GetTickCount() - startTick_), d.Width, d.Height);
                        }
                        if (pausedForUnusableFrames_)
                        {
                            pausedForUnusableFrames_ = false;
                            SetOverlayVisible(true);
                            Log::Write(L"LutEngine", L"捕获已回到可用格式，覆盖层恢复");
                        }

                        // 全黑帧护栏（每秒最多采样一次）：HDR 组合下 DXGI 可能返回整帧全黑的帧，
                        // 直接呈现出来就是黑屏。全黑帧一律不呈现；连续两次全黑则隐藏覆盖层，
                        // 恢复有内容后自动重新显示。
                        bool blackSample = false;
                        if (lastFrameTick_ - lastPinnedProbeTick_ >= 1000)
                        {
                            lastPinnedProbeTick_ = lastFrameTick_;
                            // 每秒一次的探测：先读上一轮结果（几乎不等待），再发起本轮 GPU 端拷贝。
                            // 结果用于全黑帧护栏；场景平均亮度由 UpdateSceneLuma 内部消费（暗场景优化）。
                            SceneProbe probe = UpdateSceneLuma(tex.Get(), d.Format);
                            timing.probeMs = probe.costMs;
                            if (!probe.valid)
                            {
                                Trace(L"场景探测尚无结果（首秒）");
                            }
                            else if (probe.maxLuma < kBlackLumaThreshold)
                            {
                                // 单帧全黑：丢掉这一帧，保住上一帧画面（不呈现黑屏）。
                                // 启动保护期内不累计「连续全黑」—— 启动时 DXGI 本来就会给几帧全黑，
                                // 按正常逻辑走会立刻隐藏覆盖层、随后又显示回来，形成反复显隐。
                                if (!startupGrace && pinnedBlackStreak_ < 100) pinnedBlackStreak_++;
                                blackSample = true;
                            }
                            else
                            {
                                pinnedBlackStreak_ = 0;
                            }
                        }

                        if (pinnedBlackStreak_ >= kBlackStreakToPause)
                        {
                            if (!pausedForBlackFrames_)
                            {
                                pausedForBlackFrames_ = true;
                                SetOverlayVisible(false);
                                Log::Write(L"LutEngine",
                                           L"捕获帧整帧全黑（HDR 下 DXGI 会返回全黑帧），已暂停覆盖层避免黑屏");
                            }
                            duplication_->ReleaseFrame();
                        }
                        else
                        {
                            if (pausedForBlackFrames_)
                            {
                                pausedForBlackFrames_ = false;
                                SetOverlayVisible(true);
                                Log::Write(L"LutEngine", L"捕获帧不再全黑，覆盖层恢复");
                            }
                            if (blackSample)
                            {
                                // 单帧全黑（还没到暂停阈值）：丢掉这一帧，保住上一帧画面
                                duplication_->ReleaseFrame();
                            }
                            else if (seenDesktopPresentTime_ && !desktopImageUpdated && !newPointerShape &&
                                     !paramsDirty_.load())
                            {
                                // 重复帧护栏（GPU 满载时「同一帧被渲染多次」→ 果冻 + 帧率虚高）：
                                // `LastPresentTime == 0` 的官方含义就是「自我们上次取帧以来**桌面位图
                                // 没变**」，也就是覆盖层要显示的内容与上一帧完全相同。这时重绘+呈现一次
                                // 纯属无效渲染：同一张图第二次送进合成器会落在不同合成槽上，观感就是
                                // 果冻/顿挫，帧率统计还会把它算成新帧。参数刚变（paramsDirty_）例外 ——
                                // 那时画面确实要变，必须照常呈现。
                                // 说明：本机实测这条很少命中（指针移动也会更新桌面位图），它是给
                                // 「一次取帧只上报指针更新」的驱动/场景准备的护栏。
                                // 只在「见过非零 LastPresentTime」后才启用：个别驱动不上报该时间戳，
                                // 那时宁可照旧每帧重绘，也绝不能把覆盖层冻住。
                                dupFrames_++;
                                if (pointerUpdated) pointerOnlyFrames_++;
                                duplication_->ReleaseFrame();
                            }
                            else if (!overlayVisible_.load())
                            {
                                // 覆盖层隐藏期间（捕获恢复中 / 全黑暂停中）：不必再拷整屏纹理 ——
                                // 1440p 一帧 ≈ 14MB，指针一动就是几百帧/秒的纯显存流量，而且呈现
                                // 本来就会被 DrawAndPresent 的可见性护栏挡掉（隐藏的 flip 交换链不能 Present）。
                                presentsSuppressed_ = true;   // 恢复后的第一帧别把整段隐藏时长算成「帧间隔」
                                duplication_->ReleaseFrame();
                            }
                            else
                            {
                                // 分辨率变化：先重建纹理与交换链，避免 CopyResource 失败
                                EnsureSwapChainSize(d.Width, d.Height);
                                EnsureFrameTexture(tex);

                                // 诊断：像素映射是否 1:1（窗口/交换链/捕获帧三者尺寸必须一致，
                                // 任一不一致都会让覆盖层被拉伸 → 字体模糊 + 锯齿感）
                                if (!sizesLogged_ || d.Width != lastCapturedW_ || d.Height != lastCapturedH_)
                                {
                                    sizesLogged_ = true;
                                    lastCapturedW_ = d.Width;
                                    lastCapturedH_ = d.Height;
                                    UINT swW = 0, swH = 0;
                                    if (swapChain_) { DXGI_SWAP_CHAIN_DESC sd{}; swapChain_->GetDesc(&sd); swW = sd.BufferDesc.Width; swH = sd.BufferDesc.Height; }
                                    RECT wr{}; GetWindowRect(hwnd_, &wr);
                                    Log::WriteFmt(L"LutEngine",
                                                  L"像素映射：捕获帧 %ux%u / 交换链 %ux%u / 窗口 %dx%d @(%d,%d)，引擎 %dx%d @%d,%d",
                                                  d.Width, d.Height, swW, swH,
                                                  wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top,
                                                  width_, height_, x_, y_);
                                }

                                // GPU 内拷贝：捕获帧 → 渲染纹理（无 CPU 往返）。
                                // 中性参数也照常拷贝+呈现（直通），否则 flip 交换链的覆盖层
                                // 内容会卡在旧帧。
                                context_->CopyResource(frameTexture_.Get(), tex.Get());
                                duplication_->ReleaseFrame();
                                LARGE_INTEGER qPrep{};
                                QueryPerformanceCounter(&qPrep);
                                timing.prepMs = QpcMs(qWork0.QuadPart, qPrep.QuadPart) - timing.probeMs;
                                if (timing.prepMs < 0) timing.prepMs = 0;
                                const bool presented = DrawAndPresent();
                                LARGE_INTEGER qWork1{};
                                QueryPerformanceCounter(&qWork1);
                                // 只有真的呈现了才计入帧率/帧节奏统计：原来「没呈现也算一帧」，
                                // 隐藏覆盖层期间日志能报到 400+ 帧/秒，统计完全失真。
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
                                    // 垂直同步等待在 DrawAndPresent 的 Present(1) 里，属于「绘制+呈现」
                                    // 这一段，扣掉它才是纯绘制开销（与旧口径一致，方便对照历史日志）。
                                    timing.drawMs = QpcMs(qPrep.QuadPart, qWork1.QuadPart) - timing.vsyncWaitMs;
                                    if (timing.drawMs < 0) timing.drawMs = 0;
                                    NotePresentedFrame(timing);
                                }
                            }
                        }
                    }
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
            // 自检只做一次，而且放到启动 5 秒之后：
            // 旧实现第 90 帧（≈0.5 秒）就做，正好落在 WebView2/驱动/合成器都还在
            // 冷启动的时间窗里；它内部的 CopyResource + Flush + Map 是一次完整 GPU 同步，
            // 会把刚起来的渲染节奏打断。自检只是诊断（黑屏/直通比对），晚做不影响功能。
            // 黑屏时也不再重试 —— 重试等于把同一个 GPU 同步点重复 3 次。
            if (!selfChecked_ && overlayVisible_.load() &&
                (DWORD)(GetTickCount() - startTick_) >= 5000)
            {
                selfChecked_ = true;
                RenderSelfCheck();
            }
        }
        else if (hr == kDxgiErrorWaitTimeout)
        {
            // 桌面无新帧：若参数刚变化（静态画面下调滑块），用最近一帧补一次重绘
            noFramePolls_++;
            if (paramsDirty_.load())
            {
                LARGE_INTEGER qWork0{};
                QueryPerformanceCounter(&qWork0);
                const bool presented = DrawAndPresent();
                LARGE_INTEGER qWork1{};
                QueryPerformanceCounter(&qWork1);
                // 同样只在真的呈现后计数（覆盖层隐藏时 DrawAndPresent 会直接返回 false）
                if (presented)
                {
                    FrameTiming t;
                    t.drawMs = QpcMs(qWork0.QuadPart, qWork1.QuadPart);
                    NotePresentedFrame(t);
                }
            }
        }
        else if (hr == kDxgiErrorAccessLost)
        {
            // 桌面模式/分辨率/色彩空间变化（含 DSR、HDR 切换、游戏进出全屏）。
            // 先隐藏覆盖层避免冻结画面，再重建捕获。
            Log::WriteFmt(L"LutEngine", L"捕获丢失 (0x%08X)，开始重建", (unsigned)hr);
            SetOverlayVisible(false);
            captureOk_ = false;
            if (!RecoverCaptureWithRetry(10, 400))
            {
                // 仍不可用（例如游戏独占全屏）：保持隐藏并等待，绝不留下死覆盖层
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
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

        // 固定工作格式长期收不到可用帧（但捕获仍持续出帧）：继续留在屏幕上只会是一张
        // 冻结的旧画面。隐藏覆盖层让用户看到真实桌面，恢复可用格式后自动重新显示。
        // 注意用「仍有帧到达」作前提：桌面静止时本来就没有帧，此时画面并未失效。
        DWORD tickNow = GetTickCount();
        if (!pausedForUnusableFrames_ && running_ && lastUsableFrameTick_ != 0 &&
            tickNow - startTick_ > kStartupGraceMs &&   // 启动保护期内不做这个判定
            tickNow - lastUsableFrameTick_ > kNoUsableFramePauseMs &&
            tickNow - lastFrameTick_ < 300)
        {
            pausedForUnusableFrames_ = true;
            SetOverlayVisible(false);
            Log::WriteFmt(L"LutEngine",
                          L"捕获持续只返回不可用格式（实测全黑）超过 %u 秒，已暂停覆盖层（看到的是未滤镜桌面）",
                          kNoUsableFramePauseMs / 1000);
        }
    }
    SetOverlayVisible(false);
    running_ = false;
    Log::Write(L"LutEngine", L"RenderLoop ended");
}

// 每呈现一帧调用一次。三件事：更新实时帧率、累积帧节奏统计、按节拍写帧率日志。
//   * 实时帧率：1 秒窗口内的呈现帧数（每约 0.5 秒刷新一次显示值）；
//   * 帧间隔：相邻两次呈现的间隔（顿挫最直接的证据）；>20ms 记为「长间隔」；
//   * 分段耗时：取帧 / 探测回读 / 准备 / 绘制+呈现，一眼看出卡在哪一段；
//   * 详细模式（「显示滤镜实时帧率」开启）每秒一条，关闭时每 10 秒一条。
void LutEngine::NotePresentedFrame(const FrameTiming& t)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    // 预热期（启动后 1s）：DXGI 第一次 AcquireNextFrame 可能阻塞几百毫秒（flip 池分配、
    // 驱动初始化），那一帧的「取帧等待 / 循环周期」会把均值、最大值和实时帧率一起带偏 ——
    // 日志里第一行动不动就是 100 多帧/秒、最大间隔几百毫秒，全是这一个样本造成的。
    // 预热期内只维护时间基准（保证预热结束后的第一帧间隔不会算成一大段），不记账、不刷新帧率。
    DWORD nowTick = GetTickCount();
    if (statsWarmupUntil_ == 0) statsWarmupUntil_ = nowTick + kStatsWarmupMs;
    if ((int)(statsWarmupUntil_ - nowTick) > 0)
    {
        lastPresentQpc_ = now;
        lastPresentTick_ = nowTick;
        return;
    }

    // 中间被隐藏过一段（捕获恢复 / 全黑暂停）：恢复后的第一帧不能把整段隐藏时长算成帧间隔，
    // 否则日志会跳出一个「最大 3012ms、长间隔 1 次」的假毛刺。这一帧只用来重建基准。
    if (presentsSuppressed_)
    {
        presentsSuppressed_ = false;
        lastPresentQpc_ = now;
        lastPresentTick_ = nowTick;
        fpsWindowStart_ = now;      // 实时帧率窗口也从这一刻重新起算
        fpsWindowCount_ = 0;
        ResetStatsWindow();         // 上一段窗口的累计量必须一起清掉，否则帧率会虚高
        return;
    }

    // 1) 实时帧率（界面右上角）
    if (fpsWindowStart_.QuadPart == 0) fpsWindowStart_ = now;
    fpsWindowCount_++;
    double winMs = QpcMs(fpsWindowStart_.QuadPart, now.QuadPart);
    if (winMs >= 500.0)
    {
        presentFps_.store(fpsWindowCount_ * 1000.0 / winMs);
        fpsWindowStart_ = now;
        fpsWindowCount_ = 0;
    }

    // 2) 帧节奏统计
    if (lastPresentQpc_.QuadPart)
    {
        double gap = QpcMs(lastPresentQpc_.QuadPart, now.QuadPart);
        sumIntervalMs_ += gap;
        if (gap > maxIntervalMs_) maxIntervalMs_ = gap;
        if (minIntervalMs_ <= 0.0 || gap < minIntervalMs_) minIntervalMs_ = gap;
        if (gap > 20.0) longGapCount_++;   // >20ms ≈ 掉一帧（180Hz 下一帧 5.6ms）
        intervalCount_++;
        sumAcquireMs_ += t.acquireWaitMs;
        if (t.acquireWaitMs > maxAcquireMs_) maxAcquireMs_ = t.acquireWaitMs;
        sumProbeMs_ += t.probeMs;
        if (t.probeMs > maxProbeMs_) maxProbeMs_ = t.probeMs;
        sumPrepMs_ += t.prepMs;
        if (t.prepMs > maxPrepMs_) maxPrepMs_ = t.prepMs;
        // 「单帧合计」只统计我们自己的开销（等桌面出新帧的时间单独记在「取帧等待」里）
        double workMs = t.probeMs + t.prepMs + t.drawMs;
        sumWorkMs_ += workMs;
        if (workMs > maxWorkMs_) maxWorkMs_ = workMs;
        sumDrawMs_ += t.drawMs;
        if (t.drawMs > maxDrawMs_) maxDrawMs_ = t.drawMs;
        sumVsyncWaitMs_ += t.vsyncWaitMs;
        if (t.vsyncWaitMs > maxVsyncWaitMs_) maxVsyncWaitMs_ = t.vsyncWaitMs;
        // 循环周期 与 「未被任何分段解释掉的时间」：后者 >0 说明渲染线程被调度出局（抢占），
        // 而不是取帧/绘制本身慢 —— 这是「满载时帧率乱跳」要区分的两件事。
        if (t.iterMs > 0.0)
        {
            sumIterMs_ += t.iterMs;
            if (t.iterMs > maxIterMs_) maxIterMs_ = t.iterMs;
            double unaccounted = t.iterMs - (t.acquireWaitMs + t.probeMs + t.prepMs + t.drawMs + t.vsyncWaitMs);
            if (unaccounted < 0) unaccounted = 0;
            sumUnaccountedMs_ += unaccounted;
            if (unaccounted > maxUnaccountedMs_) maxUnaccountedMs_ = unaccounted;
        }
    }
    lastPresentQpc_ = now;
    lastPresentTick_ = GetTickCount();

    // 3) 日志：首次约 1 秒记一条（便于确认已开始正常呈现），之后按模式取节拍
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

// 清空当前统计窗口的所有累计量（写完一段日志、以及覆盖层从隐藏恢复时都要清）。
// 不清干净就会出现「窗口时长按新的算、帧数还带着上一段」→ 呈现帧率虚高一倍这种假数据。
void LutEngine::ResetStatsWindow()
{
    statsStartTick_ = 0;   // 由下一帧重新起算
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


// 返回 true = 这一拍真的呈现了一帧（调用方据此统计帧率；隐藏/未呈现时返回 false）
bool LutEngine::DrawAndPresent()
{
    lastVsyncWaitMs_ = 0.0;   // 下面真的 Present 了才会填上「等 vblank 的时间」

    // 护栏：覆盖层不可见时绝不能 Present —— flip 模型交换链向未显示的窗口呈现会
    // 永久阻塞等待 DWM 取帧（v2.0.0-beta 卡死根因）。捕获恢复期间窗口是隐藏的。
    if (!swapChain_) return false;
    if (hwnd_ && !IsWindowVisible(hwnd_))
    {
        if (!presentSkipLogged_)
        {
            presentSkipLogged_ = true;
            Log::Write(L"LutEngine", L"覆盖层不可见，暂停呈现（等待恢复）");
        }
        presentsSuppressed_ = true;
        return false;
    }
    presentSkipLogged_ = false;

    // 注意：本实现没有任何自旋/等待节拍 —— 帧怎么来就怎么画，节拍由下面的 Present(1) 提供：
    // 窗口模式下 DWM 会让 Present 阻塞到下一个 vblank，于是每一帧都重新与显示器对齐一次，
    // 不会像 SyncInterval 0 那样把相位误差累积成 1/2、1/3 节拍。

    // 本帧的直通/LUT 选择只取一次：原来「重建 LUT」与「绘制」各读一次 neutral_，
    // 两次之间被 UI 线程改掉就会「跳过 LUT 生成、却用 LUT 着色器绘制」——
    // 采样到上一代（甚至未初始化）的 3D LUT，画面闪一下/颜色全错。
    const bool neutralNow = neutral_.load();

    // 1) 需要重建 LUT？把最新参数拷出并上传，派发计算着色器。
    // 拖动滑块时参数每帧都在变，而每次重建要跑 64³ 计算着色器（262144 线程、重逐像素公式）：
    // 实测拖动时打到 28~77 次/秒，等于把 GPU 让给 LUT 重建（游戏同时满载时观感就是一闪一闪 + 卡）。
    // 这里限成最快 ~30 次/秒：视觉上完全看不出差别，GPU 开销直接砍掉一大半。
    if (paramsDirty_.load())
    {
        DWORD nowTick = GetTickCount();
        if (lastLutRebuildTick_ == 0 || nowTick - lastLutRebuildTick_ >= kLutRebuildMinIntervalMs)
        {
            paramsDirty_.store(false);
            lastLutRebuildTick_ = nowTick;
            lutRebuilds_++;   // 帧率明细日志里的「LUT 重建次数」
            {
                std::lock_guard<std::mutex> lock(paramsMutex_);
                memcpy(paramsCopy_, params_, sizeof(params_));
            }
            // 槽 42（暗场景优化的场景自适应权重）**只在这里写入**：它随场景走，不属于界面下发的
            // 参数（槽 41 是用户滑块值），由渲染线程在场景探测里更新 —— 单一写入者，无需加锁协调。
            paramsCopy_[42] = (float)sceneWeight_.load();
            context_->UpdateSubresource(paramsBuffer_.Get(), 0, nullptr, paramsCopy_, 0, 0);

            // 即使当前是中性也生成一次：保证 3D LUT 纹理永远有确定内容（否则一旦由
            // 「中性→非中性」切换过快，就可能采样到从未写入过的纹理）
            context_->CSSetShader(cs_.Get(), nullptr, 0);
            context_->CSSetConstantBuffers(0, 1, paramsBuffer_.GetAddressOf());
            context_->CSSetUnorderedAccessViews(0, 1, lutUav_.GetAddressOf(), nullptr);
            context_->Dispatch(kLutSize / 4, kLutSize / 4, kLutSize / 4);
            // 解绑 UAV，避免与后续 SRV 绑定冲突
            ID3D11UnorderedAccessView* nullUav[] = { nullptr };
            context_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            context_->CSSetShader(nullptr, nullptr, 0);
        }
        // 未到间隔：这一帧先沿用上一代 LUT/参数（paramsDirty_ 仍为 true，下一帧再重建）
    }

    // 2) 绘制：每捕获到一帧就立即呈现一次，让覆盖层与桌面源帧 1:1 锁步。
    //    flip 模型交换链的 2 缓冲队列会在 DWM 合成边界自然限速，无需再用
    //    QPC 软件节流（软件节流会丢帧、并与 AI 补帧/高刷产生错相位 → 果冻）。
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
        return false;
    backBufferTex_ = backBuffer;
    // 按缓冲身份缓存 RTV：flip 模型后缓冲每帧在 2 个缓冲间轮转，
    // 单槽缓存会退化为每帧重建 RTV（4060 上明显卡顿的原因之一）。
    ID3D11RenderTargetView* rtv = GetBackBufferRtv(backBuffer.Get());
    if (!rtv) return false;

    // 全屏三角形覆盖全部像素，因此不再需要 ClearRenderTargetView
    //（旧实现每帧清屏两次 ≈ 44MB 显存流量，1440p@144Hz 纯属浪费）。
    if (neutralNow)
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
        context_->PSSetShaderResources(0, 1, InputSrvSlot());
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
        ID3D11ShaderResourceView* srvs[] = { frameSrv_.Get(), lutSrv_.Get() };
        context_->PSSetShaderResources(0, 2, srvs);
        context_->PSSetSamplers(0, 1, inputSampler_.GetAddressOf());
        context_->PSSetSamplers(1, 1, lutSampler_.GetAddressOf());
        context_->Draw(3, 0);
    }

    // 强制垂直同步：Present(1) —— 窗口模式下由 DWM 阻塞到下一个 vblank，这是窗口模式唯一
    // 能保证 Frame Pacing 稳定的节拍来源（不再使用帧延迟等待对象，避免两套同步互相打架，
    // 也避免 SyncInterval 0 那种「自己猜合成相位」的抖动）。
    // 阻塞时长记进 lastVsyncWaitMs_（≈一个刷新周期），日志里的「垂直同步等待」就是它。
    Trace(L"Present 开始");
    LARGE_INTEGER qPresent0{}, qPresent1{};
    QueryPerformanceCounter(&qPresent0);
    HRESULT presentHr = swapChain_->Present(1, 0);
    QueryPerformanceCounter(&qPresent1);
    Trace(L"Present 返回");
    lastVsyncWaitMs_ = QpcMs(qPresent0.QuadPart, qPresent1.QuadPart);
    // Present 的失败过去完全被忽略：SyncInterval 0 下它不再阻塞，出问题就只剩「画面不动」。
    // DXGI_STATUS_OCCLUDED 是成功码（窗口被遮挡，属正常），其余失败只记一条日志避免刷屏。
    if (FAILED(presentHr) && !presentFailLogged_)
    {
        presentFailLogged_ = true;
        Log::WriteFmt(L"LutEngine", L"Present 失败 (0x%08X)，呈现可能中断", (unsigned)presentHr);
    }
    return true;
}

// 供 DrawAndPresent 绑定输入纹理：工作纹理（捕获帧已 CopyResource 进来）的 SRV
ID3D11ShaderResourceView** LutEngine::InputSrvSlot()
{
    return frameSrv_.GetAddressOf();
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

bool LutEngine::RenderSelfCheck()
{
    // 从后缓冲读回中心 16x16，并比对输入帧（捕获）中心像素：确认 SDR 链路 1:1，没有压暗/裁切。
    // 中性参数时覆盖层可能从未呈现过（backBufferTex_ 为空），直接跳过。
    if (!backBufferTex_ || !frameTexture_) return false;
    const UINT pxBytes = 4u;   // 后缓冲与捕获帧都是 B8G8R8A8_UNORM
    try
    {
        // 只读回中心 16x16（整屏 CopyResource + Flush + Map 在 4K 上要 300~700ms，
        // 滤镜刚启动时会冻一下 —— 那正是旧实现的自检开销）。
        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC desc{};
        backBufferTex_->GetDesc(&desc);
        desc.Width = 16;
        desc.Height = 16;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return false;
        D3D11_BOX box{};
        box.left = (UINT)(width_ / 2 - 8);
        box.top = (UINT)(height_ / 2 - 8);
        box.right = box.left + 16;
        box.bottom = box.top + 16;
        box.back = 1;
        context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, backBufferTex_.Get(), 0, &box);
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

        // 输入（捕获帧）中心像素：中性参数下应与输出基本一致，差得多说明链路在压/裁
        D3D11_TEXTURE2D_DESC fd{};
        frameTexture_->GetDesc(&fd);
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
            ibox.left = (UINT)(width_ / 2);
            ibox.top = (UINT)(height_ / 2);
            ibox.right = ibox.left + 1;
            ibox.bottom = ibox.top + 1;
            ibox.back = 1;
            context_->CopySubresourceRegion(inStage.Get(), 0, 0, 0, 0, frameTexture_.Get(), 0, &ibox);
            D3D11_MAPPED_SUBRESOURCE im{};
            if (SUCCEEDED(context_->Map(inStage.Get(), 0, D3D11_MAP_READ, 0, &im)))
            {
                const BYTE* ip = (const BYTE*)im.pData;
                BYTE ib = ip[0], ig = ip[1], ir = ip[2];
                context_->Unmap(inStage.Get(), 0);
                Log::WriteFmt(L"LutEngine", L"输入中心像素 R=%u G=%u B=%u（%s），用于比对是否直通",
                              ir, ig, ib, FormatName(fd.Format));
            }
        }

        // LUT 灰轴采样（仅调试构建）：直接回读计算着色器生成的 3D LUT，把「输入→输出」曲线打出来。
        // 发布版不做：64³ RGBA16F 的 CopyResource + Flush + Map 是一次完整 GPU 同步，
        // 放在启动后不久会凭空制造一次卡顿，而它只是诊断信息、不影响滤镜正确性。
#ifdef HSF_DEBUG
        if (lutTexture_)
        {
            D3D11_TEXTURE3D_DESC ld{};
            lutTexture_->GetDesc(&ld);
            D3D11_TEXTURE3D_DESC sdesc = ld;
            sdesc.Usage = D3D11_USAGE_STAGING;
            sdesc.BindFlags = 0;
            sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture3D> lutStage;
            if (SUCCEEDED(device_->CreateTexture3D(&sdesc, nullptr, lutStage.GetAddressOf())))
            {
                context_->CopyResource(lutStage.Get(), lutTexture_.Get());
                context_->Flush();
                D3D11_MAPPED_SUBRESOURCE lm{};
                if (SUCCEEDED(context_->Map(lutStage.Get(), 0, D3D11_MAP_READ, 0, &lm)))
                {
                    const double kAxis[10] = { 0.0, 0.10, 0.21, 0.30, 0.41, 0.51, 0.71, 0.81, 0.90, 1.0 };
                    std::wstring line = L"LUT 灰轴(输入→输出):";
                    for (int i = 0; i < 10; i++)
                    {
                        int idx = (int)(kAxis[i] * (kLutSize - 1) + 0.5);
                        float v = 0.0f;
                        if (idx >= 0 && idx < kLutSize)
                        {
                            // 3D 纹理：z 方向用 DepthPitch，y 方向用 RowPitch（RGBA16F 每像素 8 字节）
                            const BYTE* p = (const BYTE*)lm.pData +
                                            (size_t)idx * lm.DepthPitch + (size_t)idx * lm.RowPitch + (size_t)idx * 8;
                            v = HalfToFloatForProbe(*(const unsigned short*)p);
                        }
                        line += Format(L" %.2f→%.2f", kAxis[i], v);
                    }
                    context_->Unmap(lutStage.Get(), 0);
                    Log::Write(L"LutEngine", line);
                }
            }
        }
#endif // HSF_DEBUG
        return visible;
    }
    catch (...)
    {
        return false;
    }
}

// ---------------- 销毁 ----------------

void LutEngine::ReleaseAll()
{
    statsWarmupUntil_ = 0;   // 下次 Start 重新走一遍预热
    duplication_.Reset();
    adapter_.Reset();
    output_.Reset();
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
    Log::Write(L"LutEngine", L"Dispose end");
}

} // namespace hsf
