// mag_engine.h — 放大镜 API 全屏颜色效果引擎（回退引擎：整屏线性颜色矩阵）。
#pragma once
#include "common.h"
#include "models.h"

namespace hsf {

// 5×5 全屏颜色变换矩阵（MAGCOLOREFFECT，GDI+ 约定）
struct MagColorEffect
{
    float transform[25] = {};

    static MagColorEffect Identity()
    {
        MagColorEffect m;
        m.transform[0] = 1.f;   // R
        m.transform[6] = 1.f;   // G
        m.transform[12] = 1.f;  // B
        m.transform[18] = 1.f;  // A
        m.transform[24] = 1.f;  // 末行
        return m;
    }
};

class MagEngine
{
public:
    MagEngine() = default;
    ~MagEngine() { Uninitialize(); }

    bool Initialize();   // MagInitialize
    bool Apply(const FilterSettings& s);
    void Reset();        // 应用单位矩阵
    void Uninitialize();
    bool IsInitialized() const { return initialized_; }
    std::wstring LastError;

    // 由滤镜参数构建 5×5 颜色矩阵（与旧版 FilterEngine.BuildMatrix 语义一致）
    static MagColorEffect BuildMatrix(const FilterSettings& s);

private:
    bool initialized_ = false;
    HMODULE mod_ = nullptr;
    bool LoadDll();
};

} // namespace hsf
