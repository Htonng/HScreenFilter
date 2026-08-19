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
            kind_ = EngineKind::PixelShader;
            return true;
        }
        lastError_ = L"DXGI 着色器引擎不可用：" + selfErr + L"，已回退到放大镜引擎";
    }

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
        kind_ = EngineKind::GammaRamp;
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
    if (useDxgi_ == useDxgi) return;
    useDxgi_ = useDxgi;

    // 停止当前引擎
    StopAll();
    checked_ = false;
    kind_ = EngineKind::None;
}

void FilterEngine::StopAll()
{
    for (auto& kv : lutEngines_)
    {
        if (kv.second) kv.second->Dispose();
    }
    lutEngines_.clear();
    if (magUsed_)
    {
        mag_.Reset();
        mag_.Uninitialize();
        magUsed_ = false;
    }
    try { GammaEngine::Reset(); } catch (...) {}
}

bool FilterEngine::Apply(int displayIndex, const DisplayMonitor& display, const FilterSettings& s)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!InitializeLocked()) return false;

    if (kind_ == EngineKind::PixelShader)
    {
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
                Log::WriteFmt(L"FilterEngine", L"LutEngine Start 失败: %s", lastError_.c_str());
                lutEngines_.erase(displayIndex);
                // 回退到放大镜/伽马引擎
                kind_ = EnsureMagReadyLocked() ? EngineKind::FullScreenColorEffect : EngineKind::GammaRamp;
            }
            else
            {
                eng->Capturable = capturable_;
                engine = eng.get();
                lutEngines_[displayIndex] = std::move(eng);
            }
        }
        if (engine)
        {
            engine->Apply(s);
            return true;
        }
    }

    if (kind_ == EngineKind::FullScreenColorEffect)
    {
        return mag_.Apply(s);
    }
    if (kind_ == EngineKind::GammaRamp)
    {
        if (s.Saturation != 100.0)
        {
            Log::Write(L"FilterEngine",
                       L"注意：当前使用伽马曲线引擎，鲜艳度（饱和度）无法生效；"
                       L"启用 LUT 引擎或改用放大镜引擎后鲜艳度才可用");
        }
        return GammaEngine::Apply(s);
    }
    return false;
}

bool FilterEngine::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    bool ok = true;
    // DXGI 覆盖层是本应用自建窗口：无论当前引擎是什么都停止（幂等）
    for (auto& kv : lutEngines_)
    {
        if (kv.second) kv.second->Dispose();
    }
    lutEngines_.clear();
    // 放大镜：仅当本应用初始化过才复位
    if (magUsed_)
    {
        mag_.Reset();
    }
    // 伽马：仅当正在使用伽马引擎才复位
    if (kind_ == EngineKind::GammaRamp)
    {
        ok = GammaEngine::Reset() && ok;
    }
    return ok;
}

void FilterEngine::ResetDisplay(int displayIndex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (kind_ != EngineKind::PixelShader) return;
    auto it = lutEngines_.find(displayIndex);
    if (it != lutEngines_.end())
    {
        it->second->Dispose();
        lutEngines_.erase(it);
    }
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
    auto it = lutEngines_.find(displayIndex);
    if (it != lutEngines_.end() && it->second)
    {
        it->second->UseVsync = useVsync;
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
    mag_.Apply(FilterSettings()); // 先置单位矩阵
    return true;
}

std::wstring FilterEngine::RunSelfTest()
{
    std::wstring err;
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
    ComPtr<ID3DBlob> blob;
    if (!CompileShader(g_psLutSource, "main", "ps_4_0", blob, err)) return L"像素着色器编译失败: " + err;
    if (!CompileShader(g_csLutSource, "CSMain", "cs_5_0", blob, err)) return L"计算着色器编译失败: " + err;
    return L"";
}

} // namespace hsf
