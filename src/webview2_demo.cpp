// webview2_demo.cpp — WebView2 Flat Design 原型宿主（单文件演示）
// 动态加载 WebView2Loader.dll（不链接 SDK import lib），加载 webui/index.html。
// 用法: webview2_demo.exe [--webui <dir|index.html>]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>

#include "WebView2.h"

// ---------------------------------------------------------------------------
// 动态加载 WebView2Loader.dll（避免依赖 SDK 的 .lib / MSVC 工具链）
// ---------------------------------------------------------------------------
typedef HRESULT(STDAPICALLTYPE *PFN_CreateEnvWithOptions)(
    PCWSTR browserExecutableFolder, PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler);
typedef HRESULT(STDAPICALLTYPE *PFN_GetBrowserVersion)(PCWSTR browserExecutableFolder, LPWSTR *versionInfo);

static PFN_CreateEnvWithOptions g_createEnv = nullptr;
static PFN_GetBrowserVersion   g_getVersion = nullptr;

static bool LoadWebView2Loader()
{
    HMODULE mod = LoadLibraryW(L"WebView2Loader.dll");
    if (!mod)
    {
        mod = LoadLibraryW(L"WebView2Loader.dll"); // 再试一次（若路径问题）
        if (!mod) return false;
    }
    g_createEnv  = (PFN_CreateEnvWithOptions)GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions");
    g_getVersion = (PFN_GetBrowserVersion)GetProcAddress(mod, "GetAvailableCoreWebView2BrowserVersionString");
    return g_createEnv != nullptr;
}

// ---------------------------------------------------------------------------
// 日志（写到 exe 同目录 webview2_demo.log）
// ---------------------------------------------------------------------------
static std::wstring g_logPath;
static void Log(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    FILE *f = _wfopen(g_logPath.c_str(), L"a, ccs=UTF-8");
    if (f) { fwprintf(f, L"%s\n", buf); fclose(f); }
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

// ---------------------------------------------------------------------------
// 全局
// ---------------------------------------------------------------------------
static HWND g_hwnd = nullptr;
static ICoreWebView2Environment *g_env = nullptr;
static int g_wndW = 0, g_wndH = 0;   // 外框尺寸（已按 DPI 缩放）
static ICoreWebView2Controller *g_controller = nullptr;
static ICoreWebView2 *g_webview = nullptr;
static std::wstring g_url;

static std::wstring ExeDir();

static void ResizeWebView()
{
    if (!g_controller || !g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

// ---------------------------------------------------------------------------
// COM 回调（stdcall，WebView2 事件在创建线程的消息循环上回调）
// ---------------------------------------------------------------------------
class BrowserExitedHandler : public ICoreWebView2BrowserProcessExitedEventHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2BrowserProcessExitedEventHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(ICoreWebView2Environment *sender, ICoreWebView2BrowserProcessExitedEventArgs *args) override
    {
        COREWEBVIEW2_BROWSER_PROCESS_EXIT_KIND kind = COREWEBVIEW2_BROWSER_PROCESS_EXIT_KIND_NORMAL;
        UINT32 pid = 0;
        if (args)
        {
            args->get_BrowserProcessExitKind(&kind);
            args->get_BrowserProcessId(&pid);
        }
        Log(L"[browser-exit] kind=%d pid=%u (1=FAILED)", (int)kind, pid);
        return S_OK;
    }
};

class ProcessFailedHandler : public ICoreWebView2ProcessFailedEventHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2ProcessFailedEventHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(ICoreWebView2 *sender, ICoreWebView2ProcessFailedEventArgs *args) override
    {
        COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
        int exitCode = -1;
        if (args)
        {
            args->get_ProcessFailedKind(&kind);
            ICoreWebView2ProcessFailedEventArgs2 *a2 = nullptr;
            if (SUCCEEDED(args->QueryInterface(IID_ICoreWebView2ProcessFailedEventArgs2, (void **)&a2)) && a2)
            {
                a2->get_ExitCode(&exitCode);
                a2->Release();
            }
        }
        Log(L"[process-failed] kind=%d exitCode=%d", (int)kind, exitCode);
        return S_OK;
    }
};

class NavStartingHandler : public ICoreWebView2NavigationStartingEventHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2NavigationStartingEventHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) override
    {
        Log(L"[nav-start] uri=%s", g_url.c_str());
        return S_OK;
    }
};

class PreviewDoneHandler : public ICoreWebView2CapturePreviewCompletedHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CapturePreviewCompletedHandler)
        { *ppv = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(HRESULT result) override
    {
        Log(L"[preview] CapturePreview result=0x%08X (%s)", (unsigned)result,
            SUCCEEDED(result) ? L"OK" : L"FAIL");
        return S_OK;
    }
};

static void SavePreview()
{
    if (!g_webview) return;
    std::wstring outPath = ExeDir() + L"\\webview2_preview.png";
    IStream *stream = nullptr;
    if (FAILED(SHCreateStreamOnFileW(outPath.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, &stream)))
    {
        Log(L"[preview] cannot create stream for %s", outPath.c_str());
        return;
    }
    Log(L"[preview] capturing to %s ...", outPath.c_str());
    HRESULT hr = g_webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, new PreviewDoneHandler());
    if (FAILED(hr))
        Log(L"[preview] CapturePreview call failed: 0x%08X", (unsigned)hr);
    stream->Release();
}

class MsgReceivedHandler : public ICoreWebView2WebMessageReceivedEventHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2WebMessageReceivedEventHandler)
        {
            *ppv = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) override
    {
        LPWSTR json = nullptr;
        if (args && SUCCEEDED(args->get_WebMessageAsJson(&json)) && json)
        {
            Log(L"[web] %s", json);
            CoTaskMemFree(json);
        }
        return S_OK;
    }
};

class NavCompletedHandler : public ICoreWebView2NavigationCompletedEventHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2NavigationCompletedEventHandler)
        {
            *ppv = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) override
    {
        BOOL ok = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS err = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        if (args)
        {
            args->get_IsSuccess(&ok);
            args->get_WebErrorStatus(&err);
        }
        Log(L"[nav] success=%d errorStatus=%d uri=%s", ok ? 1 : 0, (int)err, g_url.c_str());
        if (g_hwnd)
            SetWindowTextW(g_hwnd, L"HScreenFilter · Flat Design（WebView2 原型）");
        if (ok)
            SavePreview();
        return S_OK;
    }
};

class ControllerCreatedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
        {
            *ppv = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Controller *controller) override
    {
        if (FAILED(result) || !controller)
        {
            Log(L"[init] CreateCoreWebView2Controller failed: 0x%08X", (unsigned)result);
            if (g_hwnd) DestroyWindow(g_hwnd);
            return S_OK;
        }
        ICoreWebView2 *webview = nullptr;
        if (FAILED(controller->get_CoreWebView2(&webview)) || !webview)
        {
            Log(L"[init] get_CoreWebView2 failed");
            if (g_hwnd) DestroyWindow(g_hwnd);
            return S_OK;
        }
        g_controller = controller;
        g_controller->AddRef();
        g_webview = webview;   // get_CoreWebView2 已带引用
        Log(L"[init] WebView2 controller created (result=0x%08X)", (unsigned)result);

        ResizeWebView();

        // 设置项：关闭状态栏 / 缩放 / 右键菜单（保持轻量）
        ICoreWebView2Settings *settings = nullptr;
        if (SUCCEEDED(webview->get_Settings(&settings)) && settings)
        {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->Release();
        }

        HRESULT hrNav = webview->add_NavigationStarting(new NavStartingHandler(), nullptr);
        HRESULT hrDone = webview->add_NavigationCompleted(new NavCompletedHandler(), nullptr);
        HRESULT hrMsg = webview->add_WebMessageReceived(new MsgReceivedHandler(), nullptr);
        HRESULT hrPf = webview->add_ProcessFailed(new ProcessFailedHandler(), nullptr);
        Log(L"[init] handlers: navStarting=0x%08X navCompleted=0x%08X msg=0x%08X",
            (unsigned)hrNav, (unsigned)hrDone, (unsigned)hrMsg);

        Log(L"[init] navigate: %s", g_url.c_str());
        webview->Navigate(g_url.c_str());
        return S_OK;
    }
};

class EnvCreatedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
public:
    ULONG ref_ = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
        {
            *ppv = this; AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Environment *env) override
    {
        if (FAILED(result) || !env)
        {
            Log(L"[init] CreateCoreWebView2EnvironmentWithOptions failed: 0x%08X", (unsigned)result);
            if (g_hwnd) DestroyWindow(g_hwnd);
            return S_OK;
        }
        g_env = env;
        g_env->AddRef();   // 保存环境引用，防止回调返回后被销毁导致控制器/浏览器关闭
        Log(L"[init] environment created (saved, ref kept)");
        ICoreWebView2Environment5 *env5 = nullptr;
        if (SUCCEEDED(env->QueryInterface(IID_ICoreWebView2Environment5, (void **)&env5)) && env5)
        {
            HRESULT hrBe = env5->add_BrowserProcessExited(new BrowserExitedHandler(), nullptr);
            Log(L"[init] add_BrowserProcessExited=0x%08X", (unsigned)hrBe);
            env5->Release();
        }
        env->CreateCoreWebView2Controller(g_hwnd, new ControllerCreatedHandler());
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------
static void InitWebView2(const std::wstring &userDataDir)
{
    if (!g_createEnv)
    {
        Log(L"[init] WebView2Loader.dll 加载失败（请将 WebView2Loader.dll 放在 exe 旁）");
        return;
    }
    LPWSTR ver = nullptr;
    if (g_getVersion && SUCCEEDED(g_getVersion(nullptr, &ver)) && ver)
    {
        Log(L"[init] WebView2 runtime version: %s", ver);
        CoTaskMemFree(ver);
    }
    g_createEnv(nullptr, userDataDir.c_str(), nullptr, new EnvCreatedHandler());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;
    case WM_SIZE:
        ResizeWebView();
        return 0;
    case WM_DPICHANGED:
    {
        RECT *rc = (RECT *)lp;
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                     rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeWebView();
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = g_wndW;
        mmi->ptMinTrackSize.y = g_wndH;
        mmi->ptMaxTrackSize = mmi->ptMinTrackSize;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_webview) { g_webview->Release(); g_webview = nullptr; }
        if (g_controller) { g_controller->Release(); g_controller = nullptr; }
        if (g_env) { g_env->Release(); g_env = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::wstring ExeDir()
{
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static std::wstring ToFileUrl(const std::wstring &path)
{
    std::wstring p = path;
    for (auto &c : p) if (c == L'\\') c = L'/';
    return L"file:///" + p;
}

static void ResolveWebUi(std::wstring &outPath, std::wstring &outUrl)
{
    // 1) exe 旁 webui/index.html
    std::wstring cand = ExeDir() + L"\\webui\\index.html";
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        outPath = cand;
        outUrl = ToFileUrl(cand);
        return;
    }
    // 2) 仓库内 webui/index.html
    cand = L"F:\\code\\HScreenFilter\\webui\\index.html";
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        outPath = cand;
        outUrl = ToFileUrl(cand);
        return;
    }
    outPath = L"<not found>";
    outUrl = L"about:blank";
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    // PerMonitorV2 DPI（记录结果，便于排查）
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI *PFN_SetProcessDpiAwarenessContext)(void *);
    auto setDpi = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    BOOL dpiOk = FALSE;
    if (setDpi)
    {
        dpiOk = setDpi((void *)-4 /*DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2*/);
        if (!dpiOk) Log(L"[init] SetProcessDpiAwarenessContext failed: %lu", GetLastError());
    }
    else
    {
        SetProcessDPIAware();
        Log(L"[init] SetProcessDpiAwarenessContext not available, fallback SetProcessDPIAware");
    }

    // 命令行 --webui
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coInit))
        Log(L"[init] CoInitializeEx failed: 0x%08X (continue)", (unsigned)coInit);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring webuiPath;
    for (int i = 1; i + 1 < argc; i++)
        if (_wcsicmp(argv[i], L"--webui") == 0) webuiPath = argv[i + 1];
    if (argv) LocalFree(argv);

    g_logPath = ExeDir() + L"\\webview2_demo.log";
    Log(L"=== HScreenFilter WebView2 demo start ===");

    if (!LoadWebView2Loader())
    {
        Log(L"[init] 未找到 WebView2Loader.dll");
        MessageBoxW(nullptr, L"未找到 WebView2Loader.dll，请将其放在本程序同目录。",
                    L"WebView2 原型", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::wstring wndClass = L"HSFWebView2DemoWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = wndClass.c_str();
    RegisterClassExW(&wc);

    // 客户区 680x860（DIP @96），固定尺寸
    HDC hdc = GetDC(nullptr);
    UINT dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    if (dpi < 96) dpi = 96;
    int w = MulDiv(680, (int)dpi, 96);
    int h = MulDiv(860, (int)dpi, 96);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    g_wndW = rc.right - rc.left;
    g_wndH = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, wndClass.c_str(), L"HScreenFilter · Flat Design（WebView2 原型）",
                                style,
                                (sx - g_wndW) / 2, (sy - g_wndH) / 3,
                                g_wndW, g_wndH,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd)
    {
        Log(L"[init] CreateWindow failed: %lu", GetLastError());
        return 1;
    }
    g_hwnd = hwnd;

    {
        HDC hdc2 = GetDC(hwnd);
        int dpiX = GetDeviceCaps(hdc2, LOGPIXELSX);
        ReleaseDC(hwnd, hdc2);
        RECT cr; GetClientRect(hwnd, &cr);
        Log(L"[init] dpiAware=%d logpixelsX=%d client=%dx%d", dpiOk ? 1 : 0, dpiX,
            cr.right - cr.left, cr.bottom - cr.top);
    }

    // WebUI 解析
    std::wstring pagePath, pageUrl;
    if (!webuiPath.empty())
    {
        if (GetFileAttributesW(webuiPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            webuiPath += L"\\index.html";
        pagePath = webuiPath;
        pageUrl = ToFileUrl(webuiPath);
    }
    else
        ResolveWebUi(pagePath, pageUrl);
    g_url = pageUrl;
    Log(L"[init] webui: %s", pagePath.c_str());

    // 用户数据目录
    std::wstring userData = L"";
    wchar_t la[512];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, 512))
        userData = std::wstring(la) + L"\\HScreenFilter\\WebView2Demo";

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    InitWebView2(userData);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Log(L"=== demo exit ===");
    if (SUCCEEDED(coInit)) CoUninitialize();
    return (int)msg.wParam;
}
