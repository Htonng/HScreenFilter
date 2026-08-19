# Changelog

## v2.0.0-beta（发布预览版）
- **命名正式化**：发布版产物由 `webview2_demo2.exe` 更名为 `HScreenFilter.exe`；源码 `webview2_demo2.cpp`→`app_main.cpp`、
  `demo2.rc`→`release.rc`、构建脚本 `build-webview2-demo2.ps1`→`build-release.ps1`；日志 `webview2_demo2.log`→`HScreenFilter.log`；
  窗口类/托盘提示/MessageBox/预览图等统一去掉 demo/WebView2 字样。
- **便捷版号标识**：`common.h` 新增 `kVersionString`（当前 `v2.0.0-beta`），窗口标题/托盘提示/日志头统一显示
  `HScreenFilter v2.0.0-beta`，升级版本只改一处。
- **文案清理**：去掉滤镜引擎状态/引擎开关/HSL 提示等处的括号解释（如「LUT 逐像素引擎（3D LUT，支持 HSL 调色）」→「LUT 逐像素引擎」），
  功能板块标题「HSL 调色盘（3D LUT）」→「HSL 调色盘」。
- **资源版本信息中文乱码修复**：rc 文件为 UTF-8，windres 显式 `--codepage=65001` 编译（此前中文在文件属性里乱码）。

## 未发布（性能热修）
- **按应用切换失效（根因一：绑定无法添加）**：`openBindingModal` 的回调引用了不存在的 `onOk` 参数，
  点击「添加」时抛 `ReferenceError`（被事件处理器吞掉），弹窗关闭但消息从未发送 → 列表永远为空
  （历史日志中 `bindings=0` 贯穿始终）。改为弹窗确认时直接发送 `binding add/edit` 消息。
- **按应用切换失效（根因二：进程名匹配）**：`ForegroundAppWatcher` 用严格字符串相等比较进程名，而 `ProcessNameOfPid` 返回
  不带 `.exe` 的进程名（如 chrome）、UI 却提示输入 `chrome.exe`，两者永远匹配不上 → 命中恒为 -1 → 滤镜
  在按应用模式下总是自动关闭。改为规范化比较（小写、去 `.exe`、去空白）；`UpdateWatcher` 在 `SetTargets`
  后同步取最新命中值，避免添加绑定时先按旧值关滤镜、再等异步回调开启的“先关后开”。
  另：绑定弹窗补上「按当前设置」选项（对应 `ProfileIndex = -1`，宿主端早已支持）。
- **4060 LUT 卡顿（根因）**：flip 模型交换链的后缓冲每帧在 2 个缓冲间轮转，单槽 RTV 缓存因此永远命中不了，
  实际上仍每帧重建 `ID3D11RenderTargetView`（此前热修只缓存了单槽）。改为按缓冲身份缓存 RTV（多槽），
  只有分辨率/交换链重建时才真正重建 RTV。
- **AI 补帧果冻 / 启用 LUT 引擎 Low 帧（根因）**：移除「非垂直同步时按显示器刷新率用 QPC 丢帧」的软件节流。
  该节流会以与真实 vblank 错相位的节奏丢帧，覆盖层与桌面源帧错开 → 果冻、抖动与低帧。
  现改为每捕获到一帧立即呈现一次，让覆盖层与桌面源帧 1:1 锁步；flip 模型交换链的 2 缓冲队列会在
  DWM 合成边界自然限速，无需软件节流。垂直同步（`Present(1)`）仍作为显式开关保留。
- **WebView2 版**：补上垂直同步（V-Sync）开关（原生版已有，桥接版此前缺 UI）；切换后纳入脏状态判定并可保存。

## v2.0.0-beta（公测热修）
- 版本号由 v2.0.0-alpha 提升为 v2.0.0-beta；发布包输出至 `dist\HScreenFilter-v2.0.0-beta`（含 zip）。
- **修复覆盖层 Present 永久阻塞（公测卡死根因）**：flip 模型交换链首次 Present 到「从未显示过的隐藏窗口」会永久卡住，
  导致引擎启动后渲染线程死在 Present、Dispose 时 join 永远不返回。覆盖层改为创建时即 WS_VISIBLE，
  不再在运行期隐藏窗口（中性参数仍照常直通拷贝+呈现，避免内容卡在旧帧）。
- **LUT 引擎卡顿（4060 等）**：不再每帧创建 RenderTargetView（改为缓存，缓冲身份变化时才重建）；
  移除每 30 帧一次的整屏 GPU 读回自检（只保留启动后一次）；中性参数时隐藏覆盖层并跳过捕获拷贝/Present。
- **A 卡 + AI 补帧果冻/掉帧**：非垂直同步时也按显示器刷新率对 Present 限速，避免与 AFMF/高刷竞争产生撕裂与果冻。
- **调整参数后花屏**：分层窗口显式初始化 alpha（SetLayeredWindowAttributes）；
  分辨率/格式（含 HDR）变化时重建帧纹理与交换链，避免 CopyResource 失败；缓存 RTV 避免驱动层异常。
- **笔记本鲜艳度失效**：按显示器坐标（而非索引）匹配 DXGI 输出，并优先在真正驱动该显示器的适配器上创建设备，
  修复混合显卡（Optimus/双卡）下 DuplicateOutput 失败导致回退到伽马引擎（伽马引擎本身不支持鲜艳度）；
  DuplicateOutput 瞬时失败增加重试；回退伽马时记录日志说明鲜艳度不可用。
- 修复「UI 可被 OBS 捕获」开关只改标志位、未立即应用 WDA 的问题。
- 静态画面下调滑块也能立即生效（超时无新帧时用最近一帧补一次重绘）。

## v2.0.0-alpha (2026-08-16)
- WebView2 桥接版 UI（Flat Design，webui2）
- 完整桥接：profiles.json 读写、FilterEngine 实时应用、按应用切换前台监听
- 配置管理：新建/重命名/删除/上移/下移/导入/导出、每配置全局快捷键
- 开机自启、托盘（启用滤镜/配置列表菜单）
- 修复：色相"全部（主）"无效果（MasterHue 叠加到输出色相）
- 窗口最小尺寸按系统缩放/工作区自适应

## C++ 原生版（legacy 分支保留）
- C++17 / Win32 原生控件导航式 UI
- 64³ 3D LUT 管线（cs_5_0 计算着色器 + 像素着色器）
- HSL 分色系调色、按应用切换、配置与快捷键
