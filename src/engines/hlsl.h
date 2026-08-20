// hlsl.h — 着色器源码（LUT 生成计算着色器 + LUT 采样像素着色器 + 顶点着色器）
// 与 D3DCompile 运行时编译封装（d3dcompiler_47.dll，系统 DLL，运行时加载）。
#pragma once
#include "common.h"
#include "comptr.h"
#include <d3d11.h>
#include <d3dcommon.h>

namespace hsf {

// 常量缓冲布局（40 个 float = 160 字节，16 字节对齐）
// 0..8   : MasterHue, MasterSat, MasterLight, GlobalSat, Temperature, Contrast, Brightness, Highlights, Shadows
// 9..32  : 8 个色系 × (Hue, Sat, Light)
// 33..39 : 保留（0）
constexpr int kParamsFloatCount = 40;
constexpr int kLutSize = 64;

// 像素着色器（LUT 采样 + 屏幕空间抖动）
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
