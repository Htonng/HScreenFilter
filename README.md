# HScreenFilter 屏幕滤镜

一个基于 **WinUI 3** 的 Windows 屏幕滤镜工具，通过滑块实时调节屏幕的**亮度、对比度、鲜艳度、亮部、暗部**（附带色温），并支持**全局快捷键一键切换配置文件**。关闭窗口后驻留系统托盘，滤镜持续生效。

---

## ✨ 功能特性

| 功能 | 说明 |
| --- | --- |
| 🎚️ 五个核心滑块 | 亮度、对比度、鲜艳度（饱和度）、亮部、暗部，拖动即时生效 |
| 🌡️ 色温（附加） | 冷 ↔ 暖色调，适合护眼/夜间场景 |
| 📁 配置文件 | 保存/更新/删除/应用多套配置，一键切换 |
| ⌨️ 全局快捷键 | 每个配置可绑定一个全局热键（如 `Ctrl+Alt+1`），按下即切换；另有全局开关热键 |
| 🎨 快捷预设 | 标准 / 护眼 / 夜间 / 鲜艳 一键套用 |
| 🖥️ 系统托盘 | 关闭窗口最小化到托盘，滤镜保持生效；托盘菜单可退出 |
| 🚀 开机自启 | 可选开机自动启动 |

---

## 🛠️ 技术原理

滤镜基于 **5×5 颜色变换矩阵**（`MAGCOLOREFFECT`）实现，通过 Windows 放大镜 API 的 `MagSetFullscreenColorEffect` 对整个桌面应用颜色效果：

```
处理顺序：色温(tint) → 鲜艳度(saturation) → 对比度(contrast) → 亮度(brightness) → 亮部(gain) → 暗部(lift)
```

- **鲜艳度**：围绕 Rec.709 灰度轴缩放色度（跨通道混合，只有颜色矩阵能做到）。
- **对比度**：围绕 0.5 中灰缩放。
- **亮度**：加法偏移。
- **亮部 / 暗部**：在单线性矩阵下采用"色阶（Lift/Gain）"的线性近似——亮部=乘法增益，暗部=加法偏移，更接近照片处理中"高光/阴影"的效果。

> **引擎回退**：全屏颜色效果需要 **Windows 10 1903（build 18362）及以上**。若系统不支持（如被组策略禁用），会自动回退到**显卡伽马曲线**（`SetDeviceGammaRamp`），此时仍支持亮度/对比度/亮部/暗部/色温，但**鲜艳度不可用**（伽马表无法跨通道混色）。

---

## 📦 环境要求

- **系统**：Windows 10 1809+；建议 Windows 10 1903+ 或 Windows 11（全屏颜色效果需要 1903+）
- **.NET 8 Desktop Runtime**（本机已安装；换机器需安装）
- **构建**：.NET SDK 8.0+（本仓库已在用户目录安装 `8.0.423`）

> 说明：本项目使用 **Windows App SDK 2.3.1 自包含部署**（`WindowsAppSDKSelfContained=true`），运行时随程序一并打包，无需在系统里单独安装 Windows App Runtime。

---

## 🔨 构建与运行

```powershell
# 构建（Release）
.\build.ps1

# 运行
.\run.ps1
```

或手动：

```powershell
dotnet build .\HScreenFilter\HScreenFilter.csproj -c Release
# 产物：HScreenFilter\bin\Release\net8.0-windows10.0.19041.0\win-x64\HScreenFilter.exe
```

> 本项目已适配 **纯 `dotnet build`（无需 Visual Studio）** 的构建流程，直接使用当前稳定的 Windows App SDK 即可编译通过。

---

## 🎯 使用指南

1. **启用滤镜**：打开顶部"启用滤镜"开关，画面立即按当前参数变化。
2. **调节参数**：拖动滑块实时预览；右侧显示数值。
3. **保存配置**：点击"新建配置"命名保存当前参数；"更新所选"用当前参数覆盖所选配置；"应用所选"切换到某配置。
4. **绑定快捷键**：
   - 选中一个配置 → 点击"设置快捷键…" → 按下组合键（如 `Ctrl+Alt+1`）完成绑定。
   - 之后无论在任何程序里按该组合键，都会一键切换到该配置。
   - "全局开关快捷键"用于整体启用/禁用滤镜。
5. **最小化到托盘**：点击窗口关闭按钮即隐藏到托盘（滤镜继续生效）。左键单击托盘图标显示窗口，右键可"退出"。
6. **开机自启**：勾选"开机自动启动"。

> 配置与当前状态保存在：`%LocalAppData%\HScreenFilter\profiles.json`

---

## ⚠️ 注意事项

- **与系统颜色滤镜冲突**：`MagSetFullscreenColorEffect` 是系统级单槽效果（与 Windows 自带的"颜色滤镜"`Win+Ctrl+C` 共用同一个槽位）。若启用了系统颜色滤镜，两者会互相覆盖；建议使用时关闭系统颜色滤镜。
- **全屏独占应用**：某些全屏独占（exclusive）游戏/程序可能不受颜色效果影响，这是系统限制。
- **安全桌面**：锁屏界面、UAC 提权界面等"安全桌面"不会应用滤镜（系统为安全考虑）。
- **HDR 显示**：在 HDR 模式下颜色效果的表现可能与 SDR 不同。
- **快捷键冲突**：全局快捷键不能与系统或其他程序已占用的组合键重复；若设置失败，界面会提示。

---

## 📁 项目结构

```
HScreenFilter/
├── HScreenFilter.csproj   # 项目文件（Windows App SDK 2.3.1，自包含部署）
├── app.manifest             # DPI 感知等清单
├── App.xaml / App.xaml.cs   # 应用入口（单实例）
├── Program.cs               # 自定义 Main（捕获启动异常）
├── MainWindow.xaml(.cs)     # 主界面：滑块 / 配置 / 快捷键
├── Controls/
│   └── FilterSlider.xaml(.cs)  # 带标题和数值的滑块控件
├── Models/
│   ├── FilterSettings.cs    # 滤镜参数
│   ├── Profile.cs           # 配置文件
│   ├── ProfileData.cs       # 持久化状态
│   └── HotkeyText.cs        # 快捷键文本格式化
└── Services/
    ├── FilterEngine.cs      # 颜色矩阵引擎（MagSetFullscreenColorEffect）
    ├── GammaRampEngine.cs   # 回退引擎（SetDeviceGammaRamp）
    ├── HotkeyService.cs     # 全局热键
    ├── MessageWindow.cs     # 原生消息窗口（收 WM_HOTKEY / 托盘消息）
    ├── ProfileStore.cs      # JSON 持久化
    ├── TrayIcon.cs          # 系统托盘
    └── AutoStart.cs         # 开机自启
```

---

## 📜 免责声明

本工具通过系统公开 API 调整屏幕显示效果，仅供个人护眼/观感调节使用。请勿用于掩盖屏幕内容等用途。
