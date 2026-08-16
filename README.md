# HScreenFilter 屏幕滤镜（C++ 版）

用 **C++17 / Win32 原生控件** 从零重构的屏幕滤镜工具：单 exe、仅依赖系统 DLL。
**UI 采用「左侧导航 + 右侧内容页」的设置风格**（类系统设置 / Android 设置）——
全部使用系统标准控件（comctl32 v6 现代主题），由系统负责 DPI、命中与绘制；
HSL 分色系调色采用 **64³ 3D LUT 管线**，效率较旧版大幅提升。

> 本项目是 `C:\~\.Code_old\滤镜.worktrees\add-hsl-color-slider-filter`（C#/WinUI 3 版）
> 的 C++ 重写。配置数据（`%LocalAppData%\HScreenFilter\profiles.json`）与旧版**完全兼容**。

---

## ✨ 功能（与旧版一致）

| 导航页 | 功能 |
| --- | --- |
| **基础调节** | 亮度 / 对比度 / 鲜艳度 / 亮部 / 暗部 + 色温 滑块（数值可直接输入）+ 快捷预设（默认值/护眼/夜间/鲜艳） |
| **HSL 调色盘** | **红 / 橙 / 黄 / 绿 / 青 / 蓝 / 紫 / 品红** 8 色系 + 全部（主），每色系独立调整色相 / 饱和度 / 明度；选项卡切换三类滑块，滑块下方带 Photoshop 式渐变轨道；LUT 引擎开关 + V-Sync |
| **按应用切换** | 前台进程命中检测，自动切换绑定配置或自动关闭；添加/编辑/删除检测应用 |
| **配置与快捷键** | 配置列表（名称 / 引擎 / 快捷键 / 启用勾选）、新建/重命名/删除/导入/导出/上移下移、每配置全局热键、界面主题（默认/Mica）、OBS 捕获、开机自启、自启进托盘 |

顶部常驻：显示器选择 + 每显示器启用 + 滤镜总开关 + 全局开关快捷键 + 引擎状态。

## 🛠️ 技术要点

- **UI（导航式原生控件）**：左侧 ListView 导航 + 右侧内容页即时切换，无整体滚动；
  全部控件为系统标准控件（comctl32 v6，清单启用现代主题），PerMonitorV2 DPI 由内嵌清单保证，
  任意缩放比例下由系统正确渲染与命中——彻底规避自绘 UI 的错位/模糊/点击偏差问题。
- **HSL 效率（3D LUT）**：参数变化时由计算着色器（cs_5_0）对 64³ 格子点并行生成 LUT（亚毫秒级），
  每帧像素着色器只做一次三线性采样；中性参数直通。算法与旧版逐行一致（HSL 软掩码 → OKLab → 暗部衰减）。
- **引擎回退链**：LUT 逐像素引擎 → 放大镜颜色矩阵 → 显卡伽马曲线。

## 🔨 构建

前置：**便携 llvm-mingw 工具链**。设置环境变量 `LLVM_MINGW` 指向工具链根目录，然后：

```powershell
.\build.ps1          # 输出 build\HScreenFilter.exe
```

产物只有一个 `HScreenFilter.exe`（+ 可选 `assets\icon.ico`），动态依赖仅为系统 DLL
（d3d11 / dxgi / d3dcompiler_47 / shell32 / comctl32 / user32 / gdi32 / dwmapi 等）。

## 🚀 运行

```powershell
.\run.ps1                            # 启动
.\build\HScreenFilter.exe --selftest # 自检（JSON/着色器/D3D/显示器，不动屏幕）
.\build\HScreenFilter.exe --enginetest # 引擎链路测试（短暂应用滤镜后恢复）
```

## 📁 源码结构

```
src\
├── common.* / log.* / json.* / models.* / store.*   # 基础与数据（profiles.json 兼容旧版）
├── autostart.* / monitors.* / msgwindow.* / hotkeys.* / tray.* / fgwatcher.*
├── engines\
│   ├── hlsl.*                  # LUT 计算着色器 / 采样像素着色器 + D3DCompile 封装
│   ├── lut_engine.*            # DXGI 覆盖层 + 桌面捕获 + 3D LUT 管线（主引擎）
│   ├── mag_engine.* / gamma_engine.* / filter_engine.*
└── ui\
    ├── main_window.*           # 导航式原生控件布局 + 全部应用逻辑
    └── dialogs.*               # 模态对话框（新建/编辑/导入导出/确认/倒计时）
```

## 📜 许可证

GPL-3.0（与旧版一致）。

