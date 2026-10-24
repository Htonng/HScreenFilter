#include "hlsl.h"
#include "log.h"
#include <unordered_map>
#include <string>
#include <mutex>

namespace hsf {

// ---------------- 顶点着色器（全屏三角形） ----------------
const char* g_vsSource = R"HLSL(
struct VSInput { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOutput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOutput main(VSInput i) { VSOutput o; o.pos = i.pos; o.uv = i.uv; return o; }
)HLSL";

// ---------------- 像素着色器：单遍合并（LUT + 可选线性光后处理） ----------------
// 捕获内容与交换链输出统一为 SDR（B8G8R8A8 / sRGB 编码）：工作空间就是 sRGB，
// 不做任何输入/输出色彩空间转换，也不做 SDR 白点换算（HDR 显示器上的映射交给 Windows）。
//
// 单遍合并（性能修复）：
//   旧实现是两遍 —— 第一遍做 LUT 并把结果写进 R16G16B16A16_FLOAT 中间纹理，
//   第二遍读该中间纹理做 12-tap 邻域后处理再写后缓冲；另加两次全屏清屏。
//   1440p 每帧仅中间纹理的写/读/清屏就约 90MB 显存流量，是 low 帧下降的主因之一。
//   现在合并为一遍：捕获纹理 → （可选）线性光邻域后处理 → LUT 一次采样 → 输出色彩空间
//   → 后缓冲，全程无中间纹理、无清屏、仅一次全屏 draw。
//   PostActive=0（五项后处理参数全为 0）时走快路径：只做 1 次输入采样 + 1 次 LUT 采样，
//   输出与旧两遍实现的恒等路径完全一致。
//   语义差异：后处理由「LUT 之后再锐化」变为「锐化之后再套 LUT」（细节先于 LUT 生效，
//   避免强 LUT 曲线把锐化过冲放大）。
const char* g_psLutSource = R"HLSL(
Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);
Texture3D LutTexture : register(t1);
SamplerState LutSampler : register(s1);

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
    float Sharpen;
    float NoiseReduction;
    float EdgeEnhancement;
    float Clarity;
    float QualityEnhancement;
    float TexelX;
    float TexelY;
    float PostActive;
};

static const float LUT_N = 64.0;
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

float DitherNoise(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

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

// 捕获内容与交换链输出都是 SDR（B8G8R8A8 / sRGB 编码），工作空间就是它本身：
// 不需要任何输入/输出色彩空间转换，也不需要 SDR 白点换算（Windows 自己负责把
// SDR 窗口映射到 HDR 显示器）。这里保留同名函数是为了让下面的主函数改动最小。
float3 InputToSrgbWorking(float3 c)
{
    return saturate(c);
}

float3 SrgbWorkingToOutput(float3 srgb)
{
    return srgb;
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    float3 raw = InputTexture.Sample(InputSampler, uv).rgb;
    float3 work = InputToSrgbWorking(raw);

    // 可选：线性光空间邻域后处理（数学与原第二遍一致，数据源改为捕获纹理，中间纹理已移除）
    if (PostActive > 0.5)
    {
        float2 tx = float2(TexelX, 0.0);
        float2 ty = float2(0.0, TexelY);
        float3 center = SrgbToLinear(work);
        float3 neighbors =
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv - tx).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv + tx).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv - ty).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv + ty).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv - tx - ty).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv - tx + ty).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv + tx - ty).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv + tx + ty).rgb));
        float3 average = neighbors / 8.0;
        float3 denoised = lerp(center, average, NoiseReduction);
        float3 detail = denoised - average;
        float3 enhanced = denoised + detail * (Sharpen + EdgeEnhancement * 0.8);

        float2 wideTx = tx * 2.0;
        float2 wideTy = ty * 2.0;
        float3 wideAverage = (
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv - wideTx).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv + wideTx).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv - wideTy).rgb)) +
            SrgbToLinear(InputToSrgbWorking(InputTexture.Sample(InputSampler, uv + wideTy).rgb))) / 4.0;
        enhanced += (enhanced - wideAverage) * Clarity * 0.7;

        float luminance = dot(enhanced, LUMA);
        float3 quality = (enhanced - luminance) * (1.0 + QualityEnhancement * 0.35) + luminance;
        work = LinearToSrgb(max(lerp(enhanced, quality, QualityEnhancement), 0.0));
    }

    // 把 sRGB 值映射到 LUT 纹理坐标：texel i 代表值 i/(N-1)
    float3 u = (saturate(work) * (LUT_N - 1.0) + 0.5) / LUT_N;
    float3 lutted = LutTexture.Sample(LutSampler, u).rgb;
    float3 outRgb = SrgbWorkingToOutput(lutted);

    // 8bit 输出抖动：抑制暗部渐变的等高线（±1.2/255）
    float2 sp = pos.xy;
    float3 noise = float3(
        DitherNoise(sp),
        DitherNoise(sp + float2(7.1, 3.3)),
        DitherNoise(sp + float2(11.7, 5.9)));
    outRgb = saturate(outRgb + noise * (2.4 / 255.0) - (1.2 / 255.0));
    return float4(outRgb, 1.0);
}
)HLSL";

// ---------------- 像素着色器：中性直通 ----------------
// 中性直通：直接把捕获内容原样呈现（SDR：B8G8R8A8 进、B8G8R8A8 出，无色彩空间转换）
const char* g_psPassthroughSource = R"HLSL(
Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(InputTexture.Sample(InputSampler, uv).rgb, 1.0);
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
    float Sharpen;
    float NoiseReduction;
    float EdgeEnhancement;
    float Clarity;
    float QualityEnhancement;
    float TexelX;
    float TexelY;
    float PostActive;
    float DarkScene;         // 槽 41：暗场景优化的**用户滑块**强度（0..1，不含场景自适应权重）
    float DarkSceneWeight;   // 槽 42：场景自适应权重（0.5..1，渲染线程按场景平均亮度维护）
};

static const float LUT_N = 64.0;
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

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
float SrgbToLinear1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow(max((c + 0.055) / 1.055, 0.0), 2.4);
}

float LinearToSrgb1(float c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 1e-8), 1.0 / 2.4) - 0.055;
}

float3 SrgbToLinear(float3 c)
{
    return float3(SrgbToLinear1(c.r), SrgbToLinear1(c.g), SrgbToLinear1(c.b));
}

float3 LinearToSrgb(float3 c)
{
    return float3(LinearToSrgb1(c.r), LinearToSrgb1(c.g), LinearToSrgb1(c.b));
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
    // 暗场景优化（在基础调色之前，先把过暗的输入提亮）：
    //   亮度曲线 Y' = pow(Y, 1/(1+k))：低端提亮最多、白位固定不动、全程不裁切；
    //   再把 RGB 按 Y'/Y 整体缩放 → 色相/饱和度保持（不会像向白混合那样把暗部洗成灰）；
    //   增益上限 6×（纯黑仍是黑，不浮灰雾）。
    //   强度 = 用户滑块（槽 41）× 场景自适应权重（槽 42）：两者分开传进来，由渲染线程各自维护 ——
    //   旧实现把乘积算在 LutEngine::Apply() 里，而 Apply 只在界面来消息时被调用，于是
    //   「场景一亮 → 乘积为 0 → 之后场景再暗也不重建 → 用户一动滑块就静默失效、重启才恢复」。
    float dsStrength = DarkScene * DarkSceneWeight;
    if (dsStrength > 0.001)
    {
        float Y = dot(saturate(c), LUMA);
        if (Y > 1e-4)
        {
            float e = 1.0 / (1.0 + dsStrength * 1.2);
            float scale = min(pow(Y, e) / Y, 6.0);
            c = saturate(c * scale);
        }
    }

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

    rgb = saturate((rgb - 0.5) * Contrast + 0.5 + Brightness);

    // —— 高光 / 阴影（线性光亮度域的曝光增益 + 掩码 + 高光软拐点）——
    // 三个要点，都是旧实现的缺陷所在，别再改回去：
    //   1) 两段掩码之间留出中间调死区（感知域 0.50 以上阴影为 0，线性光 0.18 以下高光为 0）：
    //      阴影只抬暗端、高光只压/抬亮端，拖一端不会牵连中灰（旧实现两段掩码在 0.40~0.55
    //      重叠，没有死区，「高光」实际上从中灰就开始推）。
    //      两段掩码用的域不同是有意的：高光掩码放在**线性光**域，斜率有界 → 曲线严格单调；
    //      若放在感知域，sRGB→线性在这一段的斜率放大会把 Y·m' 顶上去，出现「输入更亮、
    //      输出反而更暗」的色调反转（旧实现就是这种反转，实测在 sRGB 0.8~1.0 之间反转 16%）。
    //      阴影掩码放在**感知**域：那里 Y·m' 很小，且「暗端」本来就该按看到的亮度划分。
    //   2) 增益施加在**线性光**上：线性 RGB 等比缩放 = 纯曝光变化，色相与饱和度严格不变；
    //      直接乘 sRGB 编码值是「伪曝光」，会顺带把饱和度推歪、暗部出现色偏。
    //      顺带把「高光 −」的力度拉回合理区间：旧实现 sRGB 域把白位压到 0.4（一层灰雾）。
    //   3) 提亮那一路接软拐点：拐点以上按指数收敛到 1，白位基本不动，高光不再被硬裁成死白
    //      （旧实现纯乘法 + saturate，+100 只是把亮部推平）。拐点取 1/gain ⇒ gain=1（滑块为 0）时
    //      这条曲线在 [0,1] 上严格恒等 —— 中性设置绝不能被它改到。
    float3 lin = SrgbToLinear(rgb);
    float Y = dot(lin, LUMA);
    float Lp = LinearToSrgb1(Y);                       // 感知域亮度（0..1）
    float hiMask = smoothstep(0.18, 0.95, Y);          // 亮端权重（线性光域）
    float loMask = 1.0 - smoothstep(0.00, 0.50, Lp);   // 暗端权重（感知域）
    float gain = 1.0 + hiMask * Highlights * 0.5 + loMask * Shadows * 0.9;
    if (abs(gain - 1.0) > 1e-4 && Y > 1e-6)
    {
        float boosted = Y * gain;
        if (gain > 1.0)
        {
            float knee = 1.0 / gain;
            if (boosted > knee)
            {
                float t = (boosted - knee) / (1.0 - knee);
                boosted = knee + (1.0 - knee) * (1.0 - exp(-t));
            }
        }
        lin *= boosted / Y;
        rgb = saturate(LinearToSrgb(lin));
    }

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

// 着色器编译缓存（进程级）：
// 每次启用/重启 LUT 滤镜都会重建管线，d3dcompiler 的 D3DCompile 是纯 CPU 工作，
// 同一个 cs_5_0 编译一次约几十到数百毫秒。缓存按「源码指针 + 入口 + profile」区分，
// 源码都是静态字符串，指针稳定。首次编译仍然要付一次成本，之后开关滤镜/看门狗重建
// 都直接复用 blob —— 这正是「重启几次才不卡」里可以消除的那部分重复开销。
namespace {
std::mutex& ShaderCacheMutex()
{
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, ComPtr<ID3DBlob>>& ShaderCache()
{
    static std::unordered_map<std::string, ComPtr<ID3DBlob>> cache;
    return cache;
}
std::string ShaderCacheKey(const char* source, const char* entry, const char* target)
{
    char key[160];
    snprintf(key, sizeof(key), "%p|%s|%s", (const void*)source, entry, target);
    return std::string(key);
}
} // namespace

bool CompileShader(const char* source, const char* entry, const char* target,
                   ComPtr<ID3DBlob>& blob, std::wstring& error)
{
    error.clear();
    if (!source || !entry || !target)
    {
        error = L"着色器源码/入口为空";
        return false;
    }
    const std::string key = ShaderCacheKey(source, entry, target);
    {
        std::lock_guard<std::mutex> lock(ShaderCacheMutex());
        auto it = ShaderCache().find(key);
        if (it != ShaderCache().end() && it->second)
        {
            blob = it->second;
            return true;
        }
    }

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
    {
        std::lock_guard<std::mutex> lock(ShaderCacheMutex());
        ShaderCache()[key] = blob;
    }
    return true;
}

} // namespace hsf
