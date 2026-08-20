#include "hlsl.h"
#include "log.h"

namespace hsf {

// ---------------- 顶点着色器（全屏三角形） ----------------
const char* g_vsSource = R"HLSL(
struct VSInput { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOutput main(VSInput i) { VSOutput o; o.pos = i.pos; o.uv = i.uv; return o; }
)HLSL";

// ---------------- 像素着色器：LUT 采样 + 屏幕空间抖动 ----------------
// 输入 sRGB → 3D LUT 三线性采样 → 输出 sRGB。
// 抖动在屏幕空间进行（确定性伪随机，同一像素每帧一致）。
const char* g_psLutSource = R"HLSL(
Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);
Texture3D LutTexture : register(t1);
SamplerState LutSampler : register(s1);

static const float LUT_N = 64.0;

float DitherNoise(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    float4 color = InputTexture.Sample(InputSampler, uv);

    // 把 sRGB 值映射到 LUT 纹理坐标：texel i 代表值 i/(N-1)
    float3 u = (color.rgb * (LUT_N - 1.0) + 0.5) / LUT_N;
    float3 rgb = LutTexture.Sample(LutSampler, u).rgb;

    // 抖动：±1.2/255 确定性噪声，打散 8-bit 量化在平滑渐变/掩码过渡区产生的色带
    float2 sp = pos.xy;
    float3 dither = float3(
        DitherNoise(sp),
        DitherNoise(sp + float2(7.1, 3.3)),
        DitherNoise(sp + float2(11.7, 5.9))) * (2.4 / 255.0) - (1.2 / 255.0);
    rgb = saturate(rgb + dither);

    return float4(rgb, 1.0);
}
)HLSL";

// ---------------- 像素着色器：中性直通 ----------------
// 直接采样输入纹理输出。与 CopyResource 相比不要求源纹理与后缓冲格式一致，
// HDR/格式切换时也不会失败。
const char* g_psPassthroughSource = R"HLSL(
Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return InputTexture.Sample(InputSampler, uv);
}
)HLSL";

// ---------------- 计算着色器：参数变化时重建 3D LUT ----------------
// 与旧版逐像素 HSL 分色系着色器的数学完全一致（RGB→HSL 掩码 → OKLCH 调色 → 基础调节），
// 区别在于只在参数变化时对 64^3 个格子点各算一次（GPU 并行，亚毫秒级），
// 每帧的像素着色器只做一次三线性采样 —— 大幅降低每像素开销。
const char* g_csLutSource = R"HLSL(
RWTexture3D<float4> LutOut : register(u0);

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
    float4 Padding;
};

static const float LUT_N = 64.0;

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

// —— OKLab/OKLCH（Björn Ottosson 2020 官方参考实现，含健壮性处理）——
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
    H = C < 1e-5 ? 0.0 : wrapHue(atan2(b, a) * 57.2957795);
}

float3 OklchToRgb(float L, float C, float H)
{
    float hr = H * 0.0174532925;
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

// 色系软掩码：以参考色相为中心，半宽 60°，平滑衰减到 0
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

float hueMaskNarrow(float h, float refHue)
{
    float a = abs(h - refHue);
    a = min(a, 360.0 - a);
    float t = a / 60.0;
    float w = saturate(1.0 - t);
    return w * w * (3.0 - 2.0 * w);
}

float isActiveLight(float light)
{
    return abs(light) > 0.001 ? 1.0 : 0.0;
}

float4 Evaluate(float3 c)
{
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
        // 除以 max(wSum,1)：单个色系时效果随掩码权重平滑淡出（中心满、越靠近边界越弱）；
        // 多个色系重叠（wSum>1）才归一，避免叠加过冲。
        float norm = max(wSum, 1.0);
        float normL = max(lSum, 1.0);
        hShift = (wR*HueR + wO*HueO + wY*HueY + wG*HueG + wC*HueC + wB*HueB + wP*HueP + wM*HueM) / norm;
        sMul = 1.0 + (wR*(SatR-1.0) + wO*(SatO-1.0) + wY*(SatY-1.0) + wG*(SatG-1.0) + wC*(SatC-1.0) + wB*(SatB-1.0) + wP*(SatP-1.0) + wM*(SatM-1.0)) / norm;
        lShift = (lR*LightR + lO*LightO + lY*LightY + lG*LightG + lC*LightC + lB*LightB + lP*LightP + lM*LightM) / normL;
    }

    // OKLCH 中间调色：感知均匀空间施加主调整 + 分色系调整
    float L2, C2, H2;
    RgbToOklch(c, L2, C2, H2);

    L2 = saturate(L2 + MasterLight + lShift);
    L2 = max(L2, 0.01);

    // 暗部特殊处理：L < 0.15 时彩度增量与色相调整权重按 smoothstep 衰减
    float darkDecay = smoothstep(0.0, 0.15, L2);

    H2 = wrapHue(H2 + MasterHue + hShift * darkDecay);

    float satTotal = MasterSat * GlobalSat * sMul;
    C2 = C2 * (1.0 + (satTotal - 1.0) * darkDecay);

    float3 rgb = OklchToRgb(L2, C2, H2);

    // 基础调节（与旧版 FilterEngine.BuildMatrix 语义一致）
    rgb.r *= 1.0 + 0.18 * Temperature;
    rgb.g *= 1.0 + 0.04 * Temperature;
    rgb.b *= 1.0 - 0.18 * Temperature;

    rgb = (rgb - 0.5) * Contrast + 0.5 + Brightness;
    rgb *= 1.0 + Highlights * 0.5;
    rgb = rgb * (1.0 - Shadows) + Shadows;

    rgb = saturate(rgb);
    return float4(rgb, 1.0);
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= uint3(LUT_N, LUT_N, LUT_N))) return;
    float3 c = float3(id) / (LUT_N - 1.0);
    LutOut[id] = Evaluate(c);
}
)HLSL";

// ---------------- 运行时编译 ----------------

struct D3DCompiler
{
    HMODULE mod = nullptr;
    typedef HRESULT(WINAPI* D3DCompileFn)(
        LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
        const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
        LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
        ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);
    D3DCompileFn Compile = nullptr;

    bool Ensure()
    {
        if (Compile) return true;
        mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!mod) return false;
        Compile = (D3DCompileFn)GetProcAddress(mod, "D3DCompile");
        return Compile != nullptr;
    }
};

static D3DCompiler& Compiler()
{
    static D3DCompiler c;
    return c;
}

bool CompileShader(const char* source, const char* entry, const char* target,
                   ComPtr<ID3DBlob>& blob, std::wstring& error)
{
    error.clear();
    if (!Compiler().Ensure())
    {
        error = L"无法加载 d3dcompiler_47.dll（系统 DLL 缺失）";
        return false;
    }
    ID3DBlob* pCode = nullptr;
    ID3DBlob* pErr = nullptr;
    HRESULT hr = Compiler().Compile(source, strlen(source), "HslFilter.hlsl",
                                    nullptr, nullptr, entry, target,
                                    0 /* 默认优化级别 */, 0, &pCode, &pErr);
    if (FAILED(hr))
    {
        if (pErr)
        {
            char* msg = (char*)pErr->GetBufferPointer();
            SIZE_T len = pErr->GetBufferSize();
            if (msg && len > 0)
            {
                std::string s(msg, (size_t)len);
                // 去掉尾部空白
                while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                    s.pop_back();
                error = Utf8ToWide(s);
            }
            pErr->Release();
        }
        if (error.empty())
            error = Format(L"HLSL 编译失败 (0x%08X)", (unsigned)hr);
        return false;
    }
    blob = pCode;
    return true;
}

} // namespace hsf
