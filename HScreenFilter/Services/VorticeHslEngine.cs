using System;
using System.Runtime.InteropServices;
using System.Threading;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using HScreenFilter.Models;
using ID3D11Buffer = Vortice.Direct3D11.ID3D11Buffer;
using ID3D11DeviceContext = Vortice.Direct3D11.ID3D11DeviceContext;
using ID3D11RenderTargetView = Vortice.Direct3D11.ID3D11RenderTargetView;
using ID3D11ShaderResourceView = Vortice.Direct3D11.ID3D11ShaderResourceView;
using ID3D11Texture2D = Vortice.Direct3D11.ID3D11Texture2D;
using ID3D11VertexShader = Vortice.Direct3D11.ID3D11VertexShader;
using ID3D11PixelShader = Vortice.Direct3D11.ID3D11PixelShader;
using ID3D11InputLayout = Vortice.Direct3D11.ID3D11InputLayout;

namespace HScreenFilter.Services;

/// <summary>
/// 全 Vortice D3D11 渲染管线：
///   1) 创建原始 Win32 覆盖层窗口（置顶、无边框、点击穿透、WDA_EXCLUDEFROMCAPTURE）；
///   2) 在该窗口 HWND 上创建 D3D11 交换链；
///   3) DXGI Desktop Duplication 捕获桌面 → GPU 内 CopyResource 到渲染纹理；
///   4) 用逐像素 HSL 着色器绘制全屏三角形 → Present。
///
/// 全程 GPU、无 CPU 往返，不触发 Windows 11 屏幕捕获隐私边框。
/// </summary>
internal sealed class VorticeHslEngine : IDisposable
{
    private const uint DXGI_ERROR_WAIT_TIMEOUT = 0x887A0027u;
    private const uint DXGI_ERROR_ACCESS_LOST = 0x887A0026u;
    private const string WindowClassName = "HScreenFilterVorticeOverlay";

    // 静态持有窗口过程，防止被 GC
    private static OverlayNative.WNDPROC? _staticWndProc;

    private IntPtr _hwnd;
    private int _x;         // 覆盖层窗口在虚拟桌面的 X（左上角）
    private int _y;         // 覆盖层窗口在虚拟桌面的 Y（左上角）
    private int _width;
    private int _height;
    private int _outputIndex; // 该显示器在 DXGI 适配器中的输出索引（用于捕获）

    // D3D11 对象
    private ID3D11Device _device = null!;
    private ID3D11DeviceContext _context = null!;
    private IDXGISwapChain1 _swapChain = null!;
    private ID3D11RenderTargetView _rtv = null!;
    private ID3D11VertexShader _vs = null!;
    private ID3D11PixelShader _ps = null!;
    private ID3D11InputLayout _inputLayout = null!;
    private ID3D11RasterizerState _rasterizerState = null!; // CullMode.None：全屏三角形不被剔除
    private ID3D11Buffer _vertexBuffer = null!;
    private ID3D11Buffer _constantBuffer = null!;
    private ID3D11Texture2D _frameTexture = null!;
    private ID3D11ShaderResourceView _srv = null!;
    private IDXGIOutputDuplication _duplication = null!;

    private Thread? _renderThread;
    private volatile bool _running;
    private volatile bool _disposed;
    private volatile bool _capturable; // 覆盖层可被 OBS 等屏幕捕获（WDA_MONITOR，DXGI 自捕获排除=防自反馈）

    // 着色器参数（与 HLSL cbuffer 顺序一致，40 个 float = 160 字节，16 字节对齐：
    //   0..32 滤镜参数，33..36 光标排除参数，37..39 padding）
    private readonly object _paramsLock = new();
    private readonly float[] _params = new float[40];
    private readonly float[] _paramsCopy = new float[40];

    // 帧率节流：Present(0) 无 vsync；桌面无变化时 AcquireNextFrame(100ms) 自然兜底，
    // 动态画面不设上限（MinFrameIntervalMs=0 = 不节流，用户要求解锁帧率）
    private readonly System.Diagnostics.Stopwatch _frameTimer = new();
    private const int MinFrameIntervalMs = 0;

    // 捕获重建锁：保护 _duplication 跨线程重建
    private readonly object _captureLock = new();

    // 光标排除：排除区半宽（像素），>0 时启用；略大于默认 32x32 光标避免边缘残留
    private const float CursorHalfSize = 24f;

    public string? LastError { get; private set; }

    /// <summary>渲染线程是否仍在运行（供上层检测意外退出并自动重建）。</summary>
    public bool IsRendering => _running && _renderThread != null && _renderThread.IsAlive;

    /// <summary>覆盖层是否可被屏幕捕获（OBS 等）。可捕获 → WDA_MONITOR（仅 DXGI Desktop Duplication 排除自身，防自反馈）。</summary>
    public bool Capturable
    {
        get => _capturable;
        set
        {
            _capturable = value;
            ApplyOverlayAffinity();
        }
    }

    /// <summary>启动：创建覆盖层窗口、交换链、着色器、捕获。失败返回 false 并记录 <see cref="LastError"/>。</summary>
    public bool Start(int x, int y, int width, int height, int outputIndex)
    {
        _x = x;
        _y = y;
        _width = width;
        _height = height;
        _outputIndex = outputIndex;
        try
        {
            _device = D3D11.D3D11CreateDevice(DriverType.Hardware, DeviceCreationFlags.BgraSupport, null!);
            _context = _device.ImmediateContext;

            CreateOverlayWindow();
            CreateSwapChain();
            CreatePipeline();
            CreateCapture();
            SetNeutralParams();

            _running = true;
            _renderThread = new Thread(RenderLoop) { IsBackground = true, Name = "VorticeHslRender" };
            _renderThread.Start();
            AppLog.Write("Engine", $"Start OK: {width}x{height}");
            return true;
        }
        catch (Exception ex)
        {
            LastError = ex.Message;
            AppLog.Write("Engine", $"Start FAILED: {ex}");
            Dispose();
            return false;
        }
    }

    /// <summary>调色强度系数：0.5 = 所有 HSL 调色强度减半（只减半“相对中性值”的偏差，
    /// 不改动用户保存的参数，UI/配置文件里的值保持原样）。</summary>
    private const double AdjustStrength = 0.75;

    /// <summary>用滤镜参数更新着色器常量（线程安全：只写入参数缓冲，渲染线程每帧读取）。</summary>
    public void Apply(FilterSettings s)
    {
        var p = _params;
        lock (_paramsLock)
        {
            // 与 HLSL cbuffer 字段顺序保持一致
            p[0] = (float)(s.Hue * AdjustStrength);                                    // MasterHue
            p[1] = (float)(1.0 + (s.HslSaturation / 100.0 - 1.0) * AdjustStrength);    // MasterSat
            p[2] = (float)(s.Lightness / 100.0 * AdjustStrength);                      // MasterLight
            p[3] = (float)(1.0 + (s.Saturation / 100.0 - 1.0) * AdjustStrength);       // GlobalSat
            p[4] = (float)(s.Temperature / 100.0);                // Temperature
            p[5] = (float)(s.Contrast / 100.0);                   // Contrast
            p[6] = (float)(s.Brightness / 100.0 * 0.5);           // Brightness
            p[7] = (float)(s.Highlights / 100.0);                 // Highlights
            p[8] = (float)(s.Shadows / 100.0);                    // Shadows

            int idx = 9;
            var channels = s.HslChannels; // 防御：反序列化旧数据可能为 null
            foreach (var name in HslChannelNames.ColorNames)
            {
                var ch = channels?.Find(c => c != null && c.Name == name);
                p[idx++] = ch == null ? 0f : (float)(ch.Hue * AdjustStrength);                          // Hue
                p[idx++] = ch == null ? 1f : (float)(1.0 + (ch.Saturation / 100.0 - 1.0) * AdjustStrength); // Sat
                p[idx++] = ch == null ? 0f : (float)(ch.Lightness / 100.0 * AdjustStrength);            // Light
            }
            // 滤镜参数填到 0..32；33..36 光标参数由渲染线程每帧更新，37..39 padding（数组初始为 0）
            for (; idx < 33; idx++) p[idx] = 0f;
        }
    }

    private void SetNeutralParams()
    {
        var s = new FilterSettings();
        Apply(s);
    }

    // ---------------- 覆盖层窗口 ----------------

    private void CreateOverlayWindow()
    {
        IntPtr hInstance = OverlayNative.GetModuleHandle(null);
        _staticWndProc ??= OverlayWndProc;

        var wc = new OverlayNative.WNDCLASS
        {
            style = 0,
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_staticWndProc),
            cbClsExtra = 0,
            cbWndExtra = 0,
            hInstance = hInstance,
            hIcon = IntPtr.Zero,
            hCursor = IntPtr.Zero,
            hbrBackground = IntPtr.Zero,
            lpszMenuName = null,
            lpszClassName = WindowClassName,
        };
        if (OverlayNative.RegisterClassW(ref wc) == 0)
        {
            // 反复开关 DXGI 时，同名窗口类已由上次启动注册过 → RegisterClassW 返回 0 且
            // GetLastError = ERROR_CLASS_ALREADY_EXISTS(1410)，此时窗口类仍可复用，不算失败。
            int err = Marshal.GetLastWin32Error();
            if (err != 1410 /* ERROR_CLASS_ALREADY_EXISTS */)
                throw new InvalidOperationException($"RegisterClassW 失败 (0x{err:X8})");
        }

        // 点击穿透必须 WS_EX_LAYERED | WS_EX_TRANSPARENT（否则跨进程鼠标点击被覆盖层挡住）
        // 注意：不能加 WS_EX_TOOLWINDOW —— OBS 窗口捕获（window-helpers.c）会过滤掉工具窗口，
        // 导致 OBS「窗口捕获」检测不到滤镜层。代价是覆盖层会出现在 Alt+Tab（标题「HScreenFilter 滤镜层」）。
        uint exStyle = OverlayNative.WS_EX_TRANSPARENT | OverlayNative.WS_EX_LAYERED
                     | OverlayNative.WS_EX_NOACTIVATE | OverlayNative.WS_EX_TOPMOST;
        uint style = OverlayNative.WS_POPUP | OverlayNative.WS_VISIBLE | OverlayNative.WS_CLIPSIBLINGS;

        // 窗口标题供 OBS「窗口捕获」识别（覆盖层全屏 = 整屏滤镜效果）
        _hwnd = OverlayNative.CreateWindowExW(exStyle, WindowClassName, "HScreenFilter 滤镜层",
            style, _x, _y, _width, _height, IntPtr.Zero, IntPtr.Zero, hInstance, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero)
            throw new InvalidOperationException("CreateWindowExW 失败");

        // 捕获亲和性：可捕获 → WDA_MONITOR（DXGI 自捕获排除 = 防自反馈，但 OBS 仍可见）；
        // 否则 → WDA_EXCLUDEFROMCAPTURE（从一切屏幕捕获排除）
        ApplyOverlayAffinity();

        // 置顶必须改 WS_EX_TOPMOST 样式位（对分层窗口仅 SetWindowPos 无效）
        OverlayNative.SetTopmost(_hwnd, true);
    }

    /// <summary>应用覆盖层的屏幕捕获亲和性：可捕获 → WDA_MONITOR（仅 DXGI Desktop Duplication 排除，OBS/WGC/BitBlt 可见）；
    /// 否则 → WDA_EXCLUDEFROMCAPTURE（从一切捕获排除，防自反馈）。</summary>
    private void ApplyOverlayAffinity()
    {
        if (_hwnd == IntPtr.Zero) return;
        try
        {
            OverlayNative.SetWindowDisplayAffinity(_hwnd,
                _capturable ? OverlayNative.WDA_MONITOR : OverlayNative.WDA_EXCLUDEFROMCAPTURE);
            AppLog.Write("Engine", $"覆盖层 WDA={( _capturable ? "MONITOR(可捕获/WGC可见)" : "EXCLUDEFROMCAPTURE(全部排除)")}");
        }
        catch
        {
            // 老系统忽略
        }
    }

    private static IntPtr OverlayWndProc(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam)
    {
        // 点击穿透：WM_NCHITTEST 一律返回 HTTRANSPARENT
        if (uMsg == OverlayNative.WM_NCHITTEST)
            return (IntPtr)OverlayNative.HTTRANSPARENT;
        // 注意：不要在这里 PostQuitMessage —— 覆盖层窗口创建在 UI 线程，
        // 销毁时若投递 WM_QUIT 会终止整个 WinUI 应用的消息循环（关闭 DXGI 时闪退）。
        return OverlayNative.DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    // ---------------- 交换链 ----------------

    private void CreateSwapChain()
    {
        using var dxgiDevice = _device.QueryInterface<IDXGIDevice>();
        var adapter = dxgiDevice.GetAdapter();
        using var factory = adapter.GetParent<IDXGIFactory2>();

        var desc = new SwapChainDescription1
        {
            Width = (uint)_width,
            Height = (uint)_height,
            Format = Format.B8G8R8A8_UNorm,
            Stereo = false,
            SampleDescription = new SampleDescription(1, 0),
            BufferUsage = Usage.RenderTargetOutput,
            BufferCount = 2,
            Scaling = Scaling.Stretch,
            SwapEffect = SwapEffect.FlipSequential,
            // 不透明（忽略 alpha）：保证 HSL 逐像素调色正确显示。
            // 注：不要改 Premultiplied 做逐像素透明——会破坏 HSL 分色系调色的显示。
            AlphaMode = AlphaMode.Ignore,
            Flags = 0,
        };

        _swapChain = factory.CreateSwapChainForHwnd(_device, _hwnd, desc);

        // 禁止 Alt+Enter 全屏切换
        factory.MakeWindowAssociation(_hwnd, WindowAssociationFlags.IgnoreAltEnter);

        var backBuffer = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        _rtv = _device.CreateRenderTargetView(backBuffer);
    }

    // ---------------- 渲染管线 ----------------

    private void CreatePipeline()
    {
        // 顶点着色器：全屏三角形（覆盖整个视口）
        string vsSource = """
            struct VSInput { float4 pos : POSITION; float2 uv : TEXCOORD0; };
            struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
            VSOutput main(VSInput i) { VSOutput o; o.pos = i.pos; o.uv = i.uv; return o; }
            """;
        var vsBlob = HlslCompiler.CompileVertexShader(vsSource);
        _vs = _device.CreateVertexShader(vsBlob);

        // 像素着色器：复用逐像素 HSL 逻辑（去掉 Win2D 的 SCENE_POSITION 语义，改用 uv）
        var psBlob = HlslCompiler.CompilePixelShader(PixelShaderSource);
        _ps = _device.CreatePixelShader(psBlob);

        // 输入布局（与顶点结构一致）
        var elements = new[]
        {
            new InputElementDescription("POSITION", 0, Format.R32G32B32A32_Float, 0, 0),
            new InputElementDescription("TEXCOORD", 0, Format.R32G32_Float, 16, 0),
        };
        _inputLayout = _device.CreateInputLayout(elements, vsBlob);

        // 光栅化器状态：禁用背面剔除（视口变换翻转 Y 后三角形呈逆时针，默认 Back 剔除会整屏不绘制 = 黑屏）
        _rasterizerState = _device.CreateRasterizerState(
            new Vortice.Direct3D11.RasterizerDescription(CullMode.None, FillMode.Solid));

        // 顶点缓冲：全屏三角形（3 顶点）
        // UV 的 V 轴：DXGI 桌面纹理 v=0 是顶部、v=1 是底部。
        // 屏幕底部顶点(y=-1)用 v=1，屏幕顶部顶点(y=3)用 v=-1(clamp 到 0) → 画面方向正确（此前 v 反向导致上下颠倒）。
        float[] vertices =
        {
            -1.0f, -1.0f, 0.5f, 1.0f,   0.0f, 1.0f,
             3.0f, -1.0f, 0.5f, 1.0f,   2.0f, 1.0f,
            -1.0f,  3.0f, 0.5f, 1.0f,   0.0f, -1.0f,
        };
        _vertexBuffer = _device.CreateBuffer(
            new BufferDescription((uint)(vertices.Length * sizeof(float)), BindFlags.VertexBuffer));
        _context.UpdateSubresource(vertices, _vertexBuffer);

        // 常量缓冲：40 个 float = 160 字节（0..32 滤镜参数 + 33..36 光标排除 + 37..39 padding）
        _constantBuffer = _device.CreateBuffer(
            new BufferDescription(40 * sizeof(float), BindFlags.ConstantBuffer));

        // 帧纹理（GPU 内拷贝目标）+ SRV
        var texDesc = new Texture2DDescription
        {
            Width = (uint)_width,
            Height = (uint)_height,
            MipLevels = 1,
            ArraySize = 1,
            Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Default,
            BindFlags = BindFlags.ShaderResource,
            CPUAccessFlags = CpuAccessFlags.None,
            MiscFlags = ResourceOptionFlags.None,
        };
        _frameTexture = _device.CreateTexture2D(texDesc);
        _srv = _device.CreateShaderResourceView(_frameTexture);
    }

    private void CreateCapture()
    {
        using var dxgiDevice = _device.QueryInterface<IDXGIDevice>();
        var adapter = dxgiDevice.GetAdapter();
        // 捕获该显示器对应的输出（多显示器时每块显示器一个覆盖层，各自捕获自己的输出）
        IDXGIOutput? output = null;
        var enResult = adapter.EnumOutputs((uint)_outputIndex, out output);
        if (enResult.Failure || output == null)
            throw new InvalidOperationException($"EnumOutputs({_outputIndex}) 失败 (0x{enResult.Code:X8})");
        using var output1 = output.QueryInterface<IDXGIOutput1>();
        _duplication = output1.DuplicateOutput(_device);
    }

    // ---------------- 渲染循环 ----------------

    private void RenderLoop()
    {
        _frameTimer.Restart();
        int frameCount = 0;
        while (_running)
        {
            try
            {
                var result = _duplication.AcquireNextFrame(100, out _, out var resource);
                if (result.Success)
                {
                    try
                    {
                        using var tex = resource.QueryInterface<ID3D11Texture2D>();
                        // GPU 内拷贝：捕获帧 → 渲染纹理（无 CPU 往返）
                        _context.CopyResource(_frameTexture, tex);
                    }
                    finally
                    {
                        resource.Dispose();
                    }
                    _duplication.ReleaseFrame();
                    DrawAndPresent();
                    // 启动后自检一次渲染画面（读回 back buffer 中心像素，排查黑屏）
                    if (++frameCount == 30) RenderSelfCheck();
                    Throttle();
                }
                else if (result.Code == unchecked((int)DXGI_ERROR_WAIT_TIMEOUT))
                {
                    continue; // 桌面无变化
                }
                else if (result.Code == unchecked((int)DXGI_ERROR_ACCESS_LOST))
                {
                    // 桌面模式/分辨率变化：重建捕获后继续，避免画面冻结
                    if (!RecoverCapture()) break;
                }
                else
                {
                    LastError = "DXGI 捕获错误: 0x" + result.Code.ToString("X8");
                    AppLog.Write("Engine", $"RenderLoop exit: {LastError}");
                    break;
                }
            }
            catch (Exception ex)
            {
                LastError = "DXGI 渲染异常: " + ex.Message;
                AppLog.Write("Engine", $"RenderLoop exception: {ex}");
                break;
            }
        }
        _running = false;
        AppLog.Write("Engine", "RenderLoop ended");
    }

    /// <summary>重建桌面捕获（桌面分辨率/模式变化后调用）。成功返回 true。</summary>
    private bool RecoverCapture()
    {
        try
        {
            lock (_captureLock)
            {
                try { _duplication?.Dispose(); } catch { }
                _duplication = null!;
                CreateCapture();
            }
            LastError = null;
            AppLog.Write("Engine", "RecoverCapture OK");
            return true;
        }
        catch (Exception ex)
        {
            LastError = "重建 DXGI 捕获失败: " + ex.Message;
            AppLog.Write("Engine", $"RecoverCapture FAILED: {ex}");
            return false;
        }
    }

    /// <summary>帧率节流：限制最大 FPS，避免无 vsync 时 GPU 满载导致系统卡死。</summary>
    private void Throttle()
    {
        long wait = MinFrameIntervalMs - _frameTimer.ElapsedMilliseconds;
        if (wait > 0) Thread.Sleep((int)wait);
        _frameTimer.Restart();
    }

    private void DrawAndPresent()
    {
        // 锁内只做内存操作（更新光标 + 拷贝参数）；GPU Map 放到锁外，
        // 避免渲染线程持锁等待 GPU 时把 UI 线程的 Apply 锁死。
        float[] local;
        lock (_paramsLock)
        {
            UpdateCursorParamsLocked();
            Array.Copy(_params, _paramsCopy, _params.Length);
            local = _paramsCopy;
        }

        // 用 UpdateSubresource 上传常量缓冲（避免 Map + Marshal.Copy 裸指针在设备异常时写坏内存导致 AccessViolation 闪退）
        _context.UpdateSubresource(local, _constantBuffer);

        // 绑定管线（必须设置视口，否则全屏三角形不会被光栅化 → 画面为清除色黑色 = 黑屏）
        _context.ClearRenderTargetView(_rtv, new Vortice.Mathematics.Color4(0, 0, 0, 0));
        _context.OMSetRenderTargets(_rtv);
        _context.RSSetViewport(0, 0, _width, _height);
        _context.RSSetState(_rasterizerState);
        _context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        _context.IASetInputLayout(_inputLayout);
        _context.IASetVertexBuffer(0, _vertexBuffer, 24, 0); // 顶点大小 = 6 float * 4 字节
        _context.VSSetShader(_vs);
        _context.PSSetShader(_ps);
        _context.PSSetConstantBuffer(0, _constantBuffer);
        _context.PSSetShaderResource(0, _srv);

        // 绘制全屏三角形并呈现（Present(0) 无垂直同步，解锁帧率）
        _context.Draw(3, 0);
        _swapChain.Present(0, PresentFlags.None);
    }

    /// <summary>启动后自检一次渲染画面（读回 back buffer 中心像素），黑屏时日志会记录「画面全黑」。</summary>
    private void RenderSelfCheck()
    {
        try
        {
            DrawAndPresent();
            var drawn = ReadBackCenter();
            bool visible = (drawn.R | drawn.G | drawn.B) > 8;
            AppLog.Write("Engine",
                $"渲染自检: 中心像素 R={drawn.R} G={drawn.G} B={drawn.B} A={drawn.A} -> {(visible ? "画面正常" : "画面全黑!")}");
        }
        catch (Exception ex)
        {
            AppLog.Write("Engine", "渲染自检异常: " + ex.Message);
        }
    }

    /// <summary>从 back buffer 0 拷贝到 staging 并读回中心像素（B8G8R8A8）。</summary>
    private (byte R, byte G, byte B, byte A) ReadBackCenter()
        => ReadBackPixel(_width / 2, _height / 2);

    /// <summary>从 back buffer 0 拷贝到 staging 并读回指定像素（B8G8R8A8）。</summary>
    private (byte R, byte G, byte B, byte A) ReadBackPixel(int px, int py)
    {
        using var back = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        var desc = back.Description;
        using var staging = _device.CreateTexture2D(new Texture2DDescription
        {
            Width = desc.Width,
            Height = desc.Height,
            MipLevels = 1,
            ArraySize = 1,
            Format = desc.Format,
            SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Staging,
            BindFlags = BindFlags.None,
            CPUAccessFlags = CpuAccessFlags.Read,
            MiscFlags = ResourceOptionFlags.None,
        });
        _context.CopyResource(staging, back);
        _context.Flush();
        var mapped = _context.Map(staging, 0, MapMode.Read, Vortice.Direct3D11.MapFlags.None);
        int off = py * (int)mapped.RowPitch + px * 4; // B8G8R8A8：B,G,R,A
        byte b = Marshal.ReadByte(mapped.DataPointer, off);
        byte g = Marshal.ReadByte(mapped.DataPointer, off + 1);
        byte r = Marshal.ReadByte(mapped.DataPointer, off + 2);
        byte a = Marshal.ReadByte(mapped.DataPointer, off + 3);
        _context.Unmap(staging, 0);
        return (r, g, b, a);
    }

    /// <summary>更新光标排除参数（必须在 _paramsLock 内调用）。
    /// HLSL cbuffer 打包：滤镜参数 0..32 后紧跟 CursorX=33、CursorY=34、CursorHalfW=35、CursorHalfH=36
    /// （32..35 落在同一 float4 寄存器，36 在下一寄存器）。</summary>
    private void UpdateCursorParamsLocked()
    {
        var pt = new OverlayNative.POINT();
        if (OverlayNative.GetCursorPos(ref pt))
        {
            // 覆盖窗口铺满所在显示器且位于 (x,y)，光标排除用相对该显示器的像素坐标
            _params[33] = pt.X - _x;
            _params[34] = pt.Y - _y;
            _params[35] = CursorHalfSize;
            _params[36] = CursorHalfSize;
        }
        else
        {
            _params[35] = -1f; // 负半宽 = 禁用光标排除
            _params[36] = -1f;
        }
    }

    // ---------------- 像素着色器源码 ----------------

    /// <summary>逐像素 HSL 分色系着色器（与 Win2D 版逻辑一致，签名改为标准 D3D11）。</summary>
    private static string PixelShaderSource => """
        Texture2D InputTexture : register(t0);
        SamplerState InputSampler : register(s0);

        cbuffer Params : register(b0)
        {
            float MasterHue;
            float MasterSat;
            float MasterLight;
            float GlobalSat;
            float Temperature;
            float Contrast;
            float Brightness;
            float Highlights;
            float Shadows;
            float HueR, SatR, LightR;
            float HueO, SatO, LightO;
            float HueY, SatY, LightY;
            float HueG, SatG, LightG;
            float HueC, SatC, LightC;
            float HueB, SatB, LightB;
            float HueP, SatP, LightP;
            float HueM, SatM, LightM;
            float CursorX, CursorY, CursorHalfW, CursorHalfH;
        };

        float wrapHue(float h)
        {
            h = fmod(h, 360.0);
            if (h < 0.0) h += 360.0;
            return h;
        }

        void rgbToHsl(float3 c, out float h, out float s, out float l)
        {
            float maxc = max(c.r, max(c.g, c.b));
            float minc = min(c.r, min(c.g, c.b));
            l = (maxc + minc) * 0.5;
            float d = maxc - minc;
            if (d < 1e-4)
            {
                h = 0.0;
                s = 0.0;
                return;
            }
            s = d / max(1.0 - abs(2.0 * l - 1.0), 1e-4);
            if (maxc == c.r)
                h = (c.g - c.b) / d;
            else if (maxc == c.g)
                h = (c.b - c.r) / d + 2.0;
            else
                h = (c.r - c.g) / d + 4.0;
            h = wrapHue(h * 60.0);
        }

        float3 hslToRgb(float h, float s, float l)
        {
            float c = (1.0 - abs(2.0 * l - 1.0)) * s;
            float hp = wrapHue(h) / 60.0;
            float x = c * (1.0 - abs(fmod(hp, 2.0) - 1.0));
            float3 rgb;
            if (hp < 1.0) rgb = float3(c, x, 0.0);
            else if (hp < 2.0) rgb = float3(x, c, 0.0);
            else if (hp < 3.0) rgb = float3(0.0, c, x);
            else if (hp < 4.0) rgb = float3(0.0, x, c);
            else if (hp < 5.0) rgb = float3(x, 0.0, c);
            else rgb = float3(c, 0.0, x);
            float m = l - c * 0.5;
            return rgb + m;
        }

        // —— OKLab/OKLCH（Björn Ottosson 2020 官方参考实现，含健壮性处理）——
        // 算法与官方一致：sRGB 线性化 → LMS → 立方根 → OKLab → 极坐标(OKLCH)。
        // 健壮性（上一版全白的根因修复）：
        //  1) 消色像素（C≈0）不调 atan2(0,0)——HLSL 中 atan2(0,0) 未定义，部分 GPU 返回 NaN，
        //     而灰色像素在 OKLab 恰为 a=0,b=0（白底/UI/阴影到处都是）→ NaN 传播 → 全白。
        //  2) pow 底数用 max(x, 1e-8)，避免 pow(0, 小数) 在部分 GPU 产生 NaN。
        float3 SrgbToLinear(float3 c)
        {
            float3 lo = c / 12.92;
            float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
            return lerp(lo, hi, step(0.04045, c));
        }

        float3 LinearToSrgb(float3 c)
        {
            float3 lo = c * 12.92;
            float3 hi = 1.055 * pow(max(c, 1e-8), 1.0 / 2.4) - 0.055;
            return lerp(lo, hi, step(0.0031308, c));
        }

        void RgbToOklch(float3 c, out float L, out float C, out float H)
        {
            float3 lin = SrgbToLinear(c);
            float l = 0.4122214708 * lin.r + 0.5363325363 * lin.g + 0.0514459929 * lin.b;
            float m = 0.2119034982 * lin.r + 0.6806995451 * lin.g + 0.1073969566 * lin.b;
            float s = 0.0883024619 * lin.r + 0.2817188376 * lin.g + 0.6299787005 * lin.b;
            l = pow(max(l, 1e-8), 1.0 / 3.0);
            m = pow(max(m, 1e-8), 1.0 / 3.0);
            s = pow(max(s, 1e-8), 1.0 / 3.0);
            float La = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
            float a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
            float b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
            L = La;
            C = sqrt(a * a + b * b);
            // 消色像素避免 atan2(0,0)（HLSL 未定义）
            H = C < 1e-5 ? 0.0 : wrapHue(atan2(b, a) * 57.2957795);
        }

        float3 OklchToRgb(float L, float C, float H)
        {
            float hr = H * 0.0174532925; // π/180
            float a = C * cos(hr);
            float b = C * sin(hr);
            float l_ = L + 0.3963377774 * a + 0.2158037573 * b;
            float m_ = L - 0.1055613458 * a - 0.0638541728 * b;
            float s_ = L - 0.0894841775 * a - 1.2914855480 * b;
            l_ = l_ * l_ * l_;
            m_ = m_ * m_ * m_;
            s_ = s_ * s_ * s_;
            float r =  4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_;
            float g = -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_;
            float b2 = -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_;
            return LinearToSrgb(float3(r, g, b2));
        }

        // 色系软掩码：以参考色相为中心，半宽 60°，平滑衰减到 0（试验阶段所有掩码统一 60°）。
        float hueMask(float h, float refHue)
        {
            float a = abs(h - refHue);
            a = min(a, 360.0 - a);
            float t = a / 60.0;
            float w = saturate(1.0 - t);
            return w * w * (3.0 - 2.0 * w);
        }

        float isActive(float h, float sat, float light)
        {
            return (abs(h) > 0.01 || abs(sat - 1.0) > 0.001 || abs(light) > 0.001) ? 1.0 : 0.0;
        }

        // 明度掩码：试验阶段与色相/饱和度同为 60°（可随时改回 30° 收紧亮暗过渡）。
        float hueMaskNarrow(float h, float refHue)
        {
            float a = abs(h - refHue);
            a = min(a, 360.0 - a);
            float t = a / 60.0;
            float w = saturate(1.0 - t);
            return w * w * (3.0 - 2.0 * w);
        }

        // 该色系是否有明度调整（亮度掩码只看明度，避免被色相/饱和度调整连带放大）
        float isActiveLight(float light)
        {
            return abs(light) > 0.001 ? 1.0 : 0.0;
        }

        // 抖动噪声：按屏幕像素坐标的确定性伪随机（0..1），同一像素每帧一致，不会闪烁/蠕变。
        float DitherNoise(float2 p)
        {
            return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
        }

        float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
        {
            float4 color = InputTexture.Sample(InputSampler, uv);

            // 光标排除已移除：光标与画面一起被滤镜正常处理（不再做剔除，避免方框/光晕影响体验）
            float3 c = color.rgb;

            float h, s, l;
            rgbToHsl(c, h, s, l);

            // 主色系（全部）色相：先旋转再用于掩码选择（所见即所选）
            h = wrapHue(h + MasterHue);

            float wR = hueMask(h, 0.0)   * isActive(HueR, SatR, LightR);
            float wO = hueMask(h, 30.0)  * isActive(HueO, SatO, LightO);
            float wY = hueMask(h, 60.0)  * isActive(HueY, SatY, LightY);
            float wG = hueMask(h, 120.0) * isActive(HueG, SatG, LightG);
            float wC = hueMask(h, 180.0) * isActive(HueC, SatC, LightC);
            float wB = hueMask(h, 240.0) * isActive(HueB, SatB, LightB);
            float wP = hueMask(h, 270.0) * isActive(HueP, SatP, LightP);
            float wM = hueMask(h, 300.0) * isActive(HueM, SatM, LightM);

            // 明度用更窄的掩码（30°）：亮暗调整集中在色系中心，过渡区不放大原画面亮度差异
            float lR = hueMaskNarrow(h, 0.0)   * isActiveLight(LightR);
            float lO = hueMaskNarrow(h, 30.0)  * isActiveLight(LightO);
            float lY = hueMaskNarrow(h, 60.0)  * isActiveLight(LightY);
            float lG = hueMaskNarrow(h, 120.0) * isActiveLight(LightG);
            float lC = hueMaskNarrow(h, 180.0) * isActiveLight(LightC);
            float lB = hueMaskNarrow(h, 240.0) * isActiveLight(LightB);
            float lP = hueMaskNarrow(h, 270.0) * isActiveLight(LightP);
            float lM = hueMaskNarrow(h, 300.0) * isActiveLight(LightM);

            float wSum = wR + wO + wY + wG + wC + wB + wP + wM;
            float lSum = lR + lO + lY + lG + lC + lB + lP + lM;
            float hShift = 0.0;
            float sMul = 1.0;
            float lShift = 0.0;
            if (wSum > 1e-5 || lSum > 1e-5)
            {
                // 关键：除以 max(wSum,1) 而不是 wSum。原来除 wSum 会把单个色系的调整归一成“满强度”，
                // 效果从色系中心到掩码边界都是全量，只有到边界才骤降 → 出现棱角分明的色块。
                // 现在单个色系时效果随掩码权重平滑淡出（中心满、越靠近边界越弱）；
                // 只有多个色系重叠（wSum>1）才归一，避免叠加过冲。
                float norm = max(wSum, 1.0);
                float normL = max(lSum, 1.0);
                hShift = (wR*HueR + wO*HueO + wY*HueY + wG*HueG + wC*HueC + wB*HueB + wP*HueP + wM*HueM) / norm;
                sMul = 1.0 + (wR*(SatR-1.0) + wO*(SatO-1.0) + wY*(SatY-1.0) + wG*(SatG-1.0) + wC*(SatC-1.0) + wB*(SatB-1.0) + wP*(SatP-1.0) + wM*(SatM-1.0)) / norm;
                lShift = (lR*LightR + lO*LightO + lY*LightY + lG*LightG + lC*LightC + lB*LightB + lP*LightP + lM*LightM) / normL;
            }

            // OKLCH 中间调色：在感知均匀空间（OKLab/OKLCH，Ottosson 官方参考）施加
            // 主调整 + 分色系调整。感知均匀的亮度/色相可减缓渐变过渡的色带；
            // 暗部特殊处理同样基于感知亮度 L。
            float L2, C2, H2;
            RgbToOklch(c, L2, C2, H2);

            // 明度：主色系 + 分色系；暗部特殊处理 3：L 最小钳制 0.01，避免纯黑奇点
            L2 = saturate(L2 + MasterLight + lShift);
            L2 = max(L2, 0.01);

            // 暗部特殊处理 1+2：L < 0.15 时，彩度增量与色相调整权重按 smoothstep 衰减，
            // 防止暗部过饱和 / 色相偏移造成色阶断裂
            float darkDecay = smoothstep(0.0, 0.15, L2);

            // 色相：wrapHue 保证在 0..360 环上取最短路径；暗部降低色相调整权重
            H2 = wrapHue(H2 + hShift * darkDecay);

            // 彩度：主 × 全局 × 分色系，各自“相对 1.0 的增量”按 darkDecay 衰减
            float satTotal = MasterSat * GlobalSat * sMul;
            C2 = C2 * (1.0 + (satTotal - 1.0) * darkDecay);

            float3 rgb = OklchToRgb(L2, C2, H2);

            rgb.r *= 1.0 + 0.18 * Temperature;
            rgb.g *= 1.0 + 0.04 * Temperature;
            rgb.b *= 1.0 - 0.18 * Temperature;

            rgb = (rgb - 0.5) * Contrast + 0.5 + Brightness;
            rgb *= 1.0 + Highlights * 0.5;
            rgb = rgb * (1.0 - Shadows) + Shadows;

            rgb = saturate(rgb);
            // 抖动：±1.2/255（1.2 LSB）的确定性噪声，打散 8-bit 量化在平滑渐变/掩码过渡区产生的色带
            // （banding），让过渡更自然；幅度很小，肉眼几乎不可见。
            float2 sp = pos.xy;
            float3 dither = float3(
                DitherNoise(sp),
                DitherNoise(sp + float2(7.1, 3.3)),
                DitherNoise(sp + float2(11.7, 5.9))) * (2.4 / 255.0) - (1.2 / 255.0);
            rgb = saturate(rgb + dither);
            return float4(rgb, 1.0);
        }
        """;

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _running = false;
        AppLog.Write("Engine", "Dispose begin");
        var t = _renderThread;
        bool renderAlive = false;
        // 等渲染线程真正退出后再释放 COM，避免它访问已释放对象造成 AccessViolation 闪退
        if (t != null && t.IsAlive && Thread.CurrentThread != t)
        {
            try { t.Join(2000); } catch { }
            renderAlive = t.IsAlive;
        }
        _renderThread = null;

        if (renderAlive)
        {
            // 渲染线程未能在超时内退出：跳过 COM 释放（宁可少量泄漏，绝不访问已释放对象导致闪退）
            AppLog.Write("Engine", "Dispose: 渲染线程未退出，跳过 COM 释放");
            if (_hwnd != IntPtr.Zero)
            {
                try { OverlayNative.DestroyWindow(_hwnd); } catch { }
                _hwnd = IntPtr.Zero;
            }
            return;
        }

        try { lock (_captureLock) { _duplication?.Dispose(); _duplication = null!; } } catch { }
        try { _srv?.Dispose(); } catch { }
        try { _frameTexture?.Dispose(); } catch { }
        try { _constantBuffer?.Dispose(); } catch { }
        try { _vertexBuffer?.Dispose(); } catch { }
        try { _inputLayout?.Dispose(); } catch { }
        try { _ps?.Dispose(); } catch { }
        try { _vs?.Dispose(); } catch { }
        try { _rasterizerState?.Dispose(); } catch { }
        try { _rtv?.Dispose(); } catch { }
        try { _swapChain?.Dispose(); } catch { }
        try { _device?.Dispose(); } catch { }

        if (_hwnd != IntPtr.Zero)
        {
            try { OverlayNative.DestroyWindow(_hwnd); } catch { }
            _hwnd = IntPtr.Zero;
        }
        AppLog.Write("Engine", "Dispose end");
    }
}
