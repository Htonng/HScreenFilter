// hlsl.h — 着色器源码（LUT 生成计算着色器 + LUT 采样像素着色器 + 顶点着色器）
// 与 D3DCompile 运行时编译封装（d3dcompiler_47.dll，系统 DLL，运行时加载）。
#pragma once
#include "common.h"
#include "comptr.h"
#include <d3d11.h>
#include <d3dcommon.h>

namespace hsf {

// 常量缓冲布局（44 个 float = 176 字节，16 字节对齐）
// 0..8   : MasterHue, MasterSat, MasterLight, GlobalSat, Temperature, Contrast, Brightness, Highlights, Shadows
// 9..32  : 8 个色系 × (Hue, Sat, Light)
// 33..39 : Sharpen / NoiseReduction / EdgeEnhancement / Clarity / QualityEnhancement / TexelX / TexelY
// 40     : PostActive（0/1：五项后处理参数是否全部为 0，用于单遍着色器的快路径分支）
constexpr int kParamsFloatCount = 44;
constexpr int kLutSize = 64;

// 像素着色器（单遍合并：LUT 采样 + 可选线性光邻域后处理，直出后缓冲）
// 旧实现为两遍（LUT → R16G16B16A16_FLOAT 中间纹理 → 后处理 → 后缓冲），
// 中间纹理的写/读与两次全屏清屏是 GPU 开销与 low 帧下降的主因，现已合并为一遍。
extern const char* g_psLutSource;
// 像素着色器（中性直通：仅采样输入纹理，用于格式无关的透明直通）
extern const char* g_psPassthroughSource;
// 顶点着色器（全屏三角形）
extern const char* g_vsSource;
// 计算着色器（参数变化时重建 LUT）
extern const char* g_csLutSource;

// 运行时编译 HLSL（d3dcompiler_47.dll）
bool CompileShader(const char* source, const char* entry, const char* target,
                   ComPtr<ID3DBlob>& blob, std::wstring& error);

} // namespace hsf
