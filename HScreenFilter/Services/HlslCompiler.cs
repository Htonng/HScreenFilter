using System;
using System.Runtime.InteropServices;
using System.Text;

namespace HScreenFilter.Services;

/// <summary>
/// 在运行时调用 d3dcompiler_47.dll 把 HLSL 源码编译成像素着色器字节码，
/// 供 Win2D 的 <c>PixelShaderEffect</c> 使用。这样无需在构建期依赖 fxc.exe，
/// 着色器源码可直接以字符串形式内嵌在程序中。
/// </summary>
public static class HlslCompiler
{
    // ID3DBlob：{8BA5FB08-5195-40E2-AC58-0D989C3A0102}
    [ComImport, Guid("8BA5FB08-5195-40E2-AC58-0D989C3A0102")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ID3DBlob
    {
        [PreserveSig]
        IntPtr GetBufferPointer();

        [PreserveSig]
        UIntPtr GetBufferSize();
    }

    // D3DCompile 的 pSourceName / pEntrypoint / pTarget 均为 LPCSTR（ANSI），
    // 因此字符串必须按 CharSet.Ansi 编组，否则入口点名会被错误地按宽字符传递。
    [DllImport("d3dcompiler_47.dll", CharSet = CharSet.Ansi)]
    private static extern int D3DCompile(
        IntPtr pSrcData,
        UIntPtr srcDataSize,
        string pSourceName,
        IntPtr pDefines,
        IntPtr pInclude,
        string pEntrypoint,
        string pTarget,
        uint flags1,
        uint flags2,
        out IntPtr ppCode,
        out IntPtr ppErrorMsgs);

    /// <summary>把 HLSL 顶点着色器源码编译为字节码（vs_4_0）。</summary>
    public static byte[] CompileVertexShader(string hlslSource, string entryPoint = "main")
        => Compile(hlslSource, entryPoint, "vs_4_0");

    /// <summary>把 HLSL 像素着色器源码编译为可执行的着色器字节码（ps_4_0）。</summary>
    public static byte[] CompilePixelShader(string hlslSource, string entryPoint = "main")
        => Compile(hlslSource, entryPoint, "ps_4_0");

    private static byte[] Compile(string hlslSource, string entryPoint, string target)
    {
        if (string.IsNullOrEmpty(hlslSource))
            throw new ArgumentNullException(nameof(hlslSource));

        // 直接使用 UTF-8 字节（着色器源码为纯 ASCII）。
        // 注意：不要加 BOM —— 实测 d3dcompiler_47.dll 对带 BOM 的源码会返回 E_FAIL。
        var bytes = Encoding.UTF8.GetBytes(hlslSource);
        if (bytes.Length == 0)
            throw new InvalidOperationException("HLSL 源码为空");

        IntPtr pSrc = Marshal.AllocHGlobal(bytes.Length);
        try
        {
            Marshal.Copy(bytes, 0, pSrc, bytes.Length);

            int hr = D3DCompile(pSrc, (UIntPtr)bytes.Length, "HslFilter.hlsl",
                IntPtr.Zero, IntPtr.Zero,
                entryPoint, target,
                0 /* D3DCOMPILE_OPTIMIZATION_LEVEL1 默认 */, 0,
                out var pCode, out var pErrorMsgs);

            if (hr != 0)
            {
                string message = "HLSL 编译失败 (0x" + hr.ToString("X8") + ")";
                if (pErrorMsgs != IntPtr.Zero)
                {
                    try
                    {
                        var errBlob = (ID3DBlob)Marshal.GetObjectForIUnknown(pErrorMsgs);
                        var msgPtr = errBlob.GetBufferPointer();
                        int len = (int)errBlob.GetBufferSize();
                        if (len > 0)
                        {
                            var raw = new byte[len];
                            Marshal.Copy(msgPtr, raw, 0, len);
                            var text = Encoding.UTF8.GetString(raw).Trim('\0', '\r', '\n', ' ');
                            if (text.Length > 0) message += "\n" + text;
                        }
                    }
                    catch
                    {
                        // 错误信息读取失败不影响主异常抛出
                    }
                    finally
                    {
                        Marshal.Release(pErrorMsgs);
                    }
                }
                throw new InvalidOperationException(message);
            }

            var blob = (ID3DBlob)Marshal.GetObjectForIUnknown(pCode);
            int size = (int)blob.GetBufferSize();
            IntPtr ptr = blob.GetBufferPointer();
            var result = new byte[size];
            Marshal.Copy(ptr, result, 0, size);
            Marshal.Release(pCode);
            return result;
        }
        finally
        {
            Marshal.FreeHGlobal(pSrc);
        }
    }
}
