// lut_engine.h — D3D11 覆盖层 + DXGI Desktop Duplication 捕获 + 3D LUT 滤镜引擎。
//
// 与旧版 VorticeHslEngine 的区别（需求 3：提高 HSL 分色系调整整体效率 / 换实现思路）：
//   旧版：每像素每帧执行 完整 RGB→HSL 掩码 + RGB→OKLCH + 调色 + OKLCH→RGB + 基础调节
//         （每像素约 10+ 个超越函数：pow/atan2/sin/cos）。
//   新版：参数变化时用 计算着色器 对 64^3 个格子点各算一次（GPU 并行，亚毫秒级），
//         生成 3D LUT；每帧像素着色器只做一次三线性采样 + 屏幕空间抖动。
//         中性参数时直接 CopyResource 直通，连采样都省掉。
#pragma once
#include "common.h"
#include "comptr.h"
#include "models.h"
#include "hlsl.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>

namespace hsf {

// 查询坐标所在显示器当前是否处于 HDR（HDR10 PQ / scRGB）状态。
// 供 UI 判断「放大镜/伽马引擎在 HDR 下会过曝、文字发虚」并给出提示。
bool QueryDisplayHdr(int x, int y);

class LutEngine
{
public:
    LutEngine() = default;
    ~LutEngine();

    // 启动：创建覆盖层窗口、交换链、着色器、捕获。失败返回 false 并记录 LastError。
    bool Start(int x, int y, int width, int height, int outputIndex);
    void Dispose();

    // 更新滤镜参数（线程安全：写入参数缓冲，渲染线程在重建 LUT 时读取）
    void Apply(const FilterSettings& s);

    // 垂直同步开关（历史字段）：LUT 覆盖层固定用 Present(1) 由 DWM 对齐 vblank，
    // 所以本字段只作记录，不再参与绘制决策。
    bool UseVsync = false;
    bool Capturable = false;      // 覆盖层可被 OBS 等捕获（WDA_MONITOR）
    bool IsRendering() const { return running_ && renderThread_.joinable(); }

    // 滤镜实时呈现帧率：最近一个统计窗口内的呈现帧数/秒。
    // 0 表示尚无样本（画面完全静止时桌面复制不产帧，呈现帧率会随之为 0，属正常现象）。
    double PresentFps() const { return presentFps_.load(); }
    // 「显示滤镜实时帧率」开关（UI 线程调用）：开启后
    //   (a) 覆盖层右上角显示实时帧率 OSD；
    //   (b) 日志每秒写一条帧率明细（帧间隔/长间隔/取帧·探测·绘制各阶段耗时）；
    // 关闭时隐藏 OSD 并恢复每 10 秒一条的简要帧率日志。
    void SetFpsDetail(bool on);
    // UI 线程每约 500ms 调用一次：刷新覆盖层右上角的帧率文字（空串 = 隐藏 OSD）。
    // 窗口创建与绘制都留在 UI 线程，渲染线程只读原子标志/帧率值。
    void SetOsdText(const std::wstring& text);

    std::wstring LastError;
    HWND Hwnd() const { return hwnd_; }

    // 立即把 Capturable 应用到覆盖层（WDA 标志改变后 DWM 才按新设置合成；
    // 仅改标志位不会生效）
    void ApplyOverlayAffinity();

    // 显示/隐藏覆盖层：捕获不可用期间必须隐藏（否则屏幕冻结最后一帧）。
    // 已内置护栏：隐藏期间 DrawAndPresent 不会呈现（flip 交换链向隐藏窗口呈现会永久阻塞）。
    void SetOverlayVisible(bool visible);
    // 覆盖层当前是否可见（捕获不可用时会被隐藏，避免残留冻结画面）
    bool OverlayVisible() const { return overlayVisible_.load(); }
    // 捕获是否正常（false = 正在等待恢复，例如游戏独占全屏/显示模式切换中）
    bool CaptureOk() const { return captureOk_.load(); }

private:
    void RenderLoop();
    bool RecoverCapture();
    // 捕获失效后的自愈：带退避重试若干次（不再一次失败就终止渲染线程）
    bool RecoverCaptureWithRetry(int attempts, int delayMs);
    // 绘制并呈现。返回 true = 这一拍真的呈现了一帧（隐藏/失败时返回 false），
    // 调用方据此统计帧率 —— 否则「没呈现也算一帧」会让帧率虚高（隐藏时实测报到 400+）。
    bool DrawAndPresent();
    // 绑定输入纹理用：工作纹理（捕获帧 CopyResource 进来）的 SRV
    ID3D11ShaderResourceView** InputSrvSlot();
    void RebuildLutIfNeeded();
    // 自检：读回后缓冲中心像素并与输入帧比对，返回 true = 画面有内容（非全黑）
    bool RenderSelfCheck();
    void CreateOverlayWindow();
    void CreateSwapChain();
    // 异格式捕获帧（不是固定的 B8G8R8A8）：本引擎只做 SDR 工作空间，
    // 跟着逐帧换解释方式会让亮度/色彩来回跳（全屏闪烁）→ 一律丢弃并记一条日志
    void OnForeignFrameFormat(ID3D11Texture2D* tex, DXGI_FORMAT format);
    // 场景探测（全 GPU 链路的关键）：把捕获帧上 5 块 16x16 拷进自有的暂存纹理 ——
    // 这一步只在 GPU 命令队列上排队，不产生任何 CPU 等待。
    bool EnsureProbeStaging(DXGI_FORMAT format);
    void BeginSceneProbe(ID3D11Texture2D* tex, DXGI_FORMAT format);
    // 读取**上一轮**发起的探测结果，返回 true 时 maxOut/meanOut 有效。
    // nonBlocking=true 时用 D3D11_MAP_FLAG_DO_NOT_WAIT：GPU 还没写完就直接放弃本轮
    //（返回 false），绝不等待 —— 呈现链路上不留任何同步点。这是整条链路唯一的 GPU→CPU 回读。
    bool ReadProbeStaging(double* maxOut, double* meanOut, bool nonBlocking = false);
    // 采样捕获帧 5 个区域（中心 + 四象限中心，各 16x16）的最大分量值与平均亮度
    //（0..1，失败返回 -1）。同步版本，只给「异格式帧」这类低频诊断路径用。
    double SampleCaptureMaxLuma(ID3D11Texture2D* tex, DXGI_FORMAT format, double* meanOut = nullptr);
    // 每秒一次的探测节拍：先读取上一轮结果，再为当前帧发起本轮拷贝（永不与刚提交的
    // 拷贝同拍 Map → 管线不被阻塞）。返回值供调用方做「整帧全黑」判定与自适应权重。
    struct SceneProbe { bool valid = false; double maxLuma = -1.0; double mean = -1.0; double costMs = 0.0; };
    SceneProbe UpdateSceneLuma(ID3D11Texture2D* tex, DXGI_FORMAT format);
    // 单帧各阶段耗时（毫秒）：定位「卡在哪一段」用。
    // acquireWaitMs = 桌面复制里等「下一个新帧」的时间（源帧率不够时它接近帧间隔）；
    // vsyncWaitMs = Present(1) 自身阻塞的时间：flip 队列有空位时立刻返回（≈0），
    //               队列压满时才阻塞到 vblank —— 所以这个值大反而说明我们在抢着呈现；
    // iterMs = 循环周期（上一拍到这一拍）；用它减去上面各段 = 「没被任何分段解释掉的时间」，
    //          那部分不为 0 就说明渲染线程被抢占/调度出局（而不是取帧或绘制慢）。
    // 其余三项（探测回读/准备/绘制+呈现）才是我们自己的开销。
    struct FrameTiming { double acquireWaitMs = 0, probeMs = 0, prepMs = 0, drawMs = 0, vsyncWaitMs = 0, iterMs = 0; };
    // 每呈现一帧调用一次：统计实时帧率（界面/OSD）与帧间隔、各阶段耗时（日志）
    void NotePresentedFrame(const FrameTiming& t);
    // 清空当前统计窗口的累计量（日志写完一段、覆盖层从隐藏恢复时都调用）
    void ResetStatsWindow();
    // 覆盖层右上角的帧率 OSD（独立置顶分层小窗，GDI 画字 + UpdateLayeredWindow）
    void CreateOsdWindow();
    void UpdateOsd();
    bool CreatePipeline();
    bool CreateDevice();
    // 按显示器坐标（而非索引）查找 DXGI 输出：EnumDisplayMonitors 顺序与
    // EnumOutputs 顺序不一定一致，多显示器时按索引可能捕获到错误的屏幕
    bool FindOutput(ComPtr<IDXGIAdapter>& adapter, ComPtr<IDXGIOutput>& output);
    void CreateCapture();
    // 取得当前后缓冲对应的 RTV。flip 模型交换链的后缓冲每帧在多个缓冲间轮转，
    // 单槽缓存会退化为每帧重建 RTV（4060 等卡顿的元凶之一），这里按缓冲身份缓存。
    ID3D11RenderTargetView* GetBackBufferRtv(ID3D11Texture2D* buffer);
    // 分辨率变化后重建帧纹理/交换链（防止 CopyResource 失败 → 黑屏/花屏）
    void EnsureFrameTexture(const ComPtr<ID3D11Texture2D>& src);
    void EnsureSwapChainSize(UINT w, UINT h);
    bool EnsureShaders();
    void ReleaseAll();

    // 覆盖层窗口
    HWND hwnd_ = nullptr;
    int x_ = 0, y_ = 0, width_ = 0, height_ = 0;
    int outputIndex_ = 0;

    // D3D11
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    // 后缓冲 RTV 缓存（按缓冲身份缓存；BufferCount=2 时只用前 2 槽）
    struct BackBufferRtv { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; };
    BackBufferRtv backBufferRtv_[3];
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11PixelShader> psPassthrough_;
    ComPtr<ID3D11ComputeShader> cs_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> paramsBuffer_;
    ComPtr<ID3D11Texture2D> frameTexture_;
    ComPtr<ID3D11ShaderResourceView> frameSrv_;
    ComPtr<ID3D11Texture2D> backBufferTex_;   // 当前后缓冲（渲染自检读回用）
    ComPtr<ID3D11Texture3D> lutTexture_;
    ComPtr<ID3D11ShaderResourceView> lutSrv_;
    ComPtr<ID3D11UnorderedAccessView> lutUav_;
    ComPtr<ID3D11SamplerState> inputSampler_;   // 线性
    ComPtr<ID3D11SamplerState> lutSampler_;     // 三线性 + clamp
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIAdapter> adapter_;
    ComPtr<IDXGIOutput> output_;

    // 场景平均亮度的采样节拍（值本身只进日志，权重才是被消费的那个）
    DWORD lastSceneSampleTick_ = 0;

    // 捕获/工作格式固定为 B8G8R8A8_UNORM（桌面复制给的 SDR 画面，也是 LUT 的工作空间）。
    // DuplicateOutput1 会请求这个格式；万一仍返回别的格式（例如 HDR 下的 scRGB FP16），
    // 一律丢弃该帧 —— 逐帧换解释方式会让画面在两套色彩映射间来回跳（全屏闪烁）。
    DXGI_FORMAT captureFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool foreignLogged_ = false;    // 丢弃日志节流：每次改钉后只记一条

    // 全黑帧识别与「格式长期不可用」暂停
    struct FormatProbe { DWORD tick = 0; bool black = false; bool known = false; };
    FormatProbe formatProbe_[3];
    ComPtr<ID3D11Texture2D> probeStaging_;
    DXGI_FORMAT probeStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    bool probePending_ = false;   // 已发起拷贝、等待下一次读取（延迟一帧回读）
    DWORD lastPinnedProbeTick_ = 0;
    int pinnedBlackStreak_ = 0;          // 固定格式连续全黑次数
    bool pausedForBlackFrames_ = false;  // 因全黑帧暂停覆盖层
    DWORD lastFrameTick_ = 0;         // 最近收到任意捕获帧
    DWORD lastUsableFrameTick_ = 0;   // 最近收到「固定工作格式」的帧
    bool pausedForUnusableFrames_ = false;
    bool sizesLogged_ = false;        // 像素映射尺寸诊断日志（尺寸变化时各记一条）
    UINT lastCapturedW_ = 0;
    UINT lastCapturedH_ = 0;

    // 自检只做一次（原来每 30 帧整屏读回 → GPU 停顿 → 卡顿）
    bool selfChecked_ = false;
    // 帧节奏统计（定位「一顿一顿」）：帧间隔与单帧处理耗时
    DWORD lastPresentTick_ = 0;
    DWORD statsStartTick_ = 0;   // 当前统计窗口起点（日志节拍）
    bool statsLogged_ = false;   // 是否已写过第一条（首条固定 1 秒时输出）
    LARGE_INTEGER lastPresentQpc_{};
    double sumIntervalMs_ = 0, maxIntervalMs_ = 0, minIntervalMs_ = 0;
    double sumWorkMs_ = 0, maxWorkMs_ = 0;
    int intervalCount_ = 0, longGapCount_ = 0;
    int noFramePolls_ = 0;      // 本窗口内「没有新帧」的空转次数（桌面复制超时）
    int lutRebuilds_ = 0;       // 本窗口内 LUT 重建（参数变化）次数
    int probeNotReady_ = 0;     // 本窗口内探测回读「GPU 还没写完」的次数（正常应极少）
    // 各阶段耗时累计（定位卡在取帧 / 探测回读 / 准备 / 绘制+呈现 的哪一段）
    double sumAcquireMs_ = 0, maxAcquireMs_ = 0;
    double sumProbeMs_ = 0, maxProbeMs_ = 0;
    double sumPrepMs_ = 0, maxPrepMs_ = 0;
    double sumDrawMs_ = 0, maxDrawMs_ = 0;
    double sumVsyncWaitMs_ = 0, maxVsyncWaitMs_ = 0;
    // 循环周期 与 「未计时间」（循环周期 - 各分段之和）：区分「线程被抢占」与「取帧慢」
    double sumIterMs_ = 0, maxIterMs_ = 0;
    double sumUnaccountedMs_ = 0, maxUnaccountedMs_ = 0;
    // 上一拍的起点（QPC）：算循环周期；0 = 还没有上一拍
    LARGE_INTEGER prevIterStartQpc_{};
    // 重复帧 / 源帧堆积统计（GPU 满载时「旧帧堆积」的两个可观测量，都在帧率明细日志里）：
    //   dupFrames_        —— 桌面位图没变（LastPresentTime==0）而跳过重绘的帧数；
    //   pointerOnlyFrames_—— 其中带指针更新的帧数（位图没变，动的是指针）；
    //   accumulatedFrames_—— 操作系统累积/合并掉的源帧总数（Sum(AccumulatedFrames-1)）；
    //   coalescedRects_   —— 脏区被合并过的次数（RectsCoalesced，用于判断元数据是否粗略）。
    // 前两个大 = 不是我们在丢帧，是桌面本身没有新内容；最后一个大 = 滤镜确实没跟上桌面更新频率。
    int dupFrames_ = 0, pointerOnlyFrames_ = 0, accumulatedFrames_ = 0, coalescedRects_ = 0;
    // 是否见过非零的 LastPresentTime（判断驱动到底有没有上报这个时间戳）：
    // 没见过就绝不启用「重复帧不重绘」—— 万一驱动永远给 0，那会把覆盖层冻住。
    bool seenDesktopPresentTime_ = false;
    double lastVsyncWaitMs_ = 0;   // RenderLoop 取帧前等「帧延迟等待对象」的耗时（已并入上面统计）
    // 实时帧率（界面右上角）：1 秒窗口内呈现帧数；每 0.5 秒刷新一次显示值
    std::atomic<double> presentFps_{ 0.0 };
    std::atomic<bool> fpsDetail_{ false };   // 详细帧率日志（每秒一条）
    LARGE_INTEGER fpsWindowStart_{};
    int fpsWindowCount_ = 0;
    // 覆盖层右上角的帧率 OSD（窗口属 UI 线程；渲染线程只读 showFpsOn_/presentFps_）
    HWND osd_ = nullptr;
    std::atomic<bool> showFpsOn_{ false };
    std::mutex osdMutex_;
    std::wstring osdText_;
    std::wstring osdDrawnText_;     // 上次真正画上去的文字（相同就跳过重绘）
    int osdDrawnX_ = 0, osdDrawnY_ = 0;
    bool osdShown_ = false;         // OSD 当前是否已显示（只在状态切换时置顶/显示）
    // 暗场景优化的场景自适应权重（0.5..1；-1 = 还没测到场景）。由渲染线程写、渲染线程读，
    // 只在 DrawAndPresent 上传常量缓冲时合进槽 42 —— 界面下发的参数里不含它。
    std::atomic<double> sceneWeight_{ 1.0 };
    // 上一次生效的权重：只有它变化才需要重建 LUT（原始亮度波动不该触发）
    double lastSceneWeight_ = -1.0;
    DWORD lastSceneRebuildTick_ = 0;
    // 自适应权重的下限：用户滑块永远至少保留这个比例（详见 lut_engine.cpp 场景探测处）
    static constexpr double kDarkSceneMinWeight = 0.5;
    // LUT 重建的最小间隔（毫秒）：拖动滑块时参数每帧都变，重建要跑 64³ 计算着色器，
    // 不限速会打到 28~77 次/秒（GPU 全被重建吃掉）。30ms ≈ 33 次/秒，肉眼无差别。
    static constexpr DWORD kLutRebuildMinIntervalMs = 30;
    DWORD lastLutRebuildTick_ = 0;
    // InputSrvSlot 返回的临时槽（只用于把 SRV 指针交给 PSSetShaderResources）
    ID3D11ShaderResourceView* srvBindScratch_ = nullptr;
    // 引擎启动时刻（RenderLoop 开头记一次）：用于两个启动期保护
    DWORD startTick_ = 0;
    // 启动保护期：捕获刚建立时 DXGI 还没分配好 flip 池，头几帧常常整帧全黑 / 格式未定。
    // 这段时间内不做全黑帧判定、不做异格式诊断 —— 免得反复 Show/Hide 覆盖层。
    static constexpr DWORD kStartupGraceMs = 1500;
    // 帧率统计预热：第一次 AcquireNextFrame 可能阻塞几百毫秒，那个样本会污染整秒统计
    static constexpr DWORD kStatsWarmupMs = 1000;
    DWORD statsWarmupUntil_ = 0;
    // 中间隐藏过（捕获恢复/全黑暂停）→ 恢复后第一帧只重建基准，不把隐藏时长算成帧间隔
    bool presentsSuppressed_ = false;
    // "覆盖层不可见 → 暂停呈现"只记一条日志（避免刷屏）
    bool presentSkipLogged_ = false;
    // Present 失败只记一条日志（避免每帧刷屏）
    bool presentFailLogged_ = false;

    // 线程与状态
    std::thread renderThread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> disposed_{ false };
    std::atomic<bool> overlayVisible_{ true };
    std::atomic<bool> captureOk_{ true };
    std::mutex paramsMutex_;
    float params_[kParamsFloatCount] = {};
    float paramsCopy_[kParamsFloatCount] = {};
    std::atomic<bool> paramsDirty_{ false };
    std::atomic<bool> neutral_{ true };

    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace hsf
