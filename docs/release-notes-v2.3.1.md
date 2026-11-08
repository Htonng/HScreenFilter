# HScreenFilter v2.3.1

> Windows 屏幕滤镜 · 实时 3D LUT 调色覆盖层

## 重点修复：打开滤镜全黑屏

v2.3.0 在分层重构后出现两个独立黑屏根因，v2.3.1 已全部修复：

1. **工作纹理继承了 DXGI 捕获源的共享/键控互斥标志**
   - DXGI 桌面复制源带 `MiscFlags=0x2900`；
   - 工作纹理直接复制源 desc 后也变成共享键控互斥纹理，`CopyResource` 静默失败；
   - 现象：场景探测读源帧正常（平均亮度 0.88），工作纹理与输出全黑。
   - 修复：工作纹理固定 `Usage=DEFAULT / BindFlags=SHADER_RESOURCE / MiscFlags=0 / CPUAccessFlags=0`。

2. **`Captureable=true` 时覆盖层被自己的桌面复制捕获**
   - `WDA_MONITOR` 允许第三方录制，同时也让本进程的 DXGI 复制抓到覆盖层自身；
   - 首帧后形成「黑 → 捕获黑 → 输出更黑」的自反馈。
   - 修复：LUT 覆盖层固定 `WDA_EXCLUDEFROMCAPTURE`；旧配置/`--capturable` 自动忽略并记录日志。

> 取舍：第三方录制工具不再能看到 LUT 覆盖层。这是「用桌面复制做滤镜」与「覆盖层允许被捕获」不可共存的底层限制。

## 同时包含 v2.3.0 的架构改进

- **渲染三层拆分**：`DesktopCapture` / `ColorProcessor` / `OverlayPresenter` 只通过纹理、SRV/RTV、帧元数据通信。
- **单一后端**：LUT / 放大镜 / 伽马任何时刻只有一个在屏幕上生效，修复多层叠加/效果缺失；进入 LUT 前强制复位残留伽马/放大镜。
- **暗场景配置持久化**：`FilterSettings::Equals` 补上 `DarkSceneBoost`；基础参数统一由 `FilterParams` 元数据表驱动，保存/导出/导入/范围夹取一致。
- **参数表下发**：网页通过 `params` 数组生成滑块，新增基础参数不再需要改网页范围表。
- **新图标与 README**：重绘玻璃质感 H 图标，README 改为居中的项目介绍布局。

## 验证

```text
--selftest            退出码 0（JSON / 参数表 / 暗场景 Equals / HLSL / D3D11）
--enginetest=8        自检 输出 R=242 G=242 B=242 / 输入 R=255，画面正常
--enginetest-cycle    退出码 0，180 帧/秒，隐藏/恢复未阻塞
发布版（旧配置 Captureable=true）：
                      自检 输出/输入 R=243 G=245 B=249，画面正常
```

## 安装

1. 下载 `HScreenFilter-v2.3.1.zip` 并解压；
2. 确认 `HScreenFilter.exe`、`WebView2Loader.dll`、`webui2\` 在同一目录；
3. 运行 `HScreenFilter.exe`；首次使用需要 Microsoft Edge WebView2 Runtime。

完整变更见仓库根目录 `CHANGELOG.md`。
