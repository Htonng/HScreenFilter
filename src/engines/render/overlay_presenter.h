// overlay_presenter.h — 呈现层：覆盖层窗口 + flip 交换链 + Present。
//
// 这里是渲染链路的「输出边界」：
//   * 只负责窗口/交换链/后缓冲 RTV/Present/帧率 OSD；
//   * 不接触捕获纹理、LUT、FilterSettings；
//   * 保留两条硬性护栏：隐藏的 flip 交换链绝不 Present；跨线程窗口操作一律异步。
#pragma once
#include "render_types.h"
#include <dxgi1_2.h>
#include <atomic>

namespace hsf {

class OverlayPresenter
{
public:
    OverlayPresenter() = default;
    ~OverlayPresenter();

    bool Start(ID3D11Device* device, int x, int y, int width, int height);
    void Stop();

    // 分辨率变化：重建后缓冲并异步调整窗口几何。
    void SetSize(int width, int height);

    // 取当前后缓冲的 RTV。覆盖层不可见/交换链未就绪时返回 false（调用方不得 Present）。
    bool BeginFrame(ID3D11RenderTargetView** rtv);

    // 强制垂直同步 Present(1)。waitMs 返回 Present 自身阻塞时长。
    HRESULT Present(double* waitMs);

    bool IsVisible() const { return overlayVisible_.load(); }
    bool WindowVisible() const;
    void SetOverlayVisible(bool visible);
    // Dispose 在 join 渲染线程前调用：让 SetOverlayVisible 的「等窗口可见」循环
    // 立刻退出，避免 UI 线程阻塞在 join 时渲染线程还在等一个永远不会来的窗口消息。
    void BeginShutdown();

    void SetCapturable(bool capturable);
    void ApplyOverlayAffinity();

    void SetFpsDetail(bool on);
    void SetOsdText(const std::wstring& text);
    void UpdateOsd();

    HWND Hwnd() const { return hwnd_; }
    int X() const { return x_; }
    int Y() const { return y_; }
    int Width() const { return width_; }
    int Height() const { return height_; }
    ID3D11Texture2D* BackBufferTexture() const { return backBufferTex_.Get(); }

    std::wstring LastError;

private:
    void CreateOverlayWindow();
    bool CreateSwapChain();
    void CreateOsdWindow();
    ID3D11RenderTargetView* GetBackBufferRtv(ID3D11Texture2D* buffer);
    void ReleaseAll();

    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    ID3D11Device* device_ = nullptr;
    int x_ = 0, y_ = 0, width_ = 0, height_ = 0;

    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain1> swapChain_;
    // flip 模型后缓冲每帧在多个缓冲间轮转；按缓冲身份缓存 RTV，否则每帧重建会卡顿。
    struct BackBufferRtv { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; };
    BackBufferRtv backBufferRtv_[3];
    ComPtr<ID3D11Texture2D> backBufferTex_;

    std::atomic<bool> overlayVisible_{ true };
    std::atomic<bool> shuttingDown_{ false };
    bool capturable_ = false;
    bool presentSkipLogged_ = false;
    bool presentFailLogged_ = false;

    // 帧率 OSD（窗口属 UI 线程；渲染线程只读 showFpsOn_）
    HWND osd_ = nullptr;
    std::atomic<bool> showFpsOn_{ false };
    std::mutex osdMutex_;
    std::wstring osdText_;
    std::wstring osdDrawnText_;
    int osdDrawnX_ = 0, osdDrawnY_ = 0;
    bool osdShown_ = false;
};

} // namespace hsf
