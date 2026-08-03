using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
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
    private TrayIcon? _tray;
    private Profile? _capturingFor;
    private bool _capturingGlobal;
    private bool _closingToTray = true;
    private bool _shutdownStarted;
    private int _globalHotkeyId;

    public MainWindow()
    {
        InitializeComponent();
        Title = "屏幕滤镜";

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

        // 恢复已保存状态
        _data = _store.Load();
        foreach (var p in _data.Profiles)
            _profiles.Add(p);

        // 初始化滤镜引擎
        FilterEngine.Initialize();
        StatusText.Text = FilterEngine.Kind switch
        {
            FilterEngineKind.FullScreenColorEffect => "滤镜引擎：全屏颜色效果（支持全部调节项）",
            FilterEngineKind.GammaRamp => "滤镜引擎：显卡伽马曲线（鲜艳度不可用）",
            _ => "滤镜引擎不可用：" + FilterEngine.LastError,
        };
        if (FilterEngine.Kind == FilterEngineKind.GammaRamp)
        {
            SaturationSlider.IsEnabled = false;
            SaturationSlider.Header = "鲜艳度 Saturation（当前引擎不支持）";
        }

        // 滑块事件
        BrightnessSlider.ValueChangedExternal += (_, v) => { _data.Current.Brightness = v; ScheduleApply(); };
        ContrastSlider.ValueChangedExternal += (_, v) => { _data.Current.Contrast = v; ScheduleApply(); };
        SaturationSlider.ValueChangedExternal += (_, v) => { _data.Current.Saturation = v; ScheduleApply(); };
        HighlightSlider.ValueChangedExternal += (_, v) => { _data.Current.Highlights = v; ScheduleApply(); };
        ShadowSlider.ValueChangedExternal += (_, v) => { _data.Current.Shadows = v; ScheduleApply(); };
        TemperatureSlider.ValueChangedExternal += (_, v) => { _data.Current.Temperature = v; ScheduleApply(); };

        LoadSettingsIntoUi(_data.Current);

        ProfileList.ItemsSource = _profiles;
        if (_data.SelectedProfileIndex >= 0 && _data.SelectedProfileIndex < _profiles.Count)
            ProfileList.SelectedIndex = _data.SelectedProfileIndex;

        AutoStartSwitch.IsOn = _data.AutoStart;
        EnableSwitch.IsOn = _data.IsEnabled; // 触发 Toggled → 应用滤镜
        UpdateGlobalHotkeyBadge();

        // 全局热键 + 系统托盘（共用一个消息窗口）
        _msgWindow = MessageWindow.Create();
        _hotkeys = new HotkeyService(_msgWindow);
        foreach (var p in _profiles)
            RegisterProfileHotkey(p);
        RegisterGlobalToggle();

        _tray = new TrayIcon(_msgWindow, ShowMainWindow, OnTrayExit);
        _tray.Show();

        Closed += MainWindow_Closed;
        Activated += MainWindow_Activated;
    }

    private bool _sizeApplied;

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
            appWindow.Resize(new Windows.Graphics.SizeInt32(580, 860));

            if (appWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter p)
            {
                p.PreferredMinimumWidth = 580;
                p.PreferredMinimumHeight = 860;
            }
        }
        catch
        {
            // 忽略尺寸设置失败
        }
    }

    // ---------------- 滤镜应用 ----------------

    private void ScheduleApply()
    {
        _applyTimer.Stop();
        _applyTimer.Start();
        _saveTimer.Stop();
        _saveTimer.Start();
    }

    private void ApplyCurrent()
    {
        if (EnableSwitch.IsOn)
        {
            if (!FilterEngine.Apply(_data.Current))
                StatusText.Text = "应用滤镜失败：" + FilterEngine.LastError;
        }
        else
        {
            FilterEngine.Reset();
        }
    }

    private void SaveState()
    {
        _data.Current = _data.Current.Clone();
        _data.IsEnabled = EnableSwitch.IsOn;
        _data.Profiles = new List<Profile>(_profiles);
        _data.SelectedProfileIndex = ProfileList.SelectedIndex;
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

    private void UpdateProfile_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p) return;
        p.Settings = _data.Current.Clone();
        SaveState();
    }

    private void ApplyProfile_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p) return;
        ApplyProfile(p);
    }

    private void DeleteProfile_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p) return;
        if (_profileHotkeyIds.TryGetValue(p, out var id))
        {
            _hotkeys.Unregister(id);
            _profileHotkeyIds.Remove(p);
        }
        _profiles.Remove(p);
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

    private void ApplyProfile(Profile profile)
    {
        DispatcherQueue.TryEnqueue(() =>
        {
            _data.Current = profile.Settings.Clone();
            LoadSettingsIntoUi(_data.Current);
            ApplyCurrent();
            SaveState();
        });
    }

    // ---------------- 快捷键 ----------------

    private void CaptureHotkey_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileList.SelectedItem is not Profile p)
        {
            CaptureHint.Text = "请先选择一个配置";
            return;
        }
        _capturingFor = p;
        _capturingGlobal = false;
        CaptureHint.Text = "请按下要绑定的按键（可带也可不带 Ctrl/Alt/Shift/Win），Esc 取消…";
        RootGrid.Focus(FocusState.Programmatic);
    }

    private void CaptureGlobalHotkey_Click(object sender, RoutedEventArgs e)
    {
        _capturingFor = null;
        _capturingGlobal = true;
        CaptureHint.Text = "请按下全局开关按键（可带也可不带修饰键），Esc 取消…";
        RootGrid.Focus(FocusState.Programmatic);
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
            CaptureHint.Text = "已取消";
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
            CaptureHint.Text = $"全局开关快捷键已设置为 {display}";
            UpdateGlobalHotkeyBadge();
        }
        else if (_capturingFor is { } profile)
        {
            profile.HotkeyModifiers = mods;
            profile.HotkeyKey = key;
            profile.HotkeyDisplay = display;
            RegisterProfileHotkey(profile);
            _capturingFor = null;
            CaptureHint.Text = $"已为「{profile.Name}」设置快捷键 {display}";
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
        FilterEngine.Reset();
        FilterEngine.Shutdown();
        _hotkeys.Dispose();
        _tray?.Dispose();
        _msgWindow.Dispose();
        Application.Current.Exit();
    }
}
