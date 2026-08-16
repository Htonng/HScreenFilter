#include "dialogs.h"
#include "comptr.h"
#include <shobjidl.h>
#include <shlwapi.h>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace hsf {

// ---------------- 深色对话框基础 ----------------
namespace {

constexpr DWORD kDialogBg = 0x1E1E1E;

struct DialogBase
{
    HWND hwnd = nullptr;
    bool done = false;
    bool result = false;
    HBRUSH bgBrush = nullptr;
    HBRUSH editBrush = nullptr;

    void InitBrushes()
    {
        bgBrush = CreateSolidBrush(kDialogBg);
        editBrush = CreateSolidBrush(0x202020);
    }
    void FreeBrushes()
    {
        if (bgBrush) { DeleteObject(bgBrush); bgBrush = nullptr; }
        if (editBrush) { DeleteObject(editBrush); editBrush = nullptr; }
    }
};

COLORREF Mix(DWORD base, BYTE white) // 在实色底上叠加白色
{
    BYTE r = (BYTE)((base >> 16) & 0xFF), g = (BYTE)((base >> 8) & 0xFF), b = (BYTE)(base & 0xFF);
    float a = white / 255.f;
    r = (BYTE)(r + (255 - r) * a);
    g = (BYTE)(g + (255 - g) * a);
    b = (BYTE)(b + (255 - b) * a);
    return RGB(r, g, b);
}

// 通用深色处理
void HandleCtlColor(HWND hDlg, WPARAM wParam, LPARAM lParam, DialogBase* base, LRESULT& handled)
{
    HDC hdc = (HDC)wParam;
    HWND ctl = (HWND)lParam;
    SetTextColor(hdc, RGB(0xEE, 0xEE, 0xEE));
    SetBkColor(hdc, kDialogBg);
    if (base && base->bgBrush)
    {
        handled = (LRESULT)base->bgBrush;
    }
}

// 居中于 owner
void CenterWindow(HWND hwnd, HWND owner)
{
    RECT ow{};
    if (owner) GetWindowRect(owner, &ow);
    else ow = { 0, 0, 1280, 800 };
    RECT w{};
    GetWindowRect(hwnd, &w);
    int ww = w.right - w.left, wh = w.bottom - w.top;
    int cx = ow.left + (ow.right - ow.left - ww) / 2;
    int cy = ow.top + (ow.bottom - ow.top - wh) / 2;
    SetWindowPos(hwnd, nullptr, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

// ---------------- 输入文本对话框 ----------------
namespace {

struct PromptData : DialogBase
{
    HWND edit = nullptr;
    std::wstring placeholder;
    std::wstring initial;
    std::wstring resultText;
};

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* d = (PromptData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_NCCREATE:
        d = (PromptData*)((CREATESTRUCTW*)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        return TRUE;
    case WM_CREATE:
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORDLG:
    {
        LRESULT hr = 0;
        HandleCtlColor(hwnd, wParam, lParam, d, hr);
        return hr;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            int len = GetWindowTextLengthW(d->edit);
            if (len > 0)
            {
                std::wstring buf(len + 1, L'\0');
                GetWindowTextW(d->edit, &buf[0], len + 1);
                buf.resize(len);
                d->resultText = Trim(buf);
            }
            d->resultText = Trim(d->resultText);
            if (!d->resultText.empty())
            {
                d->result = true;
                d->done = true;
                DestroyWindow(hwnd);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            d->result = false;
            d->done = true;
            DestroyWindow(hwnd);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        d->result = false;
        d->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        d->FreeBrushes();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool PromptTextDialog(HWND owner, const std::wstring& title, const std::wstring& placeholder,
                      const std::wstring& initial, std::wstring& outText)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HSF_PromptDialog";
    wc.hbrBackground = (HBRUSH)(kDialogBg + 1);
    RegisterClassExW(&wc);

    PromptData data;
    data.InitBrushes();
    data.placeholder = placeholder;
    data.initial = initial;

    HWND dlg = CreateWindowExW(0, L"HSF_PromptDialog", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               0, 0, 380, 140, owner, nullptr, wc.hInstance, &data);
    if (!dlg) { data.FreeBrushes(); return false; }
    data.hwnd = dlg;

    // 标签
    CreateWindowExW(0, L"STATIC", placeholder.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                    16, 16, 340, 20, dlg, nullptr, wc.hInstance, nullptr);
    // 输入框
    data.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                16, 42, 340, 26, dlg, nullptr, wc.hInstance, nullptr);
    // 按钮
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    200, 84, 70, 30, dlg, (HMENU)IDOK, wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    286, 84, 70, 30, dlg, (HMENU)IDCANCEL, wc.hInstance, nullptr);

    CenterWindow(dlg, owner);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(data.edit);

    // 模态循环
    while (!data.done)
    {
        MSG msg;
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    bool ok = data.result;
    if (ok) outText = data.resultText;
    return ok;
}

// ---------------- 添加/编辑检测应用对话框 ----------------
namespace {

struct BindingData : DialogBase
{
    HWND procEdit = nullptr, titleEdit = nullptr;
    HWND combo = nullptr;
    std::vector<std::wstring> profiles;
    AppBinding binding;
};

LRESULT CALLBACK BindingProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* d = (BindingData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_NCCREATE:
        d = (BindingData*)((CREATESTRUCTW*)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORLISTBOX:
    {
        LRESULT hr = 0;
        HandleCtlColor(hwnd, wParam, lParam, d, hr);
        return hr;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            int len = GetWindowTextLengthW(d->procEdit);
            if (len > 0)
            {
                std::wstring buf(len + 1, L'\0');
                GetWindowTextW(d->procEdit, &buf[0], len + 1);
                buf.resize(len);
                d->binding.ProcessName = Trim(buf);
            }
            len = GetWindowTextLengthW(d->titleEdit);
            if (len > 0)
            {
                std::wstring buf(len + 1, L'\0');
                GetWindowTextW(d->titleEdit, &buf[0], len + 1);
                buf.resize(len);
                d->binding.WindowTitle = Trim(buf);
            }
            int sel = (int)SendMessageW(d->combo, CB_GETCURSEL, 0, 0);
            if (sel == 0) d->binding.ProfileIndex = -1;
            else if (sel > 0 && sel <= (int)d->profiles.size()) d->binding.ProfileIndex = sel - 1;
            if (!d->binding.ProcessName.empty())
            {
                d->result = true;
                d->done = true;
                DestroyWindow(hwnd);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            d->result = false;
            d->done = true;
            DestroyWindow(hwnd);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        d->result = false;
        d->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        d->FreeBrushes();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool EditBindingDialog(HWND owner, const std::wstring& title, const std::vector<std::wstring>& profiles,
                       AppBinding& binding)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BindingProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HSF_BindingDialog";
    wc.hbrBackground = (HBRUSH)(kDialogBg + 1);
    RegisterClassExW(&wc);

    BindingData data;
    data.InitBrushes();
    data.profiles = profiles;
    data.binding = binding;

    HWND dlg = CreateWindowExW(0, L"HSF_BindingDialog", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               0, 0, 400, 260, owner, nullptr, wc.hInstance, &data);
    if (!dlg) { data.FreeBrushes(); return false; }
    data.hwnd = dlg;

    auto label = [&](const wchar_t* text, int y) {
        CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                        16, y, 360, 20, dlg, nullptr, wc.hInstance, nullptr);
    };
    label(L"进程名（不含 .exe）", 14);
    data.procEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", binding.ProcessName.c_str(),
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                    16, 36, 360, 26, dlg, nullptr, wc.hInstance, nullptr);
    label(L"窗口标题（可选）", 72);
    data.titleEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", binding.WindowTitle.c_str(),
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                     16, 94, 360, 26, dlg, nullptr, wc.hInstance, nullptr);
    label(L"该进程在前台时自动应用的配置", 130);
    data.combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                 16, 152, 360, 200, dlg, nullptr, wc.hInstance, nullptr);
    SendMessageW(data.combo, CB_ADDSTRING, 0, (LPARAM)L"（不切换配置，按当前设置）");
    int selIdx = 0;
    for (size_t i = 0; i < profiles.size(); i++)
    {
        SendMessageW(data.combo, CB_ADDSTRING, 0, (LPARAM)profiles[i].c_str());
        if ((int)i == binding.ProfileIndex) selIdx = (int)i + 1;
    }
    SendMessageW(data.combo, CB_SETCURSEL, selIdx, 0);

    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    220, 200, 70, 30, dlg, (HMENU)IDOK, wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    306, 200, 70, 30, dlg, (HMENU)IDCANCEL, wc.hInstance, nullptr);

    CenterWindow(dlg, owner);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(data.procEdit);

    while (!data.done)
    {
        MSG msg;
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (data.result) binding = data.binding;
    return data.result;
}

// ---------------- 确认对话框 ----------------
namespace {

struct ConfirmData : DialogBase
{
    std::wstring message;
    std::wstring primary;
    std::wstring close;
    bool autoClose = false;
    int autoCountdown = 0;
};

LRESULT CALLBACK ConfirmProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* d = (ConfirmData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_NCCREATE:
        d = (ConfirmData*)((CREATESTRUCTW*)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG:
    {
        LRESULT hr = 0;
        HandleCtlColor(hwnd, wParam, lParam, d, hr);
        return hr;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            d->result = true;
            d->done = true;
            DestroyWindow(hwnd);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            d->result = false;
            d->done = true;
            DestroyWindow(hwnd);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        d->result = false;
        d->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        d->FreeBrushes();
        return 0;
    case WM_TIMER:
        if (d->autoClose && d->autoCountdown > 0)
        {
            d->autoCountdown--;
            HWND txt = GetDlgItem(hwnd, 100);
            if (txt)
            {
                SetWindowTextW(txt, (std::to_wstring(d->autoCountdown) + L"\n\n" + d->message).c_str());
            }
            if (d->autoCountdown <= 0)
            {
                d->result = true;
                d->done = true;
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool ConfirmDialog(HWND owner, const std::wstring& title, const std::wstring& message,
                   const std::wstring& primaryText, const std::wstring& closeText)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ConfirmProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HSF_ConfirmDialog";
    wc.hbrBackground = (HBRUSH)(kDialogBg + 1);
    RegisterClassExW(&wc);

    ConfirmData data;
    data.InitBrushes();
    data.message = message;
    data.primary = primaryText;
    data.close = closeText;

    HWND dlg = CreateWindowExW(0, L"HSF_ConfirmDialog", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               0, 0, 400, 150, owner, nullptr, wc.hInstance, &data);
    if (!dlg) { data.FreeBrushes(); return false; }
    data.hwnd = dlg;

    CreateWindowExW(0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_WORDELLIPSIS,
                    16, 14, 360, 60, dlg, (HMENU)100, wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", primaryText.c_str(), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    200, 92, 80, 30, dlg, (HMENU)IDOK, wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", closeText.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    296, 92, 80, 30, dlg, (HMENU)IDCANCEL, wc.hInstance, nullptr);

    CenterWindow(dlg, owner);
    ShowWindow(dlg, SW_SHOW);

    while (!data.done)
    {
        MSG msg;
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return data.result;
}

// ---------------- 前台应用倒计时对话框 ----------------
bool ForegroundCountdownDialog(HWND owner)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ConfirmProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HSF_CountdownDialog";
    wc.hbrBackground = (HBRUSH)(kDialogBg + 1);
    RegisterClassExW(&wc);

    ConfirmData data;
    data.InitBrushes();
    data.autoClose = true;
    data.autoCountdown = 5;
    data.message = L"请在倒计时结束前切换到要添加的进程（当前窗口在前台即被捕获），倒计时结束后自动添加。";

    HWND dlg = CreateWindowExW(0, L"HSF_CountdownDialog", L"从前台窗口添加",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               0, 0, 420, 180, owner, nullptr, wc.hInstance, &data);
    if (!dlg) { data.FreeBrushes(); return false; }
    data.hwnd = dlg;

    CreateWindowExW(0, L"STATIC", L"5\n\n请在倒计时结束前切换到要添加的进程（当前窗口在前台即被捕获），倒计时结束后自动添加。",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    16, 10, 380, 110, dlg, (HMENU)100, wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    310, 126, 80, 30, dlg, (HMENU)IDCANCEL, wc.hInstance, nullptr);

    CenterWindow(dlg, owner);
    ShowWindow(dlg, SW_SHOW);
    SetTimer(dlg, 1, 1000, nullptr);

    while (!data.done)
    {
        MSG msg;
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // true = 倒计时自然结束（可继续添加）；false = 用户取消
    return data.result;
}

// ---------------- 导入/导出 ----------------
bool ImportProfileDialog(HWND owner, std::wstring& fileName, std::wstring& jsonText)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needUninit = SUCCEEDED(hr);
    bool ok = false;
    ComPtr<IFileOpenDialog> dlg;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(dlg.GetAddressOf()))))
    {
        COMDLG_FILTERSPEC filters[] = { { L"HScreenFilter 配置", L"*.json" } };
        dlg->SetFileTypes(1, filters);
        if (SUCCEEDED(dlg->Show(owner)))
        {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dlg->GetResult(item.GetAddressOf())))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                {
                    fileName = path;
                    CoTaskMemFree(path);
                    // 用 _wfopen 读取（宽字符路径）
                    FILE* f = _wfopen(fileName.c_str(), L"rb");
                    if (f)
                    {
                        fseek(f, 0, SEEK_END);
                        long len = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        if (len > 0 && len < 16 * 1024 * 1024)
                        {
                            std::string bytes((size_t)len, '\0');
                            size_t rd = fread(&bytes[0], 1, (size_t)len, f);
                            bytes.resize(rd);
                            jsonText = Utf8ToWide(bytes);
                            ok = true;
                        }
                        fclose(f);
                    }
                }
            }
        }
    }
    if (needUninit) CoUninitialize();
    return ok;
}

bool ExportProfileDialog(HWND owner, const std::wstring& suggestedName, const std::wstring& jsonText)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needUninit = SUCCEEDED(hr);
    bool ok = false;
    ComPtr<IFileSaveDialog> dlg;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(dlg.GetAddressOf()))))
    {
        COMDLG_FILTERSPEC filters[] = { { L"HScreenFilter 配置", L"*.json" } };
        dlg->SetFileTypes(1, filters);
        dlg->SetDefaultExtension(L"json");
        dlg->SetFileName(suggestedName.c_str());
        if (SUCCEEDED(dlg->Show(owner)))
        {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dlg->GetResult(item.GetAddressOf())))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                {
                    std::wstring file = path;
                    CoTaskMemFree(path);
                    FILE* f = _wfopen(file.c_str(), L"wb");
                    if (f)
                    {
                        std::string bytes = WideToUtf8(jsonText);
                        fwrite(bytes.data(), 1, bytes.size(), f);
                        fclose(f);
                        ok = true;
                    }
                }
            }
        }
    }
    if (needUninit) CoUninitialize();
    return ok;
}

} // namespace hsf
