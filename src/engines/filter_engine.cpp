#include "filter_engine.h"
#include "hlsl.h"
#include "gamma_engine.h"
#include "log.h"

namespace hsf {

FilterEngine& FilterEngine::Instance()
{
    static FilterEngine inst;
    return inst;
}

bool FilterEngine::Initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return InitializeLocked();
}

bool FilterEngine::InitializeLocked()
{
    if (checked_) return kind_ != EngineKind::None;
    checked_ = true;

    if (useDxgi_)
    {
        // 轻量探测：创建设备 + 编译着色器（覆盖层与捕获在首次 Apply 时才建立）
        std::wstring selfErr = RunSelfTest();
        if (selfErr.empty())
        {
            // 进入 LUT 前先清掉可能残留的低层效果：上次异常退出留下的伽马表
            // （SetDeviceGammaRamp 不随进程退出自动恢复）会叠在 LUT 输出上，
            // 表现为「打开 LUT 后画面发灰/对比度不对」，最容易被误判成「多层叠加」。
            StopNonLutLayersLocked(true);
            kind_ = EngineKind::PixelShader;
            return true;
        }
        lastError_ = L"DXGI 着色器引擎不可用：" + selfErr + L"，已回退到放大镜引擎";
    }

    // 回退路径同样要求 LUT 覆盖层已彻底退出，避免同屏两层。
    StopLutEnginesLocked();
    // 低层后端启动前也强制复位：上一个进程可能留下伽马表，放大镜可能还在生效。
    StopNonLutLayersLocked(true);
    if (InitializeFallbackLocked()) return true;
    kind_ = EngineKind::None;
    return false;
}

bool FilterEngine::InitializeFallbackLocked()
{
    // 2) 全屏颜色矩阵（Windows 10 1903+，Build 18362+）
    OSVERSIONINFOW ovi{};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    bool buildOk = false;
    // RtlGetVersion 不受 manifest 兼容层影响
    typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))
    {
        if (fn(&ovi) == 0)
            buildOk = ovi.dwMajorVersion > 10 ||
                      (ovi.dwMajorVersion == 10 && ovi.dwBuildNumber >= 18362);
    }
    if (buildOk)
    {
        if (mag_.Initialize())
        {
            magUsed_ = true;
            if (mag_.Apply(FilterSettings()))
            {
                kind_ = EngineKind::FullScreenColorEffect;
                nonLutLayersOn_ = false; // 单位矩阵，等 Apply 真正下发参数才生效
                return true;
            }
            lastError_ = L"系统拒绝了全屏颜色效果（可能被组策略/安全策略禁用）";
            mag_.Uninitialize();
            magUsed_ = false;
        }
        else
        {
            lastError_ = mag_.LastError;
        }
    }
    else
    {
        lastError_ = L"Windows 10 1903+ 才支持全屏颜色效果";
    }

    // 3) 回退：显卡伽马曲线
    if (GammaEngine::Test())
    {
        // 从放大镜切到伽马时，放大镜可能正生效：必须先复位，否则两层叠加。
        if (magUsed_)
        {
            mag_.Reset();
            mag_.Uninitialize();
            magUsed_ = false;
        }
        kind_ = EngineKind::GammaRamp;
        nonLutLayersOn_ = false;
        return true;
    }
    kind_ = EngineKind::None;
    return false;
}

EngineKind FilterEngine::Kind()
{
    std::lock_guard<std::mutex> lock(mutex_);
    InitializeLocked();
    return kind_;
}

void FilterEngine::SetUseDxgi(bool useDxgi)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 同值也要判断当前后端是否已经偏离：LUT 曾因某台显示器启动失败回退到放大镜后，
    // 用户再次把 LUT 打开（值仍是 true）必须能重试 LUT，而不是被 early-return 卡死
    // 在「开关是开、实际跑放大镜」的状态（这也是「效果缺失」的一种来源）。
    const bool sameMode = (useDxgi_ == useDxgi);
    const bool backendMatches = sameMode &&
        ((useDxgi && kind_ == EngineKind::PixelShader) ||
         (!useDxgi && (kind_ == EngineKind::FullScreenColorEffect || kind_ == EngineKind::GammaRamp)));
    if (backendMatches) return;

    useDxgi_ = useDxgi;
    StopAll();
    checked_ = false;
    kind_ = EngineKind::None;
}

void FilterEngine::StopLutEnginesLocked()
{
    for (auto& kv : lutEngines_)
    {
        if (kv.second) kv.second->Dispose();
    }
    lutEngines_.clear();
    appliedByDisplay_.clear();
}

void FilterEngine::StopNonLutLayersLocked(bool force)
{
    // 幂等：只在确实有低层效果时动系统状态（GammaEngine::Reset 每次都设 256 项表）。
    // force=true 用于「首次进入 LUT」：上一条进程异常退出留下的伽马表不在本进程的
    // 标志位里，必须无条件复位一次。
    if (!force && !nonLutLayersOn_ && !magUsed_ && !gammaApplied_) return;
    if (magUsed_)
    {
        mag_.Reset();
        mag_.Uninitialize();
        magUsed_ = false;
    }
    try { GammaEngine::Reset(); } catch (...) {}
    gammaApplied_ = false;
    nonLutLayersOn_ = false;
}

void FilterEngine::StopAll()
{
    // 两个 stop 都是幂等的；force 保证异常路径/上次进程残留也能清干净。
    StopLutEnginesLocked();
    StopNonLutLayersLocked(true);
}

bool FilterEngine::ApplyFallbackLocked(const FilterSettings& s)
{
    // 任何回退发生前都必须把 LUT 层全部拆掉：LUT 是每显示器一个覆盖层，
    // 而放大镜/伽马是整屏全局效果，两者同时存在就是「两层过滤器」。
    StopLutEnginesLocked();
    if (kind_ != EngineKind::FullScreenColorEffect && kind_ != EngineKind::GammaRamp)
    {
        StopNonLutLayersLocked(true);
        if (!InitializeFallbackLocked()) return false;
    }

    if (kind_ == EngineKind::FullScreenColorEffect)
    {
        // 从伽马切回来时先复位伽马表，避免旧曲线叠在放大镜矩阵上。
        if (gammaApplied_)
        {
            try { GammaEngine::Reset(); } catch (...) {}
            gammaApplied_ = false;
        }
        if (!EnsureMagReadyLocked()) return false;
        bool ok = mag_.Apply(s);
        nonLutLayersOn_ = ok;
        return ok;
    }

    if (kind_ == EngineKind::GammaRamp)
    {
        // 防御性：伽马生效时放大镜绝不能还开着（两层线性/曲线叠加）。
        if (magUsed_)
        {
            mag_.Reset();
            mag_.Uninitialize();
            magUsed_ = false;
        }
        // 饱和度偏离中性时提示一次，恢复中性后再偏离才重新提示，避免拖动滑杆刷屏
        if (s.Saturation != 100.0)
        {
            if (!gammaSaturationWarned_)
            {
                gammaSaturationWarned_ = true;
                Log::Write(L"FilterEngine",
                           L"注意：当前使用伽马曲线引擎，鲜艳度（饱和度）无法生效；"
                           L"启用 LUT 引擎或改用放大镜引擎后鲜艳度才可用");
            }
        }
        else
        {
            gammaSaturationWarned_ = false;
        }
        // 记录最后一次伽马参数：显示模式切换会把伽马表重置为线性，看门狗需要据此恢复
        bool ok = GammaEngine::Apply(s);
        lastGammaSettings_ = s;
        gammaApplied_ = ok;
        nonLutLayersOn_ = ok;
        return ok;
    }
    return false;
}

bool FilterEngine::Apply(int displayIndex, const DisplayMonitor& display, const FilterSettings& s)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!InitializeLocked()) return false;

    if (kind_ == EngineKind::PixelShader)
    {
        // 进入 LUT 前清一次低层残留（正常路径已在 Initialize/StopAll 清过，这里兜底）。
        StopNonLutLayersLocked();

        // 获取（必要时创建）该显示器的 LUT 引擎
        auto it = lutEngines_.find(displayIndex);
        LutEngine* engine = nullptr;
        if (it != lutEngines_.end() && it->second && it->second->IsRendering())
        {
            engine = it->second.get();
        }
        else
        {
            if (it != lutEngines_.end())
            {
                it->second->Dispose();
                lutEngines_.erase(it);
            }
            auto eng = std::make_unique<LutEngine>();
            if (!eng->Start(display.X, display.Y, display.Width, display.Height, displayIndex))
            {
                lastError_ = L"DXGI 引擎启动失败：" + eng->LastError;
                Log::WriteFmt(L"FilterEngine",
                              L"LutEngine Start 失败（显示器 %d）：%s；回退到放大镜/伽马，并拆除所有 LUT 覆盖层",
                              displayIndex, lastError_.c_str());
                // 关键：不能在保留其它显示器 LUT 覆盖层的情况下把 kind_ 改成低层引擎。
                // 那样同一桌面会先被 LUT 处理、再被放大镜/伽马处理，形成多层叠加。
                bool ok = ApplyFallbackLocked(s);
                if (!ok) kind_ = EngineKind::None;
                return ok;
            }
            else
            {
                // 引擎刚创建：把此前记录的 vsync/捕获亲和性立即套用，
                // 避免首次启用/切换引擎时设置丢失。
                auto vi = vsyncByDisplay_.find(displayIndex);
                if (vi != vsyncByDisplay_.end()) eng->UseVsync = vi->second;
                auto fi = fpsDetailByDisplay_.find(displayIndex);
                if (fi != fpsDetailByDisplay_.end()) eng->SetFpsDetail(fi->second);
                eng->Capturable = capturable_;
                eng->ApplyOverlayAffinity();
                engine = eng.get();
                lutEngines_[displayIndex] = std::move(eng);
            }
        }
        if (engine)
        {
            engine->Apply(s);
            appliedByDisplay_[displayIndex] = AppliedState{ display, s };
            return true;
        }
    }

    // 非 LUT 后端：先拆掉可能存在的 LUT 覆盖层（防御性，正常路径已拆）。
    if (!lutEngines_.empty()) StopLutEnginesLocked();
    if (kind_ == EngineKind::FullScreenColorEffect || kind_ == EngineKind::GammaRamp)
        return ApplyFallbackLocked(s);
    return false;
}

bool FilterEngine::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    bool ok = true;
    // DXGI 覆盖层是本应用自建窗口：无论当前引擎是什么都停止（幂等）
    StopLutEnginesLocked();
    // 放大镜/伽马是全局效果：关闭滤镜时必须无条件复位，不能只看 kind_。
    // 旧实现只在 kind_==GammaRamp 时复位伽马、只在 magUsed_ 时复位放大镜；
    // 任何异常切换留下的残留都会在下次启用时叠在 LUT 上。
    try { ok = GammaEngine::Reset() && ok; } catch (...) { ok = false; }
    gammaApplied_ = false;
    if (magUsed_)
    {
        mag_.Reset();
        mag_.Uninitialize();
        magUsed_ = false;
    }
    nonLutLayersOn_ = false;
    return ok;
}

void FilterEngine::ResetDisplay(int displayIndex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    appliedByDisplay_.erase(displayIndex);
    if (kind_ != EngineKind::PixelShader)
    {
        // 放大镜/伽马作用于整屏，无法只关掉某一台显示器。界面应尽量让所有
        // 启用的显示器用同一后端；这里不伪造「已关掉」的状态。
        return;
    }
    auto it = lutEngines_.find(displayIndex);
    if (it != lutEngines_.end())
    {
        it->second->Dispose();
        lutEngines_.erase(it);
    }
}

// 临时显示/隐藏滤镜覆盖层（供界面/诊断使用；隐藏期间引擎不呈现，避免冻结画面）
void FilterEngine::SetOverlayVisible(bool visible)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : lutEngines_)
    {
        if (kv.second) kv.second->SetOverlayVisible(visible);
    }
}

// 覆盖层是否处于"暂停显示"状态（引擎还活着，但暂时拿不到画面）
bool FilterEngine::OverlayPaused()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : lutEngines_)
        if (kv.second && kv.second->IsRendering() && !kv.second->OverlayVisible()) return true;
    return false;
}

// 看门狗：把"引擎静默死亡"变成"自动重建"。
// 旧实现里 LutEngine 的渲染线程一旦结束，覆盖层会停在屏幕上冻结最后一帧，
// 滤镜静默失效，只有用户动 UI/切换前台应用才会重建 —— 这正是"某个操作后滤镜就没了"
// 和"屏幕发暗/画面冻住"的来源。
bool FilterEngine::Watchdog()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 上次探测完全失败（例如显示器刚切换、驱动短暂不可用）时给一次重新探测机会；
    // 否则 checked_ 会永远把 kind_=None 缓存到进程结束。
    if (kind_ == EngineKind::None) checked_ = false;
    if (!InitializeLocked()) return false;

    if (kind_ == EngineKind::PixelShader)
    {
        StopNonLutLayersLocked();
        bool rebuilt = false;
        for (auto& kv : appliedByDisplay_)
        {
            int index = kv.first;
            const AppliedState& st = kv.second;
            auto it = lutEngines_.find(index);
            if (it != lutEngines_.end() && it->second && it->second->IsRendering()) continue;

            Log::WriteFmt(L"FilterEngine", L"看门狗：显示器 %d 的 LUT 引擎已停止，自动重建", index);
            if (it != lutEngines_.end())
            {
                if (it->second) it->second->Dispose();
                lutEngines_.erase(it);
            }
            auto eng = std::make_unique<LutEngine>();
            if (!eng->Start(st.Display.X, st.Display.Y, st.Display.Width, st.Display.Height, index))
            {
                Log::WriteFmt(L"FilterEngine", L"看门狗：重建失败：%s", eng->LastError.c_str());
                continue;
            }
            auto vi = vsyncByDisplay_.find(index);
            if (vi != vsyncByDisplay_.end()) eng->UseVsync = vi->second;
            auto fi = fpsDetailByDisplay_.find(index);
            if (fi != fpsDetailByDisplay_.end()) eng->SetFpsDetail(fi->second);
            eng->Capturable = capturable_;
            eng->ApplyOverlayAffinity();
            eng->Apply(st.Settings);
            lutEngines_[index] = std::move(eng);
            rebuilt = true;
        }
        return rebuilt;
    }

    // 低层后端下不允许残留任何 LUT 覆盖层（防御性清场）。
    if (!lutEngines_.empty()) StopLutEnginesLocked();

    if (kind_ == EngineKind::GammaRamp && gammaApplied_)
    {
        // 显示模式切换/游戏进出全屏会由驱动重置伽马表 → 检测不一致才重设（一致时零动作）
        if (!GammaEngine::ApplyIfChanged(lastGammaSettings_))
            Log::Write(L"FilterEngine", L"看门狗：伽马曲线被系统重置，重设失败");
        return false;
    }
    return false;
}

void FilterEngine::SetOverlayCapturable(bool capturable)
{
    std::lock_guard<std::mutex> lock(mutex_);
    capturable_ = capturable;
    for (auto& kv : lutEngines_)
    {
        if (kv.second)
        {
            kv.second->Capturable = capturable;
            // 立即应用 WDA 标志（只改标志位不会让 DWM 重新合成覆盖层）
            kv.second->ApplyOverlayAffinity();
        }
    }
}

void FilterEngine::SetVsync(int displayIndex, bool useVsync)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 先记录期望值：即使引擎尚未创建，后续 Apply 创建引擎时也能套用
    vsyncByDisplay_[displayIndex] = useVsync;
    auto it = lutEngines_.find(displayIndex);
    if (it != lutEngines_.end() && it->second)
    {
        it->second->UseVsync = useVsync;
    }
}

// 滤镜实时呈现帧率（界面右上角）。引擎不存在时返回 -1，界面显示「—」。
double FilterEngine::PresentFps(int displayIndex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lutEngines_.find(displayIndex);
    if (it != lutEngines_.end() && it->second && it->second->IsRendering())
        return it->second->PresentFps();
    return -1.0;
}

// 详细帧率日志开关：先记录期望值（引擎尚未创建时也能生效），已存在则立即切换。
void FilterEngine::SetFpsDetail(int displayIndex, bool on)
{
    std::lock_guard<std::mutex> lock(mutex_);
    fpsDetailByDisplay_[displayIndex] = on;
    auto it = lutEngines_.find(displayIndex);
    if (it != lutEngines_.end() && it->second)
        it->second->SetFpsDetail(on);
}

// 覆盖层右上角的帧率 OSD：每个在跑的 LUT 引擎各显示自己那台显示器的实时帧率。
// 由 UI 线程的定时器调用 —— 窗口创建/绘制都发生在拥有它的线程上，无跨线程窗口调用。
void FilterEngine::UpdateFpsOsd()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : lutEngines_)
    {
        LutEngine* eng = kv.second.get();
        if (!eng) continue;
        double fps = eng->IsRendering() ? eng->PresentFps() : -1.0;
        wchar_t buf[32];
        if (fps < 0.0) wcscpy(buf, L"— FPS");
        else swprintf(buf, 32, L"%.1f FPS", fps);
        eng->SetOsdText(buf);
    }
}

void FilterEngine::Shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    StopAll();
}

bool FilterEngine::EnsureMagReadyLocked()
{
    if (magUsed_) return true;
    if (!mag_.Initialize()) return false;
    magUsed_ = true;
    if (!mag_.Apply(FilterSettings())) // 先置单位矩阵
    {
        mag_.Uninitialize();
        magUsed_ = false;
        lastError_ = L"系统拒绝了全屏颜色效果（可能被组策略/安全策略禁用）";
        return false;
    }
    return true;
}

std::wstring FilterEngine::RunSelfTest()
{
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
    if (FAILED(hr))
    {
        // 无 GPU → 尝试 WARP 软件光栅化
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
    }
    if (FAILED(hr)) return Format(L"D3D11CreateDevice 失败 (0x%08X)", (unsigned)hr);
    // 这里只做设备可用性探测，不再预编译着色器：真正启滤镜时 LutEngine::CreatePipeline
    // 会编译（且 CompileShader 已按源码缓存）。启动阶段重复编译两遍纯属白花 UI 线程时间，
    // 也是「刚启动那一下卡顿」的一部分。着色器编译失败会在 LutEngine::Start 返回失败时回退。
    return L"";
}

} // namespace hsf
