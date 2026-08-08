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
    private int _width;
    private int _height;

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

    // 着色器参数（与 HLSL cbuffer 顺序一致，36 个 float = 144 字节，16 字节对齐：
    //   0..32 滤镜参数，33..35 padding）
    private readonly object _paramsLock = new();
    private readonly float[] _params = new float[36];
    private readonly float[] _paramsCopy = new float[36];

    // 帧率节流：Present(0) 无 vsync；桌面无变化时 AcquireNextFrame(100ms) 自然兜底，
    // 动态画面不设上限（MinFrameIntervalMs=0 = 不节流，用户要求解锁帧率）
    private readonly System.Diagnostics.Stopwatch _frameTimer = new();
    private const int MinFrameIntervalMs = 0;

    // 捕获重建锁：保护 _duplication 跨线程重建
    private readonly object _captureLock = new();

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
    public bool Start(int width, int height)
    {
        _width = width;
        _height = height;
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

    /// <summary>用滤镜参数更新着色器常量（线程安全：只写入参数缓冲，渲染线程每帧读取）。</summary>
    public void Apply(FilterSettings s)
    {
        var p = _params;
        lock (_paramsLock)
        {
            // 与 HLSL cbuffer 字段顺序保持一致
            p[0] = (float)s.Hue;                                  // MasterHue
            p[1] = (float)(s.HslSaturation / 100.0);              // MasterSat
            p[2] = (float)(s.Lightness / 100.0);                  // MasterLight
            p[3] = (float)(s.Saturation / 100.0);                 // GlobalSat
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
                p[idx++] = ch == null ? 0f : (float)ch.Hue;                    // Hue
                p[idx++] = ch == null ? 1f : (float)(ch.Saturation / 100.0);   // Sat
                p[idx++] = ch == null ? 0f : (float)(ch.Lightness / 100.0);    // Light
            }
            // 补齐 padding（33..35），与 HLSL float4 对齐
            for (; idx < 36; idx++) p[idx] = 0f;
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
            style, 0, 0, _width, _height, IntPtr.Zero, IntPtr.Zero, hInstance, IntPtr.Zero);
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

        // 常量缓冲：36 个 float = 144 字节（0..32 滤镜参数 + 33..35 padding）
        _constantBuffer = _device.CreateBuffer(
            new BufferDescription(36 * sizeof(float), BindFlags.ConstantBuffer));

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
        adapter.EnumOutputs(0, out var output);
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
        int cx = _width / 2;
        int cy = _height / 2;
        int off = cy * (int)mapped.RowPitch + cx * 4; // B8G8R8A8：B,G,R,A
        byte b = Marshal.ReadByte(mapped.DataPointer, off);
        byte g = Marshal.ReadByte(mapped.DataPointer, off + 1);
        byte r = Marshal.ReadByte(mapped.DataPointer, off + 2);
        byte a = Marshal.ReadByte(mapped.DataPointer, off + 3);
        _context.Unmap(staging, 0);
        return (r, g, b, a);
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

        float hueMask(float h, float refHue)
        {
            float a = abs(h - refHue);
            a = min(a, 360.0 - a);
            float t = a / 40.0;
            float w = saturate(1.0 - t);
            return w * w * (3.0 - 2.0 * w);
        }

        float isActive(float h, float sat, float light)
        {
            return (abs(h) > 0.01 || abs(sat - 1.0) > 0.001 || abs(light) > 0.001) ? 1.0 : 0.0;
        }

        float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
        {
            float4 color = InputTexture.Sample(InputSampler, uv);

            float3 c = color.rgb;

            float h, s, l;
            rgbToHsl(c, h, s, l);

            h = wrapHue(h + MasterHue);
            s = saturate(s * MasterSat);
            l = saturate(l + MasterLight);
            s = saturate(s * GlobalSat);

            float wR = hueMask(h, 0.0)   * isActive(HueR, SatR, LightR);
            float wO = hueMask(h, 30.0)  * isActive(HueO, SatO, LightO);
            float wY = hueMask(h, 60.0)  * isActive(HueY, SatY, LightY);
            float wG = hueMask(h, 120.0) * isActive(HueG, SatG, LightG);
            float wC = hueMask(h, 180.0) * isActive(HueC, SatC, LightC);
            float wB = hueMask(h, 240.0) * isActive(HueB, SatB, LightB);
            float wP = hueMask(h, 270.0) * isActive(HueP, SatP, LightP);
            float wM = hueMask(h, 300.0) * isActive(HueM, SatM, LightM);

            float wSum = wR + wO + wY + wG + wC + wB + wP + wM;
            if (wSum > 1e-5)
            {
                float hShift = (wR*HueR + wO*HueO + wY*HueY + wG*HueG + wC*HueC + wB*HueB + wP*HueP + wM*HueM) / wSum;
                float sMul = 1.0 + (wR*(SatR-1.0) + wO*(SatO-1.0) + wY*(SatY-1.0) + wG*(SatG-1.0) + wC*(SatC-1.0) + wB*(SatB-1.0) + wP*(SatP-1.0) + wM*(SatM-1.0)) / wSum;
                float lShift = (wR*LightR + wO*LightO + wY*LightY + wG*LightG + wC*LightC + wB*LightB + wP*LightP + wM*LightM) / wSum;
                h = wrapHue(h + hShift);
                s = saturate(s * sMul);
                l = saturate(l + lShift);
            }

            float3 rgb = hslToRgb(h, s, l);

            rgb.r *= 1.0 + 0.18 * Temperature;
            rgb.g *= 1.0 + 0.04 * Temperature;
            rgb.b *= 1.0 - 0.18 * Temperature;

            rgb = (rgb - 0.5) * Contrast + 0.5 + Brightness;
            rgb *= 1.0 + Highlights * 0.5;
            rgb = rgb * (1.0 - Shadows) + Shadows;

            rgb = saturate(rgb);
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
