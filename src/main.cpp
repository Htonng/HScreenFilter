// main.cpp — 入口：DPI 感知、单实例、自检模式、主窗口消息循环。
#include "common.h"
#include "log.h"
#include "models.h"
#include "monitors.h"
#include "json.h"
#include "ui/main_window.h"
#include "engines/hlsl.h"
#include "engines/filter_engine.h"

namespace hsf {

// 引擎测试：启动 DXGI LUT 引擎 → 应用非中性滤镜 → 短暂运行 → 关闭。
// 用于验证覆盖层/DXGI 捕获/LUT 重建/呈现 全链路（会在当前显示器短暂应用滤镜）。
static int RunEngineTest()
{
    FILE* logf = _wfopen((ExeDir() + L"\\enginetest.log").c_str(), L"w, ccs=UTF-8");
    auto out = [logf](const std::wstring& line) {
        fputws(line.c_str(), stdout);
        fputwc(L'\n', stdout);
        if (logf) { fputws(line.c_str(), logf); fputwc(L'\n', logf); }
    };

    auto monitors = Monitors::Enumerate();
    if (monitors.empty())
    {
        out(L"[FAIL] 未检测到显示器");
        return 1;
    }
    auto& fe = FilterEngine::Instance();
    fe.SetUseDxgi(true);
    if (!fe.Initialize())
    {
        out(L"[FAIL] 引擎初始化失败: " + fe.LastError());
        return 1;
    }
    out(L"[ OK ] 引擎探测通过，kind=" + NumToStr((double)(int)fe.Kind()));

    // 非中性设置：亮度 -20 + 红色系色相 +30
    FilterSettings s;
    s.Brightness = -20;
    s.Contrast = 110;
    if (auto* ch = s.FindChannel(HslChannelNames::Red)) ch->Hue = 30;

    if (!fe.Apply(0, monitors[0], s))
    {
        out(L"[FAIL] 引擎应用失败: " + fe.LastError());
        return 1;
    }
    out(L"[ OK ] 引擎已应用（LUT 覆盖层启动）");

    // 运行 4 秒（渲染线程重建 LUT 并呈现）
    Sleep(4000);

    fe.Reset();
    fe.Shutdown();
    out(L"[ OK ] 引擎已关闭");
    if (logf) fclose(logf);
    return 0;
}

static int RunSelfTest()
{
    // GUI 子系统程序没有有效 stdout，结果同时写入 selftest.log
    FILE* logf = _wfopen((ExeDir() + L"\\selftest.log").c_str(), L"w, ccs=UTF-8");
    auto out = [logf](const std::wstring& line) {
        fputws(line.c_str(), stdout);
        fputwc(L'\n', stdout);
        if (logf) { fputws(line.c_str(), logf); fputwc(L'\n', logf); }
    };
    int fails = 0;

    // 1) JSON 往返
    {
        ProfileData data;
        data.IsEnabled = true;
        data.UseDxgi = true;
        data.Theme = L"mica";
        data.GlobalKey = 0x31;
        data.GlobalModifiers = 0x2;
        data.GlobalDisplay = L"Ctrl+1";
        data.Current.Brightness = -12.5;
        data.Current.HslSaturation = 150;
        data.Current.FindChannel(HslChannelNames::Red)->Hue = 30;
        Profile p;
        p.Name = L"护眼";
        p.HotkeyKey = 0x32;
        p.HotkeyDisplay = L"Ctrl+2";
        p.UseDxgi = false;
        data.Profiles.push_back(p);
        AppBinding b;
        b.ProcessName = L"chrome";
        b.WindowTitle = L"哔哩";
        b.ProfileIndex = 0;
        data.AppBindings.push_back(b);
        DisplayState d;
        d.Index = 0;
        d.IsEnabled = true;
        d.ActiveProfileIndex = 0;
        data.Displays.push_back(d);

        std::wstring json = data.ToJson().Serialize(true);
        JsonValue root;
        if (!JsonValue::Parse(json, root))
        {
            out(L"[FAIL] JSON 解析失败");
            fails++;
        }
        else
        {
            ProfileData back;
            back.FromJson(root);
            bool ok = back.IsEnabled == true && back.UseDxgi == true &&
                      back.Theme == L"mica" && back.GlobalDisplay == L"Ctrl+1" &&
                      back.Current.Brightness == -12.5 &&
                      back.Current.HslSaturation == 150 &&
                      back.Current.FindChannel(HslChannelNames::Red) &&
                      back.Current.FindChannel(HslChannelNames::Red)->Hue == 30 &&
                      back.Profiles.size() == 1 && back.Profiles[0].Name == L"护眼" &&
                      back.Profiles[0].HotkeyDisplay == L"Ctrl+2" &&
                      back.Profiles[0].UseDxgi == false &&
                      back.AppBindings.size() == 1 && back.AppBindings[0].ProcessName == L"chrome" &&
                      back.AppBindings[0].WindowTitle == L"哔哩" &&
                      back.Displays.size() == 1 && back.Displays[0].IsEnabled;
            if (!ok)
            {
                out(L"[FAIL] JSON 往返数据不一致");
                fails++;
            }
            else
            {
                out(L"[ OK ] JSON 序列化/解析往返一致");
            }
        }
    }

    // 2) HLSL 着色器编译（d3dcompiler_47.dll）
    {
        std::wstring err;
        ComPtr<ID3DBlob> blob;
        if (!CompileShader(g_psLutSource, "main", "ps_4_0", blob, err))
        {
            out(L"[FAIL] 像素着色器编译失败: " + err);
            fails++;
        }
        else
        {
            out(L"[ OK ] 像素着色器编译成功 (ps_4_0)");
        }
        if (!CompileShader(g_csLutSource, "CSMain", "cs_5_0", blob, err))
        {
            out(L"[FAIL] 计算着色器编译失败: " + err);
            fails++;
        }
        else
        {
            out(L"[ OK ] 计算着色器编译成功 (cs_5_0)");
        }
    }

    // 3) D3D11 设备
    {
        ComPtr<ID3D11Device> device;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
        const wchar_t* driver = L"硬件";
        if (FAILED(hr))
        {
            // 无 GPU（虚拟机/远程桌面）→ WARP 软件光栅化
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
            driver = L"WARP(软件)";
        }
        if (FAILED(hr))
        {
            out(Format(L"[FAIL] D3D11CreateDevice 失败 (0x%08X)", (unsigned)hr));
            fails++;
        }
        else
        {
            out(Format(L"[ OK ] D3D11 设备创建成功（%s）", driver));
        }
    }

    // 4) 显示器枚举
    {
        auto monitors = Monitors::Enumerate();
        out(Format(L"[INFO] 检测到 %d 台显示器", (int)monitors.size()));
    }

    // 5) 滤镜矩阵构建（放大镜引擎语义）
    {
        FilterSettings s;
        s.Brightness = -10;
        s.Contrast = 110;
        s.Temperature = 25;
        s.HslSaturation = 130;
        auto m = hsf::MagEngine::BuildMatrix(s);
        out(L"[ OK ] 5×5 颜色矩阵构建成功");
    }

    if (logf) fclose(logf);
    if (fails == 0)
    {
        out(L"\n自检全部通过 ✔");
        return 0;
    }
    out(L"\n自检失败，请查看上方 [FAIL] 项");
    return 1;
}

} // namespace hsf

// 崩溃记录：把异常信息追加到 %LocalAppData%\HScreenFilter\debug.log
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    try
    {
        std::wstring dir = hsf::HScreenFilterDataDir();
        HANDLE f = CreateFileW((dir + L"\\debug.log").c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            // 定位异常地址所在模块
            HMODULE mod = nullptr;
            wchar_t modPath[MAX_PATH] = L"unknown";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod) && mod)
            {
                GetModuleFileNameW(mod, modPath, MAX_PATH);
            }
            std::wstring msg = hsf::Format(L"\n[CRASH] 未处理异常 0x%08X @0x%p (%s) base=0x%p\n",
                                           (unsigned)ep->ExceptionRecord->ExceptionCode,
                                           ep->ExceptionRecord->ExceptionAddress, modPath, (void*)mod);
            std::string utf8 = hsf::WideToUtf8(msg);
            DWORD written = 0;
            WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            CloseHandle(f);
        }
    }
    catch (...)
    {
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    using namespace hsf;

    // 按显示器 DPI 感知（否则 2K/4K 高分屏被虚拟化为 1080p，覆盖层/捕获分辨率不对；
    // 清单中已声明 PerMonitorV2，此调用为兜底）
    BOOL dpiOk = SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    Log::WriteFmt(L"Main", L"DPI 感知设置: ok=%d err=%u", dpiOk ? 1 : 0, (unsigned)GetLastError());
    SetUnhandledExceptionFilter(CrashFilter);

    // 单实例
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"HScreenFilter_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // 命令行参数
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool selftest = false;
    bool enginetest = false;
    if (argv)
    {
        for (int i = 1; i < argc; i++)
        {
            if (_wcsicmp(argv[i], L"--selftest") == 0) selftest = true;
            if (_wcsicmp(argv[i], L"--enginetest") == 0) enginetest = true;
        }
        LocalFree(argv);
    }

    if (selftest)
    {
        int rc = RunSelfTest();
        CloseHandle(mutex);
        return rc;
    }
    if (enginetest)
    {
        int rc = RunEngineTest();
        CloseHandle(mutex);
        return rc;
    }

    try
    {
        Log::Write(L"Main", L"启动");
        MainWindow win;
        if (!win.Create(hInstance))
        {
            Log::Write(L"Main", L"主窗口创建失败");
            CloseHandle(mutex);
            return 1;
        }
        win.Show();
        Log::Write(L"Main", L"窗口已显示，进入消息循环");
        MSG msg;
        BOOL gm;
        while ((gm = GetMessageW(&msg, nullptr, 0, 0)) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Log::WriteFmt(L"Main", L"消息循环退出: GetMessage=%d err=%u 窗口存在=%d",
                      (int)gm, (unsigned)GetLastError(), IsWindow(win.Hwnd()) ? 1 : 0);
        win.Destroy();
        Log::Write(L"Main", L"退出");
    }
    catch (...)
    {
        try { Log::Write(L"Main", L"启动异常"); } catch (...) {}
    }
    CloseHandle(mutex);
    return 0;
}
