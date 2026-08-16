# WebView2 · Flat Design 原型说明

用 **WebView2** 承载的 HScreenFilter 扁平化 UI 原型（演示用途，未接入主程序）。

## 交付物

| 文件 | 说明 |
| --- | --- |
| webui/index.html / styles.css / app.js | Flat Design 界面原型（左侧导航 + 右侧内容页，交互完整：页面切换、滑块、HSL 色系/选项卡、预设、开关、保存条、浅/深色主题） |
| webui/index-dark.html | 深色主题预览副本（直接打开即可） |
| src/webview2_demo.cpp | WebView2 宿主 demo：固定 680×860 客户区窗口，动态加载 WebView2Loader.dll（不依赖 SDK 的 .lib），加载 webui/index.html，含导航/消息/进程退出日志与 CapturePreview 截图 |
| build-webview2-demo.ps1 | 构建脚本（llvm-mingw，-static 静态链接 libc++，产物无外部 C++ 运行库依赖） |
| build/webview2_demo.exe | 构建产物（需与 WebView2Loader.dll、webui/ 同目录） |
| build/flat_light.png / build/flat_dark.png | 本地无头渲染的界面效果图（浅色/深色） |

## 构建

powershell: .\build-webview2-demo.ps1   （输出 build\webview2_demo.exe，含 WebView2Loader.dll 与 webui\）

## 运行

powershell: .\build\webview2_demo.exe

## 白屏根因与修复（重要更正）

> 早前误判为火绒拦截。实际根因在宿主代码，与安全软件无关（加白不解决）。

**根因 1：WebView2 环境对象未保存。**
`EnvCreatedHandler` 回调返回后，运行时释放了传给回调的 `ICoreWebView2Environment*` 引用；
宿主没有保存（未 AddRef），环境被销毁，连带销毁控制器 → 浏览器窗口关闭 → 浏览器进程优雅退出
（浏览器日志中的 `RemoveKeepAlive(kBrowserWindow) → Unloading profile` 序列是铁证，杀毒软件不会产生优雅退出）。
浏览器进程看似"1 秒内消失"，其实是环境销毁导致的正常退出。
修复：保存 `g_env` 并 AddRef；同时把控制器/WebView 也 AddRef，并先 ShowWindow 再创建控制器。

**根因 2：WM_GETMINMAXINFO 写死 680x860（未乘 DPI）。**
min=max 轨道尺寸把窗口强制回 680x860 物理像素，125% 缩放下窗口只有设计尺寸的 80%。
修复：使用按 DPI 缩放后的实际外框尺寸。

**验证（当前全部通过）：**
- 浏览器进程持续存活（12s 轮询）
- `[nav-start]` → `[nav] success=1`
- 页面 JS 的完整状态经 postMessage 到达宿主日志
- `CapturePreview` 成功，预览图 850x1075（与窗口一致）

## 快速预览（不经过 WebView2）

powershell: start msedge "F:\code\HScreenFilter\webui\index.html"

## 更新记录（第二轮）

1. 标题「屏幕滤镜」→「HScreenFilter」
2. HSL 渐变轨道修正：色相=全彩虹（所有色系一致）；饱和度=灰→该色系纯色；明亮度=黑→纯色→白（Photoshop 式）
3. 配置与快捷键：启用列改为按钮，n选1（同时更新行首圆点指示）
4. 界面主题新增「跟随系统」，默认该项（matchMedia 监听系统深浅自动切换）
5. 顶部工具栏改为两行（显示器/启用/滤镜总开关 + 全局快捷键），窄宽不再溢出
6. 全部开关绑定统一 state 并通过 postMessage 同步到 WebView2 宿主（LUT 关闭→HSL 禁用；按应用关闭→列表/按钮禁用；总开关→引擎状态；主题→即时应用）

## demo2：WebView2 桥接版（真实数据 + 真实引擎）

demo1 是纯 UI 原型；demo2 把 UI 真正接进了原项目的数据与引擎。

### 交付物

| 文件 | 说明 |
| --- | --- |
| webui2/index.html / styles.css / app.js | 桥接版页面：显示器/配置/按应用列表全部由宿主数据动态渲染，滑块范围与原项目一致 |
| src/webview2_demo2.cpp | 宿主：Store 读写 profiles.json + Monitors 枚举显示器 + FilterEngine 实时应用 + WebView2 |
| build-webview2-demo2.ps1 | 构建脚本（链接真实数据层 + 滤镜引擎） |
| build/webview2_demo2.exe | 产物（含 WebView2Loader.dll + webui2/） |

### 构建 / 运行

powershell: .\build-webview2-demo2.ps1  然后  .\build\webview2_demo2.exe

### 滑块范围（与原项目 main_window.cpp / models.h 严格一致）

- 基础：亮度/亮部/暗部/色温 -100..100；对比度/鲜艳度 0..200
- HSL 色相：全部（主）-180..180；分色系 -30..30
- HSL 饱和度：0..200（100 中性，数值框显示相对值 +20/-15）
- HSL 明亮度：-30..30
- 快捷预设：默认 / 护眼(B-5,C95,S95,T25) / 夜间(B-40,C100,S90,T45) / 鲜艳(C110,S150)

### 桥接协议

- 宿主 → 页面：PostWebMessageAsJson 发送 init/sync（displays、settings、profiles、bindings、开关状态、引擎文本）
- 页面 → 宿主：postMessage 发送 base/hsl/preset/hsl-reset/switch/display/profile-activate/save/cancel
- 保存：写回 %LocalAppData%\HScreenFilter\profiles.json（格式与原版完全兼容，字段一致）
- 取消：还原上次快照并重新应用引擎
- 引擎：滑块/开关变化实时调用 FilterEngine.Apply（LUT/放大镜/伽马按 UseDxgi 与可用性自动选择）

### 注意

- 启用滤镜开关为 ON 且显示器启用时，demo2 会真实过滤屏幕（与原版行为一致）；关闭后恢复
- 快捷键捕获/新建/重命名/删除/导入导出/按应用前台检测 在 demo2 未实现（请求会记录到日志）

## demo2 完整功能（第三轮）

在 demo2 桥接版基础上补齐了原 C# 版本的全部核心功能：

| 功能 | 实现 |
| --- | --- |
| 配置管理 | 新建 / 重命名 / 删除 / 上移 / 下移 / 导入(.json 文件) / 导出(所选配置) —— 页面弹窗输入名称，宿主实时写回 profiles.json |
| 快捷键 | 每配置全局热键 + 全局开关快捷键：点击「设置快捷键…」后进入低级键盘钩子捕获（Esc 取消），RegisterHotKey 注册，收到热键时切换配置/总开关 |
| 按应用切换 | 添加/编辑/删除检测应用（进程名 + 窗口标题包含 + 绑定配置）、「用当前前台应用添加」（GetForegroundWindow 拾取）、后台线程每 500ms 轮询前台窗口命中绑定自动切换配置/自动关闭 |
| 开机自启 | 开关写入 HKCU Run 注册表（带 --autostart 参数） |
| 自启进托盘 | MinimizeToTray 标志；最小化/关闭隐藏到托盘，托盘菜单：显示主窗口/退出 |
| OBS 捕获 | 开关同步 FilterEngine.SetOverlayCapturable |
| 调试日志 | 按钮打开 webview2_demo2.log |
| 界面 | 修复「按应用切换」导航图标；配置列表点击选中、启用按钮 n选1；模态弹窗交互 |

### 开发测试钩子
宿主支持读取 exe 同目录 `demo2-test.json`（JSON 数组，每项一条页面消息）在启动 5 秒后按序执行，用于自动化验证消息处理（无此文件则不执行）。

## 目录整理（第四轮）

```
F:\code\HScreenFilter\
├─ build\                  # 主程序（C++ 原生版）构建输出
│   ├─ HScreenFilter.exe
│   └─ assets\icon.ico
├─ dist\demo2\             # demo2 独立运行包（整体可拷走）
│   ├─ webview2_demo2.exe
│   ├─ WebView2Loader.dll
│   └─ webui2\ (index.html / styles.css / app.js)
├─ previews\               # 界面设计效果图（flat_*.png）
└─ build-webview2-demo2.ps1 # 构建到 dist\demo2
```

- demo2 运行：`dist\demo2\webview2_demo2.exe`（日志/预览图写在 exe 同目录）
- 日志：`dist\demo2\webview2_demo2.log`

## v2.0.0-alpha 发布（第五轮）

输出目录：`dist\HScreenFilter-v2.0.0-alpha\`（exe + WebView2Loader.dll + webui2，整体可拷走运行）。

### 本轮修复/改动

1. 全局快捷键旁新增「清除」按钮
2. 修复「色相·全部（主）」无反应 —— hlsl.cpp 中 MasterHue 之前只旋转掩码选择帧、未叠加到输出色相；现输出色相也叠加 MasterHue（主程序与 demo2 同步修复）
3. 「用当前前台应用添加」改为 3 秒倒计时结束后再捕获前台进程
4. 配置列表「启用」按钮左移
5. 移除 HSL 页的 V-Sync 开关
6. 保存弹窗改为：当前数值与保存快照不一致（含 LUT 开关状态变化）时出现「配置发生改变，是否保存至「{配置名}」？」
7. 保存后同步：LUT 开关状态写入活动配置（UseDxgi），配置列表引擎列实时刷新
8. 程序加图标：demo2 内嵌 icon（窗口/托盘/文件图标）+ 版本资源 v2.0.0-alpha
9. 托盘右键菜单修复（Win10/11 右键消息为 WM_CONTEXTMENU）；菜单新增「启用滤镜」勾选项与全部配置列表；双击托盘恢复窗口

### 运行

```powershell
.uild-webview2-demo2.ps1                 # 构建到 distHScreenFilter-v2.0.0-alpha
distHScreenFilter-v2.0.0-alphawebview2_demo2.exe
```

## v2.0.0-alpha 更新（第六轮）

1. 饱和度数值框改为显示原值（0..200，默认 100），不再显示相对值 0
2. 窗口改为可缩放：最小 = 默认 680x860 DIP，可放大（WebView 内容随窗口自适应）
3. 保存提示改为两行：`配置发生改变，是否保存至\n{配置名}`
4. 移除「UI 可被 OBS 捕获」开关（沿用配置文件 Captureable 值）
5. 全局加入适量动画（页面淡入、弹窗缩放、保存条上滑、按钮/色系/滑块过渡、倒计时脉冲）
