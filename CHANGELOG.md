# Changelog

## v2.1.1 (2026-09-12)
- **两遍渲染合并为单遍（Low 帧 / 性能）**：旧实现第一遍做 LUT 写 `R16G16B16A16_FLOAT` 中间纹理、第二遍读该纹理做
  12-tap 后处理再写后缓冲，另有两次全屏 `ClearRenderTargetView`；1440p 每帧仅中间纹理的写/读/清屏就约 90MB 显存流量。
  现在合并为一遍：捕获纹理 →（可选）线性光邻域后处理 → LUT 一次采样 → 输出色彩空间 → 后缓冲，
  无中间纹理、无清屏、一次全屏 draw。新增 `PostActive` 快路径：锐化/降噪/边缘/清晰度/画质全为 0 时
  只做 1 次输入采样 + 1 次 LUT 采样，输出与旧两遍实现的恒等路径一致。
  语义变化：后处理由「LUT 之后再锐化」变为「锐化之后再套 LUT」（细节先于 LUT 生效，避免强 LUT 曲线放大锐化过冲）。
- **引擎自愈（滤镜中途失效 / 屏幕发暗画面冻住）**：`LutEngine::RenderLoop` 原先在 `DXGI_ERROR_ACCESS_LOST`
  （显示模式/分辨率/色彩空间变化、DSR、游戏进出独占全屏）后只重试一次捕获，失败即退出渲染线程 ——
  而覆盖层窗口仍留在屏幕上冻结最后一帧，且上层没有看门狗，只能等用户动 UI 才恢复。现在：
  - 捕获失效改为带退避重试（10×400ms），仍不可用则**隐藏覆盖层**并继续等待，恢复后自动重新显示并继续渲染；
  - 色彩空间变化时**原地重建交换链**（格式 + `SetColorSpace1`）而不是停止引擎；
  - 新增 `FilterEngine::Watchdog()`：每 5 秒检查一次，LUT 引擎线程已结束则按最后一次参数自动重建；
    伽马曲线被系统/驱动重置时用 `GammaEngine::ApplyIfChanged` 自动重设（模式切换会冲掉 gamma ramp）。
  - 界面引擎状态会提示「覆盖层暂停：独占全屏或显示模式切换中」，不再让用户以为滤镜坏了。
- **修复渲染线程与 UI 线程死锁（本轮引入并当场修掉）**：覆盖层窗口属于 UI 线程，渲染线程对它的
  `ShowWindow`/`SetWindowPos` 是同步跨线程调用；UI 线程一旦阻塞在 `join()`/模态循环，两边互等。
  改为 `ShowWindowAsync` + `SWP_ASYNCWINDOWPOS`，并在显示后做有界等待。
- **Present 护栏（防历史卡死复发）**：flip 模型交换链向「未显示的窗口」`Present` 会永久阻塞
  （v2.0.0-beta 卡死根因）。现在 `DrawAndPresent` 在覆盖层不可见时直接跳过呈现（每次隐藏只记一条日志）。
  已用 `--enginetest-cycle` 回归：隐藏 3 秒期间 283 次呈现被正确拦下、`Present` 进出完全配平、恢复后继续呈现。
- **托盘左键双击可靠弹出主窗口**：原先只调 `SW_SHOW` —— 对最小化的窗口不会还原，后台进程
  `SetForegroundWindow` 又常被前台锁拒绝，表现为「双击没反应」。现在按 `IsIconic` 走 `SW_RESTORE`、
  置顶抖动 + 闪任务栏兜底；并处理 `TaskbarCreated`（explorer 重启后托盘图标被系统清掉时自动重新添加，
  否则此后点托盘永远无响应）。
- **显示模式变化处理**：主窗口处理 `WM_DISPLAYCHANGE`，重新枚举显示器并重新应用（覆盖层几何跟随新分辨率）。
- **HDR 渲染自检补全**：此前 HDR 输出模式直接跳过自检（等于 HDR 下没有黑屏检查），现在
  SDR(BGRA8)/HDR10(R10G10B10A2)/scRGB(RGBA16F) 三种后缓冲格式都做非黑校验。
- **诊断增强**：启动日志加入构建时间戳；`HSF_TRACE=1` 输出渲染循环阶段打点（定位卡在捕获/Present/恢复）；
  `--capturable` / `--no-capturable` 临时覆盖「允许捕获」便于排查 NVIDIA 录制冲突（不写回配置）；
  引擎启动时记录覆盖层捕获亲和性（WDA）状态。`--enginetest=秒数` 可延长引擎测试时间，
  `--enginetest-cycle` 增加覆盖层隐藏/恢复回归。
- 说明：版本号升级至 v2.1.1（`src\version.h` 为单一来源，构建时间见启动日志）。
  改动前的两遍渲染版本保留在 `dist\HScreenFilter-v2.1.0\HScreenFilter-旧版两遍渲染-对比用.exe`，供性能 A/B 对比。

## 未发布
- **按应用切换配置引擎不生效**：命中绑定的配置时只应用了该配置的滤镜参数，未同步切换其 UseDxgi 引擎模式；
  手动当前为放大镜、绑定配置为 LUT 时，自动切换后 LUT 引擎不会启用、滤镜失效。
  `ApplyCurrent` 现在按命中配置的 UseDxgi 切换引擎层（不改动保存的 `data_.UseDxgi`），未命中时恢复手动模式。
- **HDR 显示器下 LUT 引擎全屏闪烁/过亮**：改用与输出色彩空间匹配的交换链（HDR10=R10G10B10A2+PQ、scRGB=R16G16B16A16_FLOAT）并 `SetColorSpace1` 声明；
  输入模式按捕获纹理格式判定、输出模式按交换链色彩空间判定，二者独立，LUT 前后做 PQ/scRGB↔sRGB 转换并带 SDR 白点缩放；运行期色彩空间变化时下次应用重建。

## v2.0.0 (2026-08-20)
- **代码审查修复（P1/P2/P3）**：FilterEngine 新建 LUT 引擎时套用 V-Sync 与「可被 OBS 捕获」亲和性；取消保存时回滚 V-Sync；
  中性直通改为全屏三角形 + 直通着色器（兼容 HDR/格式变化）；profiles.json 原子写入；构建脚本用 Start-Process 取真实退出码；
  开发钩子以 HSF_DEBUG 门控；伽马引擎饱和度告警去重。
- **命名正式化**：发布版产物由 `webview2_demo2.exe` 更名为 `HScreenFilter.exe`；源码 `webview2_demo2.cpp`→`app_main.cpp`、
  `demo2.rc`→`release.rc`、构建脚本 `build-webview2-demo2.ps1`→`build-release.ps1`；日志 `webview2_demo2.log`→`HScreenFilter.log`；
  窗口类/托盘提示/MessageBox/预览图等统一去掉 demo/WebView2 字样。
- **便捷版号标识**：`version.h` 作为单一版本号来源（当前 `v2.0.0`），窗口标题/托盘提示/日志头统一显示
  `HScreenFilter v2.0.0`，升级版本只改这一处。
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
