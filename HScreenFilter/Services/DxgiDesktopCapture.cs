using System;
using System.Runtime.InteropServices;
using System.Threading;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;

namespace HScreenFilter.Services;

/// <summary>
/// 用 DXGI Desktop Duplication（IDXGIOutputDuplication）捕获主显示器，并把每帧像素
/// 通过 CPU 读回（Map），供 Win2D 的 <c>CanvasBitmap.CreateFromBytes</c> 喂给逐像素
/// 着色器。全程不经过 Windows.Graphics.Capture，因此不会触发 Windows 11 的屏幕捕获
/// 隐私指示器边框（黄框）。
///
/// 说明：Vortice 自建独立 D3D11 设备做捕获（无法从 Win2D CanvasDevice QI 出原生
/// ID3D11Device —— WinRT IDirect3DDevice 不暴露底层设备），因此帧数据走 GPU→CPU→GPU
/// 桥接，帧率会低于 WGC 的全 GPU 方案，但能去掉隐私边框。
/// </summary>
internal sealed class DxgiDesktopCapture : IDisposable
{
    private const uint DXGI_ERROR_WAIT_TIMEOUT = 0x887A0027u;
    private const uint DXGI_ERROR_ACCESS_LOST = 0x887A0026u;

    private readonly ID3D11Device _d3dDevice;
    private readonly ID3D11DeviceContext _context;
    private readonly IDXGIOutputDuplication _duplication;
    private readonly ID3D11Texture2D _staging;
    private readonly int _width;
    private readonly int _height;
    private readonly int _pitch;       // 紧凑行字节数 = width*4
    private readonly byte[][] _buffers; // 环形像素缓冲，避免每帧 Clone
    private int _bufferIndex;
    private const int RingSize = 3;
    private Thread? _thread;
    private volatile bool _running;

    /// <summary>每帧捕获完成后触发，携带 B8G8R8A8 紧凑像素数据（长度 = width*height*4）。</summary>
    public event Action<byte[]>? FrameReady;

    private DxgiDesktopCapture(ID3D11Device d3dDevice, ID3D11DeviceContext context,
        IDXGIOutputDuplication duplication, ID3D11Texture2D staging, int width, int height)
    {
        _d3dDevice = d3dDevice;
        _context = context;
        _duplication = duplication;
        _staging = staging;
        _width = width;
        _height = height;
        _pitch = width * 4;
        int stride = width * height * 4;
        _buffers = new byte[RingSize][];
        for (int i = 0; i < RingSize; i++) _buffers[i] = new byte[stride];
    }

    /// <summary>创建 DXGI 桌面捕获。失败时返回 null 并给出 <paramref name="error"/>。</summary>
    public static DxgiDesktopCapture? Create(int width, int height, out string error)
    {
        error = "";
        try
        {
            // 1) 用 Vortice 创建 D3D11 设备（BGRA 支持，供 DXGI 桌面纹理使用）
            //    特性级别传 null（用默认），此处用 null! 表明有意为之
            var d3dDevice = D3D11.D3D11CreateDevice(DriverType.Hardware, DeviceCreationFlags.BgraSupport, null!);
            var context = d3dDevice.ImmediateContext;

            // 2) DXGI 链：IDXGIDevice → Adapter → Output0 → IDXGIOutput1 → DuplicateOutput
            using var dxgiDevice = d3dDevice.QueryInterface<IDXGIDevice>();
            var adapter = dxgiDevice.GetAdapter();
            adapter.EnumOutputs(0, out var output);
            using var output1 = output.QueryInterface<IDXGIOutput1>();
            var duplication = output1.DuplicateOutput(d3dDevice);

            // 3) CPU 可读的 staging 纹理（用于把桌面纹理拷回 CPU）
            var desc = new Texture2DDescription
            {
                Width = (uint)width,
                Height = (uint)height,
                MipLevels = 1,
                ArraySize = 1,
                Format = Format.B8G8R8A8_UNorm,
                SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Staging,
                BindFlags = BindFlags.None,
                CPUAccessFlags = CpuAccessFlags.Read,
            };
            var staging = d3dDevice.CreateTexture2D(desc);

            return new DxgiDesktopCapture(d3dDevice, context, duplication, staging, width, height);
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return null;
        }
    }

    /// <summary>启动后台捕获线程。</summary>
    public void Start()
    {
        if (_running) return;
        _running = true;
        _thread = new Thread(CaptureLoop) { IsBackground = true, Name = "DxgiDesktopCapture" };
        _thread.Start();
    }

    private void CaptureLoop()
    {
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
                        _context.CopyResource(_staging, tex);
                        var buf = _buffers[_bufferIndex];
                        _bufferIndex = (_bufferIndex + 1) % RingSize;
                        ReadStagingToPixels(buf);
                        FrameReady?.Invoke(buf);
                    }
                    finally
                    {
                        resource.Dispose();
                    }
                    _duplication.ReleaseFrame();
                }
                else if (result.Code == unchecked((int)DXGI_ERROR_WAIT_TIMEOUT))
                {
                    continue; // 无新帧
                }
                else
                {
                    break; // 桌面变化/其它错误 → 上层会重建或回退
                }
            }
            catch
            {
                break;
            }
        }
        _running = false;
    }

    /// <summary>把 staging 纹理按行读回目标缓冲（处理行对齐）。</summary>
    private void ReadStagingToPixels(byte[] dest)
    {
        var box = _context.Map(_staging, 0, MapMode.Read, Vortice.Direct3D11.MapFlags.None);
        try
        {
            IntPtr src = box.DataPointer;
            int srcPitch = (int)box.RowPitch;
            int dstOffset = 0;
            for (int row = 0; row < _height; row++)
            {
                Marshal.Copy(src, dest, dstOffset, _pitch);
                src = IntPtr.Add(src, srcPitch);
                dstOffset += _pitch;
            }
        }
        finally
        {
            _context.Unmap(_staging, 0);
        }
    }

    public void Dispose()
    {
        _running = false;
        try { _duplication.Dispose(); } catch { }
        try { _staging.Dispose(); } catch { }
        try { _d3dDevice.Dispose(); } catch { }
    }
}
