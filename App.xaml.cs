using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;
using Hardcodet.Wpf.TaskbarNotification;
using WinCal.Core.Helpers;
using WinCal.Core.Services;
using WinCal.ViewModels;
using WinCal.Views;

namespace WinCal;

public partial class App : Application
{
    private TaskbarIcon? _trayIcon;
    private PopupWindow? _popup;
    private SettingsWindow? _settingsWindow;
    private SystemCalendarInterceptor? _interceptor;
    private ICalendarService? _calendarService;
    private DispatcherTimer? _popupReleaseTimer;
    private bool _isShuttingDown;

    private static readonly TimeSpan PopupRetention = TimeSpan.FromSeconds(90);

    public App()
    {
        // 日历弹窗以文字和轻量透明度动画为主。软件渲染可避免 Intel D3D 驱动
        // 为一个小型托盘面板长期保留大量进程级图形缓存。
        RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // 全局异常处理
        DispatcherUnhandledException += (s, args) =>
        {
            System.Diagnostics.Debug.WriteLine($"WinCal: Unhandled exception: {args.Exception}");
            args.Handled = true;
        };

        // 初始化托盘图标
        _trayIcon = (TaskbarIcon)FindResource("TrayIcon")!;

        // 等鼠标抬起后再弹出。若在 MouseDown 阶段激活窗口，任务栏处理随后到来的
        // MouseUp 时会重新夺回前台窗口，导致方向键仍然发送给任务栏。
        _trayIcon.TrayLeftMouseUp += (s, args) => TogglePopup();

        // 动态生成带今日日期数字的图标
        _trayIcon.Icon = TrayIconGenerator.Generate(DateTime.Today.Day);

        // 更新托盘提示文本
        _trayIcon.ToolTipText = $"miniCal - {DateTime.Now:yyyy年M月d日 dddd}";

        // 应用保存的主题设置
        var settings = AppSettings.Load();
        ThemeHelper.ApplyTheme(settings.ThemeMode);

        // 动态创建右键菜单
        var menu = new ContextMenu();

        var settingsItem = new MenuItem { Header = "设置" };
        settingsItem.Click += (s, args) => OpenSettings();
        menu.Items.Add(settingsItem);

        menu.Items.Add(new Separator());

        var exitItem = new MenuItem { Header = "退出" };
        exitItem.Click += (s, args) => Shutdown();
        menu.Items.Add(exitItem);

        _trayIcon.ContextMenu = menu;

        // 启动系统日历拦截器：点击任务栏时钟时替换为我们的面板
        try
        {
            _interceptor = new SystemCalendarInterceptor(Dispatcher);
            _interceptor.Start(ShowPopup);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"WinCal: Interceptor failed: {ex.Message}");
        }

        // 应用空闲后预热数据并创建尚未显示的弹窗，确保首次点击无需同步构造 XAML。
        // 真正昂贵的渲染资源会在首次 Show 时创建，隐藏超时后会关闭并回收。
        Dispatcher.BeginInvoke(
            new Action(WarmUpPopup),
            DispatcherPriority.ApplicationIdle);
    }

    /// <summary>
    /// 显示设置窗口（独立顶层窗口，单例模式）
    /// 可从右键菜单或齿轮按钮调用
    /// </summary>
    public static void ShowSettings()
    {
        try
        {
            var app = (App)Current;
            if (app._settingsWindow != null && app._settingsWindow.IsVisible)
            {
                // 已打开则激活
                app._settingsWindow.Activate();
                return;
            }

            app._settingsWindow = new SettingsWindow();
            app._settingsWindow.Closed += (_, _) =>
            {
                app._settingsWindow = null;
                app.ResetPopup();
            };
            app._settingsWindow.Show();
            app._settingsWindow.Activate();
        }
        catch (Exception ex)
        {
            var logPath = System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.Desktop), "wincal_error.log");
            System.IO.File.WriteAllText(logPath,
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}]\n{ex}\n\n--- InnerException ---\n{ex.InnerException}");
            MessageBox.Show($"错误已写入桌面 minical_error.log", "miniCal 错误",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void OpenSettings()
    {
        // 使用 Dispatcher 延迟打开，避免与右键菜单的弹出窗口冲突
        Dispatcher.BeginInvoke(new Action(() =>
        {
            System.Diagnostics.Debug.WriteLine("WinCal: OpenSettings dispatcher callback executing");
            ShowSettings();
        }));
    }

    /// <summary>
    /// 显示日历面板（不切换，始终显示）。用于拦截器回调。
    /// </summary>
    private void ShowPopup()
    {
        try
        {
            if (_popup != null && _popup.IsVisible)
            {
                // 已显示则只激活，不重新创建
                _popup.ActivateForKeyboardNavigation();
                return;
            }

            var popup = GetOrCreatePopup();
            popup.PrepareForShow();
            popup.Show();
            WindowPositionHelper.PositionNearTaskbar(popup, preferCursorMonitor: true);
            popup.ActivateForKeyboardNavigation();

            // 延迟启动焦点跟踪定时器，给窗口时间获取焦点
            Dispatcher.BeginInvoke(new Action(() =>
            {
                if (popup.IsVisible)
                    popup.StartFocusTracking();
            }), System.Windows.Threading.DispatcherPriority.Loaded);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"WinCal: ShowPopup error: {ex}");
            ReleasePopup();
        }
    }

    /// <summary>
    /// 切换日历面板显示/隐藏。用于托盘图标点击。
    /// </summary>
    private void TogglePopup()
    {
        try
        {
            if (_popup == null || !_popup.IsVisible)
            {
                var popup = GetOrCreatePopup();
                popup.PrepareForShow();
                popup.Show();
                WindowPositionHelper.PositionNearTaskbar(popup, preferCursorMonitor: true);
                popup.ActivateForKeyboardNavigation();
                popup.StartFocusTracking();
            }
            else
            {
                _popup.Hide();
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"WinCal: TogglePopup error: {ex}");
            ReleasePopup();
        }
    }

    private PopupWindow GetOrCreatePopup()
    {
        if (_popup != null)
            return _popup;

        var popup = new PopupWindow(GetOrCreateCalendarService());
        popup.IsVisibleChanged += OnPopupVisibilityChanged;
        popup.Closed += (_, _) =>
        {
            if (ReferenceEquals(_popup, popup))
            {
                StopPopupReleaseTimer();
                _popup = null;
            }
        };
        _popup = popup;
        return popup;
    }

    private ICalendarService GetOrCreateCalendarService() =>
        _calendarService ??= CalendarViewModel.CreateDefaultService();

    private void WarmUpPopup()
    {
        try
        {
            if (!_isShuttingDown)
                _ = GetOrCreatePopup();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"WinCal: Popup warm-up failed: {ex.Message}");
        }
    }

    private void OnPopupVisibilityChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (sender is not PopupWindow popup || !ReferenceEquals(_popup, popup))
            return;

        if (popup.IsVisible)
            StopPopupReleaseTimer();
        else
            SchedulePopupRelease();
    }

    private void SchedulePopupRelease()
    {
        StopPopupReleaseTimer();

        _popupReleaseTimer = new DispatcherTimer(DispatcherPriority.ApplicationIdle)
        {
            Interval = PopupRetention
        };
        _popupReleaseTimer.Tick += OnPopupReleaseTimerTick;
        _popupReleaseTimer.Start();
    }

    private void StopPopupReleaseTimer()
    {
        if (_popupReleaseTimer == null)
            return;

        _popupReleaseTimer.Stop();
        _popupReleaseTimer.Tick -= OnPopupReleaseTimerTick;
        _popupReleaseTimer = null;
    }

    private void OnPopupReleaseTimerTick(object? sender, EventArgs e)
    {
        StopPopupReleaseTimer();
        if (_popup is { IsVisible: false })
            ReleasePopup();
    }

    private void ReleasePopup(bool prepareReplacement = true)
    {
        StopPopupReleaseTimer();

        var popup = _popup;
        _popup = null;
        if (popup == null)
            return;

        popup.IsVisibleChanged -= OnPopupVisibilityChanged;
        popup.Close();

        // 弹窗已不可见，此处完成一次低频回收，让 WPF/COM 终结器及时释放
        // 日期格、文字排版和渲染表面关联的资源。
        Dispatcher.BeginInvoke(new Action(() =>
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();

            // 保留一个尚未显示的轻量弹窗，避免下次点击重新同步解析 XAML。
            if (prepareReplacement && !_isShuttingDown && _settingsWindow == null)
                WarmUpPopup();
        }), DispatcherPriority.ApplicationIdle);
    }

    private void ResetPopup()
    {
        ReleasePopup(prepareReplacement: false);
        if (_calendarService is IDisposable disposableService)
            disposableService.Dispose();
        _calendarService = null;

        Dispatcher.BeginInvoke(
            new Action(WarmUpPopup),
            DispatcherPriority.ApplicationIdle);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _isShuttingDown = true;
        StopPopupReleaseTimer();
        ReleasePopup(prepareReplacement: false);
        if (_calendarService is IDisposable disposableService)
            disposableService.Dispose();
        _calendarService = null;
        _interceptor?.Dispose();
        _trayIcon?.Dispose();
        base.OnExit(e);
    }
}
