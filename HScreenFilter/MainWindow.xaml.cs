using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.System;
using HScreenFilter.Controls;
using HScreenFilter.Models;
using HScreenFilter.Services;
using DispatcherQueueTimer = Microsoft.UI.Dispatching.DispatcherQueueTimer;

namespace HScreenFilter;

public sealed partial class MainWindow : Window
{
    private readonly ProfileStore _store = new();
    private ProfileData _data = new();
    private readonly ObservableCollection<Profile> _profiles = new();
    private HotkeyService _hotkeys = null!;
    private MessageWindow _msgWindow = null!;
    private readonly Dictionary<Profile, int> _profileHotkeyIds = new();
    private readonly DispatcherQueueTimer _applyTimer;
    private readonly DispatcherQueueTimer _saveTimer;
    private readonly DispatcherQueueTimer _hintTimer;
    private bool _uiInit;   // 避免恢复置顶/可捕获开关状态时触发 UI 事件
    private TrayIcon? _tray;
    private Profile? _capturingFor;
    private bool _capturingGlobal;
    private bool _closingToTray = true;
    private bool _shutdownStarted;
    private int _globalHotkeyId;
    private bool _pendingEdit;       // 配置数据已改变，保存条正在显示（等待用户保存/取消）
    private bool _profileToggleInit; // 避免恢复/互斥切换开关时触发 UI 事件
    private Popup? _saveBar;         // 底部“是否保存”条
    private Border? _saveBarBorder;  // 保存条主体（用于上浮动画）
    private FilterSettings _savedSnapshot = new(); // 最近一次已提交（可回滚）的配置快照
    private Profile? _dragProfile;   // 拖拽排序：当前被拖动的配置
    private static readonly JsonSerializerOptions _profileJson = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    /// <summary>当前处于激活状态（开关打开）的配置；没有则返回 null（此时为临时配置）。</summary>
    private Profile? ActiveProfile =>
        _data.ActiveProfileIndex >= 0 && _data.ActiveProfileIndex < _profiles.Count
            ? _profiles[_data.ActiveProfileIndex]
            : null;

    public MainWindow()
    {
        InitializeComponent();
        Title = "HScreenFilter";

        // 设置窗口与任务栏图标
        try
        {
            string iconPath = Path.Combine(AppContext.BaseDirectory, "assets", "icon.ico");
            if (File.Exists(iconPath))
                this.AppWindow.SetIcon(iconPath);
        }
        catch
        {
            // 图标设置失败不影响主流程
        }

        // Window 自身有实例属性 DispatcherQueue，会遮蔽类型名，这里用完全限定名。
        var dq = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
        _applyTimer = dq.CreateTimer();
        _applyTimer.Interval = TimeSpan.FromMilliseconds(80);
        _applyTimer.IsRepeating = false;
        _applyTimer.Tick += (_, _) => ApplyCurrent();

        _saveTimer = dq.CreateTimer();
        _saveTimer.Interval = TimeSpan.FromMilliseconds(400);
        _saveTimer.IsRepeating = false;
        _saveTimer.Tick += (_, _) => SaveState();

        // 快捷键设置提示：10 秒后自动清空
        _hintTimer = dq.CreateTimer();
        _hintTimer.Interval = TimeSpan.FromSeconds(10);
        _hintTimer.IsRepeating = false;
        _hintTimer.Tick += (_, _) => { CaptureHint.Text = ""; GlobalCaptureHint.Text = ""; };

        // 恢复已保存状态
        _data = _store.Load();
        foreach (var p in _data.Profiles)
            _profiles.Add(p);

        // 窗口装饰：按主题设置背景（默认无毛玻璃兼容老设备；Mica 为毛玻璃）
        ApplyWindowTheme(_data.Theme);
        _themeInit = true;
        ThemeComboBox.SelectedIndex = _data.Theme == "mica" ? 1 : 0;
        _themeInit = false;

        // 可被 OBS 捕获开关（UI 恒置顶，避免关闭置顶后 UI 被滤镜覆盖层盖住看不见）
        _uiInit = true;
        CaptureSwitch.IsOn = _data.Captureable;
        _uiInit = false;

        // 需求 2：UI 置顶且不被 DXGI 滤镜捕获（UI 颜色不受滤镜影响）
        TryEnsureUiTopmost();

        // 恢复 DXGI 开关状态，再初始化滤镜引擎
        _dxgiInit = true;
        DxgiSwitch.IsOn = _data.UseDxgi; // 触发 Toggled → 设置引擎模式
        FilterEngine.UseDxgi = _data.UseDxgi;
        _dxgiInit = false;
        FilterEngine.Initialize();
        UpdateEngineStatus();
        RefreshHslEnabled();

        // 滑块事件
        BrightnessSlider.ValueChangedExternal += (_, v) => { _data.Current.Brightness = v; ScheduleApply(); };
        ContrastSlider.ValueChangedExternal += (_, v) => { _data.Current.Contrast = v; ScheduleApply(); };
        SaturationSlider.ValueChangedExternal += (_, v) => { _data.Current.Saturation = v; ScheduleApply(); };
        HighlightSlider.ValueChangedExternal += (_, v) => { _data.Current.Highlights = v; ScheduleApply(); };
        ShadowSlider.ValueChangedExternal += (_, v) => { _data.Current.Shadows = v; ScheduleApply(); };
        TemperatureSlider.ValueChangedExternal += (_, v) => { _data.Current.Temperature = v; ScheduleApply(); };
        // HSL 面板：色相 / 饱和度 / 明亮度 三个子区域的全部滑块
        foreach (var (slider, channel, field) in AllHslSliders())
            WireHslSlider(slider, channel, field);

        // 恢复上次激活的配置（开关 n 选 1）：有则加载其设置；无则用临时配置（下次启动恢复默认）
        if (_data.ActiveProfileIndex >= 0 && _data.ActiveProfileIndex < _profiles.Count)
        {
            _profileToggleInit = true;
            _profiles[_data.ActiveProfileIndex].IsActive = true;
            _profileToggleInit = false;
            _data.Current = _profiles[_data.ActiveProfileIndex].Settings.Clone();
        }
        else
        {
            _data.Current = new FilterSettings();
        }
        _savedSnapshot = _data.Current.Clone();

        LoadSettingsIntoUi(_data.Current);

        ProfileList.ItemsSource = _profiles;
        if (_data.SelectedProfileIndex >= 0 && _data.SelectedProfileIndex < _profiles.Count)
            ProfileList.SelectedIndex = _data.SelectedProfileIndex;

        AutoStartSwitch.IsOn = _data.AutoStart;
        EnableSwitch.IsOn = _data.IsEnabled; // 触发 Toggled → 应用滤镜
        UpdateGlobalHotkeyBadge();

        // 按应用切换滤镜（进程列表 + 配置绑定）
        _foregroundWatcher = new ForegroundAppWatcher();
        _foregroundWatcher.MatchChanged += ForegroundWatcher_MatchChanged;
        _perAppInit = true;
        PerAppSwitch.IsOn = _data.PerAppEnabled;
        _perAppInit = false;
        RefreshBindingList();
        UpdatePerAppStatus();
        if (_data.PerAppEnabled)
            StartPerAppWatching();

        // 全局热键 + 系统托盘（共用一个消息窗口）
        _msgWindow = MessageWindow.Create();
        _hotkeys = new HotkeyService(_msgWindow);
        foreach (var p in _profiles)
            RegisterProfileHotkey(p);
        RegisterGlobalToggle();

        _tray = new TrayIcon(_msgWindow, ShowMainWindow, OnTrayExit)
        {
            // 右键托盘菜单显示配置列表，当前生效（激活）的配置前打勾
            ProfilesProvider = () =>
            {
                var items = new (string Name, bool IsActive)[_profiles.Count];
                for (int i = 0; i < _profiles.Count; i++)
                    items[i] = (_profiles[i].Name, _profiles[i].IsActive);
                return items;
            },
            ProfileSelected = index =>
            {
                if (index >= 0 && index < _profiles.Count)
                    ApplyProfile(_profiles[index]);
            },
        };
        _tray.Show();

        Closed += MainWindow_Closed;
        Activated += MainWindow_Activated;
    }

    private bool _sizeApplied;
    private ForegroundAppWatcher _foregroundWatcher = null!;
    private AppBinding? _activeBinding;   // 按应用模式下，当前命中的绑定（null=未命中）
    private bool _perAppInit;             // 避免恢复状态时触发 UI 事件

    // 窗口激活后设置一次固定尺寸。参数顺序：(宽, 高)。
    // 仅在首次激活时设置一次，避免与 Windows 记录的窗口状态反复竞争。
    // 宽度约 460（默认观感），高度 860（保证开机自启区默认可见）。
    private void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        if (_sizeApplied) return;
        _sizeApplied = true;

        try
        {
            var appWindow = this.AppWindow;
            appWindow.Resize(new Windows.Graphics.SizeInt32(680, 860));

            if (appWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter p)
            {
                p.PreferredMinimumWidth = 540;
                p.PreferredMinimumHeight = 860;
            }

            // 内容延伸到标题栏后需指定可拖动区域（标题栏行）
            SetupTitleBarDragArea();
            TryEnsureUiTopmost();
        }
        catch
        {
            // 忽略尺寸设置失败
        }
    }

    /// <summary>设置标题栏拖动区域（ExtendsContentIntoTitleBar 后必须指定，否则窗口无法拖动）。</summary>
    private void SetupTitleBarDragArea()
    {
        try
        {
            var tb = this.AppWindow.TitleBar;
            if (!tb.ExtendsContentIntoTitleBar) return;
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            uint dpi = OverlayNative.GetDpiForWindow(hwnd);
            float scale = dpi / 96f;
            int titleH = (int)(32 * scale);        // 标题栏高（物理像素）
            int rightReserve = (int)(150 * scale); // 右侧留给系统按钮
            int w = (int)(680 * scale);
            tb.SetDragRectangles(new[]
            {
                new Windows.Graphics.RectInt32(0, 0, Math.Max(1, w - rightReserve), titleH),
            });
        }
        catch
        {
        }
    }

    /// <summary>对本进程所有可见 UI 顶层窗口执行操作（WinUI 3 有内外两层窗口，需都设置；
    /// 跳过滤镜覆盖层，保持其 WDA 排除防自反馈 + 置顶）。</summary>
    private static void ForEachOwnWindow(Action<IntPtr> action)
    {
        uint currentPid = (uint)Environment.ProcessId;
        OverlayNative.EnumWindows((h, l) =>
        {
            OverlayNative.GetWindowThreadProcessId(h, out uint pid);
            if (pid == currentPid && OverlayNative.IsWindowVisible(h) && !IsOverlayWindow(h))
            {
                try { action(h); } catch { }
            }
            return true;
        }, IntPtr.Zero);
    }

    /// <summary>是否为滤镜覆盖层窗口（不能被取消 WDA/去置顶，否则自反馈、滤镜层被盖住）。</summary>
    private static bool IsOverlayWindow(IntPtr h)
    {
        try
        {
            var sb = new System.Text.StringBuilder(128);
            OverlayNative.GetClassName(h, sb, sb.Capacity);
            return sb.ToString() == "HScreenFilterVorticeOverlay";
        }
        catch
        {
            return false;
        }
    }

    /// <summary>应用 UI 置顶 + 可捕获两个开关：
    ///   置顶开关（UiTopmostSwitch）：UI 是否置顶（盖在滤镜覆盖层之上，否则被覆盖层挡住看不见）；
    ///   可捕获开关（CaptureSwitch）：UI 与滤镜层是否可被 OBS 等屏幕捕获。
    ///     可捕获开 → UI 设 WDA_NONE（完全可被捕获，包括 DXGI 截屏/截图，不再防二次捕获），
    ///                覆盖层设 WDA_MONITOR（仅从 DXGI 自捕获排除 = 防自反馈，但 WGC/BitBlt 仍可见）；
    ///                这样 Win+Shift+S / 基于 DXGI 的截图、OBS（WGC/窗口捕获）都能录到 UI 与滤镜效果。
    ///     可捕获关 → UI 与覆盖层都 WDA_EXCLUDEFROMCAPTURE（从一切捕获排除，颜色不受滤镜影响）。</summary>
    private void TryEnsureUiTopmost()
    {
        try
        {
            // UI 恒置顶（强制）：覆盖层恒置顶，若 UI 不置顶会被滤镜层盖住看不见
            bool top = true;
            bool cap = _data.Captureable;

            // WinUI 原生置顶（比 Win32 样式可靠，不会被 WinUI 重置）
            try
            {
                if (this.AppWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter p)
                    p.IsAlwaysOnTop = top;
            }
            catch { }

            // UI 窗口（WinUI 3 有内外两层，需都设置；跳过滤镜覆盖层）
            // 可捕获开 → WDA_NONE：UI 完全可被捕获（含 DXGI），避免基于 DXGI 的截屏/截图看不到 UI。
            // 可捕获关 → WDA_EXCLUDEFROMCAPTURE：从一切捕获排除。
            ForEachOwnWindow(h =>
            {
                OverlayNative.SetWindowDisplayAffinity(h,
                    cap ? OverlayNative.WDA_NONE : OverlayNative.WDA_EXCLUDEFROMCAPTURE);
                // 分层/WinUI 窗口仅 SetWindowPos 置顶无效，需改 WS_EX_TOPMOST 样式位
                OverlayNative.SetTopmost(h, top);
            });

            // 滤镜覆盖层：可捕获→WDA_MONITOR（防自反馈）；否则→WDA_EXCLUDEFROMCAPTURE。恒置顶（覆盖层内部）。
            FilterEngine.SetOverlayCapturable(cap);

            AppLog.Write("UI", $"UI置顶={top} 可捕获={cap} → 覆盖层WDA={(cap ? "MONITOR(防自反馈+OBS可见)" : "EXCLUDEFROMCAPTURE")}");
        }
        catch
        {
        }
    }

    private void CaptureSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        if (_uiInit) return;
        _data.Captureable = CaptureSwitch.IsOn;
        TryEnsureUiTopmost();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    /// <summary>按主题设置窗口背景与标题栏装饰：default=无毛玻璃（兼容老设备）；mica=毛玻璃。</summary>
    private void ApplyWindowTheme(string theme)
    {
        try
        {
            var tb = this.AppWindow.TitleBar;
            tb.ExtendsContentIntoTitleBar = true;
            tb.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent;
            tb.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent;
            tb.ButtonHoverBackgroundColor = Microsoft.UI.ColorHelper.FromArgb(40, 255, 255, 255);
            tb.ButtonPressedBackgroundColor = Microsoft.UI.ColorHelper.FromArgb(60, 255, 255, 255);
            SystemBackdrop = theme == "mica" ? new MicaBackdrop() : null;
        }
        catch
        {
            // 主题切换失败不影响主流程
        }
    }

    private bool _themeInit;

    private void ThemeComboBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_themeInit) return;
        if (ThemeComboBox.SelectedItem is ComboBoxItem item)
        {
            var theme = (item.Tag as string) == "mica" ? "mica" : "default";
            _data.Theme = theme;
            ApplyWindowTheme(theme);
            _saveTimer.Stop();
            _saveTimer.Start();
        }
    }

    /// <summary>阻止鼠标滚轮切换界面主题（避免滚动页面时误触 ComboBox 换主题）。</summary>
    private void ThemeComboBox_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
    {
        e.Handled = true;
    }

    // ---------------- 滤镜应用 ----------------

    private void ScheduleApply()
    {
        MarkFilterChanged();
        _applyTimer.Stop();
        _applyTimer.Start();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    /// <summary>返回 HSL 全部中性的滤镜设置副本（保留基础调节项），用于放大镜引擎（HSL 不可用）。</summary>
    private static FilterSettings NeutralizeHsl(FilterSettings s)
    {
        var clone = s.Clone();
        clone.Hue = 0;
        clone.HslSaturation = 100;
        clone.Lightness = 0;
        foreach (var ch in clone.HslChannels)
        {
            ch.Hue = 0;
            ch.Saturation = 100;
            ch.Lightness = 0;
        }
        return clone;
    }

    private void ApplyCurrent()
    {
        // 实际是否应用滤镜：
        //  - 总开关（EnableSwitch）关闭 → 必关
        //  - 按应用模式开启且列表非空 → 需前台命中某绑定（_activeBinding != null），否则自动关闭
        bool perAppRequiresHit = _data.PerAppEnabled && _data.AppBindings.Count > 0;
        bool shouldApply = EnableSwitch.IsOn && (!perAppRequiresHit || _activeBinding != null);
        AppLog.Write("Apply",
            $"shouldApply={shouldApply} IsEnabled={EnableSwitch.IsOn} PerAppEnabled={_data.PerAppEnabled} Bindings={_data.AppBindings.Count} active={( _activeBinding != null)}");

        if (shouldApply)
        {
            FilterSettings settings;
            // 命中绑定且绑定了配置 → 自动应用该配置的滤镜设置
            if (perAppRequiresHit && _activeBinding != null && _activeBinding.ProfileIndex >= 0 &&
                _activeBinding.ProfileIndex < _profiles.Count)
            {
                settings = _profiles[_activeBinding.ProfileIndex].Settings;
            }
            // 关闭 DXGI（放大镜引擎）时 HSL 不可用 → 应用时忽略 HSL，避免近似模拟出分色效果
            else
            {
                settings = _data.UseDxgi ? _data.Current : NeutralizeHsl(_data.Current);
            }

            if (!FilterEngine.Apply(settings))
            {
                StatusText.Text = "应用滤镜失败：" + FilterEngine.LastError;
                UpdateEngineStatus(); // 可能已回退到其它引擎
            }
            else
            {
                // 滤镜覆盖层创建后，确保 UI 重新置顶在滤镜层之上
                TryEnsureUiTopmost();
            }
        }
        else
        {
            FilterEngine.Reset();
        }
    }

    /// <summary>刷新引擎状态与 HSL 提示文案。</summary>
    private void UpdateEngineStatus()
    {
        StatusText.Text = FilterEngine.Kind switch
        {
            FilterEngineKind.PixelShader => "滤镜引擎：DXGI 逐像素着色器（支持 HSL 调色）",
            FilterEngineKind.FullScreenColorEffect => "滤镜引擎：全屏颜色效果（放大镜 API，HSL 不可用）",
            FilterEngineKind.GammaRamp => "滤镜引擎：显卡伽马曲线（鲜艳度不可用）",
            _ => "滤镜引擎不可用：" + FilterEngine.LastError,
        };
        UpdateHslHint();
    }

    private void UpdateHslHint()
    {
        if (HslChannelHint == null) return;
        HslChannelHint.Text = FilterEngine.Kind == FilterEngineKind.PixelShader
            ? "已启用 DXGI 引擎：红/橙/黄/绿/青/蓝/紫/品红可分别精确调整、互不干扰（逐像素着色器）。"
            : "提示：关闭 DXGI 后使用放大镜引擎，暂不支持分色系 HSL。";
    }

    /// <summary>根据当前引擎是否可用 HSL 启用/禁用 HSL 面板与滑块。</summary>
    private void RefreshHslEnabled()
    {
        bool hslOk = FilterEngine.Kind == FilterEngineKind.PixelShader;
        HslPivot.IsEnabled = hslOk;
        foreach (var (slider, _, _) in AllHslSliders())
            slider.IsEnabled = hslOk;
    }

    // ---------------- DXGI 引擎开关 ----------------

    private bool _dxgiInit; // 避免恢复状态时触发 UI 事件

    private void DxgiSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        if (_dxgiInit) return;
        bool useDxgi = DxgiSwitch.IsOn;

        // 开启时提示性能代价
        if (useDxgi)
        {
            DxgiSwitch.IsOn = false; // 先回弹，等用户确认
            var dialog = new ContentDialog
            {
                Title = "启用 DXGI 引擎",
                Content = new TextBlock
                {
                    Text = "启用 DXGI 时 HSL 功能可用，但会造成性能损失，是否启用？",
                    TextWrapping = TextWrapping.Wrap,
                    Margin = new Thickness(0, 8, 0, 0),
                },
                PrimaryButtonText = "启用",
                CloseButtonText = "取消",
                DefaultButton = ContentDialogButton.Primary,
                XamlRoot = RootGrid.XamlRoot,
            };
            _ = ConfirmDxgiEnableAsync(dialog);
            return;
        }

        ApplyDxgiMode(false);
    }

    private async System.Threading.Tasks.Task ConfirmDxgiEnableAsync(ContentDialog dialog)
    {
        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.Primary)
        {
            _dxgiInit = true;
            DxgiSwitch.IsOn = true;
            _dxgiInit = false;
            ApplyDxgiMode(true);
        }
    }

    /// <summary>应用 DXGI 模式：设置引擎、刷新引擎状态与 HSL 可用性、应用滤镜并保存。</summary>
    private void ApplyDxgiMode(bool useDxgi)
    {
        _data.UseDxgi = useDxgi;
        FilterEngine.UseDxgi = useDxgi;
        FilterEngine.Initialize();
        UpdateEngineStatus();
        RefreshHslEnabled();
        ApplyCurrent();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    // ---------------- 按应用切换滤镜（进程列表 + 配置绑定） ----------------

    private void StartPerAppWatching()
    {
        _foregroundWatcher.SetTargets(_data.AppBindings);
        _foregroundWatcher.Start(500);
    }

    private void StopPerAppWatching()
    {
        _foregroundWatcher.Stop();
    }

    private void ForegroundWatcher_MatchChanged(AppBinding? hit, string process, string title)
    {
        // watcher 在后台线程回调，需切回 UI 线程
        DispatcherQueue.TryEnqueue(() =>
        {
            _activeBinding = hit;
            AppLog.Write("PerApp", hit == null
                ? $"未命中 (前台={process} 标题={title}) → 关闭滤镜"
                : $"命中 {hit.ProcessName} → 应用绑定配置{(hit.ProfileIndex >= 0 ? $" #{hit.ProfileIndex}" : "")}");
            UpdatePerAppStatus();
            ApplyCurrent();
            _saveTimer.Stop();
            _saveTimer.Start();
        });
    }

    private void UpdatePerAppStatus()
    {
        if (PerAppStatus == null) return;
        if (!_data.PerAppEnabled)
        {
            PerAppStatus.Text = "";
            return;
        }
        if (_data.AppBindings.Count == 0)
        {
            PerAppStatus.Text = "○ 尚未添加要检测的应用（请点击「添加应用…」）";
            return;
        }
        if (_activeBinding == null)
        {
            PerAppStatus.Text = $"○ 列表内无进程在前台，滤镜已自动关闭（共 {_data.AppBindings.Count} 个检测目标）";
            return;
        }
        string cfg = _activeBinding.ProfileIndex >= 0 && _activeBinding.ProfileIndex < _profiles.Count
            ? $"已自动应用配置「{_profiles[_activeBinding.ProfileIndex].Name}」"
            : "按当前设置应用";
        PerAppStatus.Text = $"● {_activeBinding.ProcessName} 在前台，滤镜已启用（{cfg}）";
    }

    private void PerAppSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        if (_perAppInit) return;
        _data.PerAppEnabled = PerAppSwitch.IsOn;
        if (_data.PerAppEnabled)
        {
            StartPerAppWatching();
        }
        else
        {
            StopPerAppWatching();
            _activeBinding = null;
        }
        UpdatePerAppStatus();
        ApplyCurrent();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    private void RefreshBindingList()
    {
        foreach (var b in _data.AppBindings)
            b.ProfileName = b.ProfileIndex >= 0 && b.ProfileIndex < _profiles.Count
                ? _profiles[b.ProfileIndex].Name : "";
        AppBindingList.ItemsSource = null;
        AppBindingList.ItemsSource = _data.AppBindings;
    }

    private async void AddBinding_Click(object sender, RoutedEventArgs e)
    {
        var binding = new AppBinding();
        if (await EditBindingDialogAsync(binding))
        {
            _data.AppBindings.Add(binding);
            RefreshBindingList();
            if (_data.PerAppEnabled) StartPerAppWatching();
            SaveState();
            UpdatePerAppStatus();
            ApplyCurrent();
        }
    }

    private async void EditBinding_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not AppBinding b) return;
        var copy = new AppBinding { ProcessName = b.ProcessName, WindowTitle = b.WindowTitle, ProfileIndex = b.ProfileIndex };
        if (await EditBindingDialogAsync(copy))
        {
            b.ProcessName = copy.ProcessName;
            b.WindowTitle = copy.WindowTitle;
            b.ProfileIndex = copy.ProfileIndex;
            RefreshBindingList();
            if (_data.PerAppEnabled) StartPerAppWatching();
            SaveState();
            UpdatePerAppStatus();
            ApplyCurrent();
        }
    }

    private void DeleteBinding_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is not AppBinding b) return;
        _data.AppBindings.Remove(b);
        if (ReferenceEquals(_activeBinding, b)) _activeBinding = null;
        RefreshBindingList();
        if (_data.PerAppEnabled) StartPerAppWatching();
        SaveState();
        UpdatePerAppStatus();
        ApplyCurrent();
    }

    /// <summary>弹出「添加/编辑检测应用」对话框：进程名（必填）+ 可选窗口标题 + 绑定配置。返回 true 表示确认并已写入 binding。</summary>
    private async Task<bool> EditBindingDialogAsync(AppBinding binding)
    {
        var procBox = new TextBox { PlaceholderText = "例如 notepad 或 chrome", Text = binding.ProcessName, MinWidth = 240 };
        var titleBox = new TextBox { PlaceholderText = "标题包含的文字（可选，留空匹配任意标题）", Text = binding.WindowTitle, MinWidth = 240 };
        var cfgCombo = new ComboBox { MinWidth = 240 };
        cfgCombo.Items.Add(new ComboBoxItem { Content = "（不切换配置，按当前设置）", Tag = -1 });
        for (int i = 0; i < _profiles.Count; i++)
            cfgCombo.Items.Add(new ComboBoxItem { Content = _profiles[i].Name, Tag = i });
        int sel = binding.ProfileIndex + 1;
        cfgCombo.SelectedIndex = sel >= 0 && sel < cfgCombo.Items.Count ? sel : 0;

        var panel = new StackPanel { Spacing = 12, Margin = new Thickness(0, 8, 0, 0) };
        panel.Children.Add(new TextBlock { Text = "进程名（不含 .exe）" });
        panel.Children.Add(procBox);
        panel.Children.Add(new TextBlock { Text = "窗口标题（可选）" });
        panel.Children.Add(titleBox);
        panel.Children.Add(new TextBlock { Text = "该进程在前台时自动应用的配置" });
        panel.Children.Add(cfgCombo);

        var dialog = new ContentDialog
        {
            Title = "添加检测应用",
            Content = panel,
            PrimaryButtonText = "确定",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = RootGrid.XamlRoot,
        };
        var result = await dialog.ShowAsync();
        if (result != ContentDialogResult.Primary) return false;
        if (string.IsNullOrWhiteSpace(procBox.Text)) return false; // 进程名必填
        binding.ProcessName = procBox.Text.Trim();
        binding.WindowTitle = titleBox.Text.Trim();
        if (cfgCombo.SelectedItem is ComboBoxItem item && item.Tag is int idx)
            binding.ProfileIndex = idx;
        return true;
    }

    private async void PickForegroundButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            // 倒计时：提示用户在 5 秒内切换到要添加的进程，随后捕获当时的前台窗口
            const int countdownSeconds = 5;
            var dq = Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread();
            var timer = dq.CreateTimer();
            timer.Interval = TimeSpan.FromSeconds(1);

            var countdownText = new TextBlock { FontSize = 36, HorizontalAlignment = HorizontalAlignment.Center, Text = countdownSeconds.ToString() };
            var hintText = new TextBlock
            {
                Text = "请在倒计时结束前切换到要添加的进程（当前窗口在前台即被捕获），倒计时结束后自动添加。",
                FontSize = 12,
                TextWrapping = TextWrapping.Wrap,
                Foreground = (Microsoft.UI.Xaml.Media.Brush)Microsoft.UI.Xaml.Application.Current.Resources["TextFillColorSecondaryBrush"],
            };
            var panel = new StackPanel { Spacing = 12 };
            panel.Children.Add(countdownText);
            panel.Children.Add(hintText);

            var dialog = new ContentDialog
            {
                Title = "从前台窗口添加",
                Content = panel,
                CloseButtonText = "取消",
                DefaultButton = ContentDialogButton.Close,
                XamlRoot = RootGrid.XamlRoot,
            };

            int remaining = countdownSeconds;
            timer.Tick += (_, _) =>
            {
                remaining--;
                countdownText.Text = remaining.ToString();
                if (remaining <= 0)
                {
                    timer.Stop();
                    dialog.Hide();
                }
            };
            timer.Start();
            dialog.Closed += (_, _) => timer.Stop();

            var result = await dialog.ShowAsync();
            // 用户点击“取消”（或未倒计时完就关闭）：当 result 为 None 且倒计时未结束 → 放弃添加
            if (result == ContentDialogResult.None && remaining > 0) return;
            // 倒计时结束（倒计时已归零）→ 继续添加
            if (remaining > 0) return;

            IntPtr hwnd = ForegroundAppWatcher.GetForegroundWindowForPicker();
            var (proc, title) = ForegroundAppWatcher.GetForegroundInfo(hwnd);
            if (string.IsNullOrEmpty(proc))
            {
                PerAppStatus.Text = "未能读取当前前台应用";
                return;
            }
            var binding = new AppBinding { ProcessName = proc, WindowTitle = title };
            if (await EditBindingDialogAsync(binding))
            {
                _data.AppBindings.Add(binding);
                RefreshBindingList();
                if (_data.PerAppEnabled) StartPerAppWatching();
                SaveState();
                UpdatePerAppStatus();
                ApplyCurrent();
            }
        }
        catch (Exception ex)
        {
            PerAppStatus.Text = "读取前台应用失败：" + ex.Message;
        }
    }

    private void SaveState()
    {
        _data.Current = _data.Current.Clone();
        _data.IsEnabled = EnableSwitch.IsOn;
        _data.ActiveProfileIndex = ActiveProfile is { } ap ? _profiles.IndexOf(ap) : -1;
        _data.Profiles = new List<Profile>(_profiles);
        _data.SelectedProfileIndex = ProfileList.SelectedIndex;
        _data.PerAppEnabled = PerAppSwitch?.IsOn ?? _data.PerAppEnabled;
        _data.Captureable = CaptureSwitch?.IsOn ?? _data.Captureable;
        _data.UseDxgi = DxgiSwitch?.IsOn ?? _data.UseDxgi;
        _store.Save(_data);
    }

    private void LoadSettingsIntoUi(FilterSettings s)
    {
        BrightnessSlider.SetValueSilently(s.Brightness);
        ContrastSlider.SetValueSilently(s.Contrast);
        SaturationSlider.SetValueSilently(s.Saturation);
        HighlightSlider.SetValueSilently(s.Highlights);
        ShadowSlider.SetValueSilently(s.Shadows);
        TemperatureSlider.SetValueSilently(s.Temperature);

        // 加载全部 HSL 滑块（色相/饱和度/明亮度 三个子区域）
        LoadHslSliders(s);
    }

    /// <summary>打开调试日志文件（用默认关联程序打开）。</summary>
    private void DebugLogButton_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var logPath = AppLog.FilePath;
            if (!File.Exists(logPath)) AppLog.Write("UI", "首次打开调试日志");
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(logPath)
            {
                UseShellExecute = true,
            });
        }
        catch (Exception ex)
        {
            AppLog.Write("UI", "打开日志失败: " + ex.Message);
            StatusText.Text = "打开日志失败：" + ex.Message;
        }
    }

    // ---------------- HSL 面板（色相/饱和度/明度 三子区域） ----------------

    private enum HslField { Hue, Saturation, Lightness }

    /// <summary>把滑块事件绑定到对应色系/字段。</summary>
    private void WireHslSlider(FilterSlider slider, string channelName, HslField field)
    {
        slider.ValueChangedExternal += (_, v) =>
        {
            SetHslValue(_data.Current, channelName, field, v);
            ScheduleApply();
            _saveTimer.Stop();
            _saveTimer.Start();
        };
    }

    /// <summary>把数值写入对应色系/字段（全部=主标量，其余写进 HslChannels）。</summary>
    private static void SetHslValue(FilterSettings s, string channelName, HslField field, double value)
    {
        if (channelName == HslChannelNames.Master)
        {
            if (field == HslField.Hue) s.Hue = value;
            else if (field == HslField.Saturation) s.HslSaturation = value;
            else s.Lightness = value;
        }
        else
        {
            var ch = s.HslChannels.FirstOrDefault(c => c.Name == channelName);
            if (ch == null)
            {
                ch = new HslChannel { Name = channelName };
                s.HslChannels.Add(ch);
            }
            if (field == HslField.Hue) ch.Hue = value;
            else if (field == HslField.Saturation) ch.Saturation = value;
            else ch.Lightness = value;
        }
    }

    /// <summary>读取对应色系/字段的当前值。</summary>
    private static double ChannelValue(FilterSettings s, string channelName, HslField field)
    {
        if (channelName == HslChannelNames.Master)
            return field switch
            {
                HslField.Hue => s.Hue,
                HslField.Saturation => s.HslSaturation,
                _ => s.Lightness,
            };
        var ch = s.HslChannels.FirstOrDefault(c => c.Name == channelName);
        if (ch == null) return field == HslField.Saturation ? 100.0 : 0.0;
        return field switch
        {
            HslField.Hue => ch.Hue,
            HslField.Saturation => ch.Saturation,
            _ => ch.Lightness,
        };
    }

    /// <summary>全部 27 个 HSL 滑块及其对应的色系/字段（8 色系 + 全部主）。</summary>
    private IEnumerable<(FilterSlider Slider, string Channel, HslField Field)> AllHslSliders()
    {
        yield return (HueMasterSlider, HslChannelNames.Master, HslField.Hue);
        yield return (HueRedSlider, HslChannelNames.Red, HslField.Hue);
        yield return (HueOrangeSlider, HslChannelNames.Orange, HslField.Hue);
        yield return (HueYellowSlider, HslChannelNames.Yellow, HslField.Hue);
        yield return (HueGreenSlider, HslChannelNames.Green, HslField.Hue);
        yield return (HueCyanSlider, HslChannelNames.Cyan, HslField.Hue);
        yield return (HueBlueSlider, HslChannelNames.Blue, HslField.Hue);
        yield return (HuePurpleSlider, HslChannelNames.Purple, HslField.Hue);
        yield return (HueMagentaSlider, HslChannelNames.Magenta, HslField.Hue);

        yield return (SatMasterSlider, HslChannelNames.Master, HslField.Saturation);
        yield return (SatRedSlider, HslChannelNames.Red, HslField.Saturation);
        yield return (SatOrangeSlider, HslChannelNames.Orange, HslField.Saturation);
        yield return (SatYellowSlider, HslChannelNames.Yellow, HslField.Saturation);
        yield return (SatGreenSlider, HslChannelNames.Green, HslField.Saturation);
        yield return (SatCyanSlider, HslChannelNames.Cyan, HslField.Saturation);
        yield return (SatBlueSlider, HslChannelNames.Blue, HslField.Saturation);
        yield return (SatPurpleSlider, HslChannelNames.Purple, HslField.Saturation);
        yield return (SatMagentaSlider, HslChannelNames.Magenta, HslField.Saturation);

        yield return (LightMasterSlider, HslChannelNames.Master, HslField.Lightness);
        yield return (LightRedSlider, HslChannelNames.Red, HslField.Lightness);
        yield return (LightOrangeSlider, HslChannelNames.Orange, HslField.Lightness);
        yield return (LightYellowSlider, HslChannelNames.Yellow, HslField.Lightness);
        yield return (LightGreenSlider, HslChannelNames.Green, HslField.Lightness);
        yield return (LightCyanSlider, HslChannelNames.Cyan, HslField.Lightness);
        yield return (LightBlueSlider, HslChannelNames.Blue, HslField.Lightness);
        yield return (LightPurpleSlider, HslChannelNames.Purple, HslField.Lightness);
        yield return (LightMagentaSlider, HslChannelNames.Magenta, HslField.Lightness);
    }

    /// <summary>加载所有 HSL 滑块的值（静默，不触发外部事件）。
    /// 按各滑块范围夹紧旧值，并写回模型，保证着色器实际应用的值与滑块显示一致。</summary>
    private void LoadHslSliders(FilterSettings s)
    {
        foreach (var (slider, channel, field) in AllHslSliders())
        {
            double v = Math.Clamp(ChannelValue(s, channel, field), slider.Minimum, slider.Maximum);
            SetHslValue(s, channel, field, v);
            slider.SetValueSilently(v);
        }
    }

    /// <summary>把所有 HSL 恢复为默认值（中性，不产生任何效果）。</summary>
    private void HslReset_Click(object sender, RoutedEventArgs e)
    {
        var s = _data.Current;
        s.Hue = 0;
        s.HslSaturation = 100;
        s.Lightness = 0;
        foreach (var ch in s.HslChannels)
        {
            ch.Hue = 0;
            ch.Saturation = 100;
            ch.Lightness = 0;
        }
        LoadHslSliders(s);
        ScheduleApply();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    // ---------------- 开关与预设 ----------------

    private void EnableSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        ApplyCurrent();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    private void AutoStartSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        _data.AutoStart = AutoStartSwitch.IsOn;
        AutoStart.Set(_data.AutoStart);
    }

    private void PresetStandard_Click(object sender, RoutedEventArgs e) => ApplyPreset(new FilterSettings());
    private void PresetEyeCare_Click(object sender, RoutedEventArgs e) => ApplyPreset(new FilterSettings { Brightness = -5, Contrast = 95, Saturation = 95, Temperature = 25 });
    private void PresetNight_Click(object sender, RoutedEventArgs e) => ApplyPreset(new FilterSettings { Brightness = -40, Contrast = 100, Saturation = 90, Temperature = 45 });
    private void PresetVivid_Click(object sender, RoutedEventArgs e) => ApplyPreset(new FilterSettings { Contrast = 110, Saturation = 150 });

    private void ApplyPreset(FilterSettings preset)
    {
        MarkFilterChanged();
        _data.Current = preset.Clone();
        LoadSettingsIntoUi(_data.Current);
        ApplyCurrent();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    // ---------------- 配置文件 ----------------

    private async void NewProfile_Click(object sender, RoutedEventArgs e)
    {
        var name = await PromptNameAsync("新建配置", "配置名称", $"配置 {_profiles.Count + 1}");
        if (string.IsNullOrEmpty(name)) return;

        var profile = new Profile { Name = name, Settings = _data.Current.Clone() };
        _profiles.Add(profile);
        ProfileList.SelectedItem = profile;
        SaveState();
    }

    private void DeleteProfile_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p) return;
        if (_profileHotkeyIds.TryGetValue(p, out var id))
        {
            _hotkeys.Unregister(id);
            _profileHotkeyIds.Remove(p);
        }
        bool wasActive = ReferenceEquals(ActiveProfile, p);
        int removedIndex = _profiles.IndexOf(p);
        _profiles.Remove(p);
        // 清理/修正按应用绑定中指向被删配置的引用
        foreach (var b in _data.AppBindings)
        {
            if (b.ProfileIndex == removedIndex) b.ProfileIndex = -1;
            else if (b.ProfileIndex > removedIndex) b.ProfileIndex--;
        }
        // 删除的是激活配置 → 回到临时配置（默认）；否则修正激活索引
        if (wasActive)
        {
            _profileToggleInit = true;
            foreach (var x in _profiles) x.IsActive = false;
            _profileToggleInit = false;
            _data.ActiveProfileIndex = -1;
            _data.Current = new FilterSettings();
            LoadSettingsIntoUi(_data.Current);
            ApplyCurrent();
        }
        else if (_data.ActiveProfileIndex > removedIndex)
        {
            _data.ActiveProfileIndex--;
        }
        RefreshBindingList();
        SaveState();
    }

    private void ProfileList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        // 捕捉快捷键期间不清空提示，避免绑定成功的提示被清掉
        if (_capturingFor == null && !_capturingGlobal)
            CaptureHint.Text = "";
    }

    private async Task<string?> PromptNameAsync(string title, string placeholder, string initial)
    {
        var box = new TextBox { PlaceholderText = placeholder, Text = initial };
        var dialog = new ContentDialog
        {
            Title = title,
            Content = box,
            PrimaryButtonText = "确定",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = RootGrid.XamlRoot,
        };
        var result = await dialog.ShowAsync();
        return result == ContentDialogResult.Primary ? box.Text.Trim() : null;
    }

    /// <summary>重命名所选配置。</summary>
    private async void RenameProfile_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p) return;
        var name = await PromptNameAsync("重命名配置", "配置名称", p.Name);
        if (string.IsNullOrEmpty(name)) return;
        p.Name = name;
        RefreshBindingList();
        SaveState();
    }

    /// <summary>导入配置：从 JSON 文件读取滤镜设置并新建一个配置。</summary>
    private async void ImportProfile_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
        picker.FileTypeFilter.Add(".json");
        InitFilePicker(picker);
        var file = await picker.PickSingleFileAsync();
        if (file == null) return;
        try
        {
            var json = await FileIO.ReadTextAsync(file);
            var settings = JsonSerializer.Deserialize<FilterSettings>(json, _profileJson);
            if (settings == null) throw new Exception("文件内容为空或格式不正确");
            var name = Path.GetFileNameWithoutExtension(file.Name);
            var profile = new Profile { Name = name, Settings = settings };
            _profiles.Add(profile);
            ProfileList.SelectedItem = profile;
            RefreshBindingList();
            SaveState();
            StatusText.Text = $"已导入配置「{name}」";
        }
        catch (Exception ex)
        {
            StatusText.Text = "导入配置失败：" + ex.Message;
        }
    }

    /// <summary>导出所选配置：把滤镜设置保存为 JSON 文件。</summary>
    private async void ExportProfile_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p) return;
        var picker = new FileSavePicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            SuggestedFileName = p.Name,
        };
        picker.FileTypeChoices.Add("HScreenFilter 配置", new List<string> { ".json" });
        InitFilePicker(picker);
        var file = await picker.PickSaveFileAsync();
        if (file == null) return;
        try
        {
            await FileIO.WriteTextAsync(file, JsonSerializer.Serialize(p.Settings, _profileJson));
            StatusText.Text = $"已导出配置「{p.Name}」";
        }
        catch (Exception ex)
        {
            StatusText.Text = "导出配置失败：" + ex.Message;
        }
    }

    private void InitFilePicker<T>(T picker) where T : class
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
    }

    // ---------------- 拖拽排序 ----------------

    private void ProfileList_DragItemsStarting(object sender, DragItemsStartingEventArgs e)
    {
        _dragProfile = e.Items.Count > 0 ? e.Items[0] as Profile : null;
        e.Data.RequestedOperation = DataPackageOperation.Move;
    }

    private void ProfileList_DragOver(object sender, DragEventArgs e)
    {
        e.AcceptedOperation = DataPackageOperation.Move;
        if (e.DragUIOverride != null)
        {
            e.DragUIOverride.Caption = "移动配置";
            e.DragUIOverride.IsCaptionVisible = true;
        }
    }

    private void ProfileList_Drop(object sender, DragEventArgs e)
    {
        if (_dragProfile == null) return;
        int oldIndex = _profiles.IndexOf(_dragProfile);
        if (oldIndex < 0) { _dragProfile = null; return; }

        // 找释放位置对应的目标索引（落在某行上半部则插到该行之前，否则末尾）
        var pos = e.GetPosition(ProfileList);
        int target = _profiles.Count;
        for (int i = 0; i < _profiles.Count; i++)
        {
            if (ProfileList.ContainerFromIndex(i) is not FrameworkElement c) continue;
            var topLeft = c.TransformToVisual(ProfileList).TransformPoint(new Windows.Foundation.Point(0, 0));
            if (pos.Y < topLeft.Y + c.ActualHeight / 2)
            {
                target = i;
                break;
            }
        }
        if (target == oldIndex) { _dragProfile = null; return; }

        // 记录每个按应用绑定当前指向的配置对象（重排后按对象重新映射索引）
        var bound = new Profile?[_data.AppBindings.Count];
        for (int i = 0; i < _data.AppBindings.Count; i++)
        {
            var b = _data.AppBindings[i];
            bound[i] = b.ProfileIndex >= 0 && b.ProfileIndex < _profiles.Count ? _profiles[b.ProfileIndex] : null;
        }

        _profiles.Move(oldIndex, target);

        // 重新映射按应用绑定索引
        for (int i = 0; i < _data.AppBindings.Count; i++)
        {
            var profile = bound[i];
            _data.AppBindings[i].ProfileIndex = profile == null ? -1 : _profiles.IndexOf(profile);
        }

        // 修正激活配置索引（按对象）
        var activeProfile = _profiles.FirstOrDefault(p => p.IsActive);
        _data.ActiveProfileIndex = activeProfile == null ? -1 : _profiles.IndexOf(activeProfile);

        RefreshBindingList();
        SaveState();
        _dragProfile = null;
    }

    private void ApplyProfile(Profile profile)
    {
        DispatcherQueue.TryEnqueue(() => SetActiveProfileAndApply(profile));
    }

    // ---------------- 配置开关（n 选 1）+ 自动保存 ----------------

    /// <summary>列表里配置开关的 Toggled：开=应用该配置，关=回到临时配置（默认）。</summary>
    private void ProfileActiveToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_profileToggleInit) return;
        if (sender is not ToggleSwitch toggle) return;
        if (toggle.DataContext is not Profile profile) return;
        if (toggle.IsOn) ActivateProfile(profile);
        else DeactivateProfile();
    }

    /// <summary>激活某配置：n 选 1（其它配置开关自动关闭），并应用其滤镜设置。
    /// 无论通过列表开关、快捷键还是外部调用，都统一走这里，保证开关显示与当前生效配置同步。</summary>
    private void SetActiveProfileAndApply(Profile profile)
    {
        _profileToggleInit = true;
        foreach (var p in _profiles)
            p.IsActive = ReferenceEquals(p, profile);
        _profileToggleInit = false;

        _data.ActiveProfileIndex = _profiles.IndexOf(profile);
        _data.Current = profile.Settings.Clone();
        _savedSnapshot = _data.Current.Clone();
        _pendingEdit = false;
        HideSaveBar();
        LoadSettingsIntoUi(_data.Current);
        ApplyCurrent();
        SaveState();
    }

    private void ActivateProfile(Profile profile) => SetActiveProfileAndApply(profile);

    /// <summary>关闭当前激活配置 → 无激活配置，改动进入临时配置（下次启动恢复默认）。</summary>
    private void DeactivateProfile()
    {
        _profileToggleInit = true;
        foreach (var p in _profiles)
            p.IsActive = false;
        _profileToggleInit = false;

        _data.ActiveProfileIndex = -1;
        _data.Current = new FilterSettings();
        _savedSnapshot = _data.Current.Clone();
        _pendingEdit = false;
        HideSaveBar();
        LoadSettingsIntoUi(_data.Current);
        ApplyCurrent();
        SaveState();
    }

    /// <summary>配置数据首次发生改变 → 弹出底部保存条（“配置发生改变，是否保存？”）。</summary>
    private void MarkFilterChanged()
    {
        if (_pendingEdit) return;
        _pendingEdit = true;
        ShowSaveBar();
    }

    /// <summary>从下方浮出条状保存浮窗：蓝色“保存”按钮 + 白色“取消”按钮。</summary>
    private void ShowSaveBar()
    {
        EnsureSaveBar();
        if (_saveBar == null) return;
        var size = RootGrid.XamlRoot.Size;
        _saveBar.HorizontalOffset = Math.Max(0, (size.Width - 440) / 2);
        _saveBar.VerticalOffset = Math.Max(0, size.Height - 52 - 24);
        _saveBar.IsOpen = true;

        // 从下方上浮动画
        var translate = new TranslateTransform { Y = 60 };
        if (_saveBarBorder != null)
            _saveBarBorder.RenderTransform = translate;
        var sb = new Storyboard();
        var anim = new DoubleAnimation
        {
            From = 60,
            To = 0,
            Duration = new Duration(TimeSpan.FromMilliseconds(200)),
            EnableDependentAnimation = true,
        };
        Storyboard.SetTarget(anim, translate);
        Storyboard.SetTargetProperty(anim, "Y");
        sb.Children.Add(anim);
        sb.Begin();
    }

    private void HideSaveBar()
    {
        if (_saveBar != null)
            _saveBar.IsOpen = false;
    }

    private void EnsureSaveBar()
    {
        if (_saveBar != null) return;

        var saveButton = new Button
        {
            Content = "保存",
            Padding = new Thickness(16, 6, 16, 6),
            Background = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(255, 0, 120, 212)),
            Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        };
        saveButton.Click += (_, _) => SaveBarSave_Click();

        var cancelButton = new Button
        {
            Content = "取消",
            Padding = new Thickness(16, 6, 16, 6),
            Background = new SolidColorBrush(Microsoft.UI.Colors.White),
            Foreground = new SolidColorBrush(Microsoft.UI.Colors.Black),
        };
        cancelButton.Click += (_, _) => SaveBarCancel_Click();

        var panel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 14,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        panel.Children.Add(new TextBlock
        {
            Text = "配置发生改变，是否保存？",
            FontSize = 13,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
        });
        panel.Children.Add(saveButton);
        panel.Children.Add(cancelButton);

        var border = new Border
        {
            Width = 440,
            Background = new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(240, 40, 40, 40)),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(16, 10, 16, 10),
        };
        border.Child = panel;
        _saveBarBorder = border;

        _saveBar = new Popup
        {
            Child = border,
            IsLightDismissEnabled = false,
            XamlRoot = RootGrid.XamlRoot,
        };
    }

    /// <summary>点击“保存”：把当前改动提交到激活配置（无激活则保留为当前配置），并持久化。</summary>
    private void SaveBarSave_Click()
    {
        if (ActiveProfile is { } ap)
            ap.Settings = _data.Current.Clone();
        _savedSnapshot = _data.Current.Clone();
        _pendingEdit = false;
        HideSaveBar();
        SaveState();
    }

    /// <summary>点击“取消”：回滚到最近一次已提交的配置，不保存。</summary>
    private void SaveBarCancel_Click()
    {
        _data.Current = _savedSnapshot.Clone();
        LoadSettingsIntoUi(_data.Current);
        ApplyCurrent();
        _pendingEdit = false;
        HideSaveBar();
        SaveState();
    }

    // ---------------- 快捷键 ----------------

    private void CaptureHotkey_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p)
        {
            ShowCaptureHint("请先选择一个配置", "");
            return;
        }
        _capturingFor = p;
        _capturingGlobal = false;
        ShowCaptureHint("请按下要绑定的按键（可带也可不带 Ctrl/Alt/Shift/Win），Esc 取消…", "");
        RootGrid.Focus(FocusState.Programmatic);
    }

    private void CaptureGlobalHotkey_Click(object sender, RoutedEventArgs e)
    {
        _capturingFor = null;
        _capturingGlobal = true;
        ShowCaptureHint("", "请按下全局开关按键（可带也可不带修饰键），Esc 取消…");
        RootGrid.Focus(FocusState.Programmatic);
    }

    /// <summary>显示快捷键设置提示，并安排在 10 秒后自动清空。</summary>
    private void ShowCaptureHint(string profileText, string globalText)
    {
        CaptureHint.Text = profileText;
        GlobalCaptureHint.Text = globalText;
        _hintTimer.Stop();
        _hintTimer.Start();
    }

    private void ClearHotkey_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p) return;
        p.HotkeyModifiers = 0;
        p.HotkeyKey = 0;
        p.HotkeyDisplay = "";
        RegisterProfileHotkey(p);
        SaveState();
    }

    private void ClearGlobalHotkey_Click(object sender, RoutedEventArgs e)
    {
        _data.GlobalModifiers = 0;
        _data.GlobalKey = 0;
        _data.GlobalDisplay = "";
        RegisterGlobalToggle();
        UpdateGlobalHotkeyBadge();
        SaveState();
    }

    /// <summary>把已绑定的全局开关快捷键显示在“启用滤镜”开关旁边（未绑定则隐藏）。</summary>
    private void UpdateGlobalHotkeyBadge()
    {
        if (GlobalHotkeyBadge == null) return;
        var display = _data.GlobalDisplay;
        if (string.IsNullOrEmpty(display))
        {
            GlobalHotkeyBadge.Visibility = Visibility.Collapsed;
        }
        else
        {
            GlobalHotkeyText.Text = display;
            GlobalHotkeyBadge.Visibility = Visibility.Visible;
        }
    }

    private void RootGrid_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (!_capturingGlobal && _capturingFor == null) return;

        if (e.Key == VirtualKey.Escape)
        {
            _capturingFor = null;
            _capturingGlobal = false;
            ShowCaptureHint("已取消", "已取消");
            return;
        }

        // 忽略纯修饰键，等待真正的主键
        if (e.Key is VirtualKey.Control or VirtualKey.Shift or VirtualKey.Menu
            or VirtualKey.LeftControl or VirtualKey.RightControl
            or VirtualKey.LeftShift or VirtualKey.RightShift
            or VirtualKey.LeftMenu or VirtualKey.RightMenu
            or VirtualKey.LeftWindows or VirtualKey.RightWindows)
            return;

        int mods = 0;
        if (IsKeyDown(0x11)) mods |= HotkeyText.MOD_CONTROL;
        if (IsKeyDown(0x10)) mods |= HotkeyText.MOD_SHIFT;
        if (IsKeyDown(0x12)) mods |= HotkeyText.MOD_ALT;
        if (IsKeyDown(0x5B) || IsKeyDown(0x5C)) mods |= HotkeyText.MOD_WIN;

        var key = (int)e.Key;
        var display = HotkeyText.Format(mods, key);

        if (_capturingGlobal)
        {
            _data.GlobalModifiers = mods;
            _data.GlobalKey = key;
            _data.GlobalDisplay = display;
            RegisterGlobalToggle();
            _capturingGlobal = false;
            ShowCaptureHint("", $"全局开关快捷键已设置为 {display}");
            UpdateGlobalHotkeyBadge();
        }
        else if (_capturingFor is { } profile)
        {
            profile.HotkeyModifiers = mods;
            profile.HotkeyKey = key;
            profile.HotkeyDisplay = display;
            RegisterProfileHotkey(profile);
            _capturingFor = null;
            ShowCaptureHint($"已为「{profile.Name}」设置快捷键 {display}", "");
        }

        e.Handled = true;
        SaveState();
    }

    private void RegisterProfileHotkey(Profile profile)
    {
        if (_profileHotkeyIds.TryGetValue(profile, out var oldId))
        {
            _hotkeys.Unregister(oldId);
            _profileHotkeyIds.Remove(profile);
        }
        if (!profile.HasHotkey) return;
        if (_hotkeys.Register(profile.HotkeyModifiers, profile.HotkeyKey, () => ApplyProfile(profile), out var id))
            _profileHotkeyIds[profile] = id;
    }

    private void RegisterGlobalToggle()
    {
        if (_globalHotkeyId != 0)
        {
            _hotkeys.Unregister(_globalHotkeyId);
            _globalHotkeyId = 0;
        }
        if (_data.GlobalKey == 0) return;
        if (_hotkeys.Register(_data.GlobalModifiers, _data.GlobalKey, ToggleGlobal, out var id))
            _globalHotkeyId = id;
    }

    private void ToggleGlobal()
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            EnableSwitch.IsOn = !EnableSwitch.IsOn;
        });
    }

    private static bool IsKeyDown(int vk) => (NativeMethods.GetAsyncKeyState(vk) & 0x8000) != 0;

    // ---------------- 窗口与托盘 ----------------

    private void MainWindow_Closed(object sender, WindowEventArgs args)
    {
        if (_closingToTray)
        {
            // 关闭按钮 → 隐藏到托盘继续运行（保持滤镜生效）
            args.Handled = true;
            this.AppWindow.Hide();
            _tray?.ShowBalloon("屏幕滤镜仍在运行", "已最小化到系统托盘，右键托盘图标可退出。");
        }
        else
        {
            ShutdownApp();
        }
    }

    private void ShowMainWindow()
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            this.AppWindow.Show();
            Activate();
        });
    }

    private void OnTrayExit()
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            _closingToTray = false;
            ShutdownApp();
        });
    }

    private void ShutdownApp()
    {
        if (_shutdownStarted) return;
        _shutdownStarted = true;

        SaveState();
        _foregroundWatcher?.Dispose();
        FilterEngine.Reset();
        FilterEngine.Shutdown();
        _hotkeys.Dispose();
        _tray?.Dispose();
        _msgWindow.Dispose();
        Application.Current.Exit();
    }
}
