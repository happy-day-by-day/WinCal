using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using WinCal.Core.Helpers;
using WinCal.Core.Models;
using WinCal.Core.Services;
using WinCal.ViewModels;
using WinCal.Views.Controls;

namespace WinCal.Views;

public partial class PopupWindow : Window
{
    private readonly CalendarViewModel _calendarViewModel;
    private readonly AppSettings _settings;
    private EventDetailWindow? _detailWindow;
    private bool _hasLoaded;
    private readonly LowLevelKeyboardProc _keyboardProc;
    private IntPtr _keyboardHook;
    private int _keyboardHookError;

    private const int WH_KEYBOARD_LL = 13;
    private const int WM_KEYDOWN = 0x0100;
    private const int WM_KEYUP = 0x0101;
    private const int WM_SYSKEYDOWN = 0x0104;
    private const int WM_SYSKEYUP = 0x0105;
    private const uint VK_LEFT = 0x25;
    private const uint VK_UP = 0x26;
    private const uint VK_RIGHT = 0x27;
    private const uint VK_DOWN = 0x28;
    private const int VK_SHIFT = 0x10;
    private const int VK_CONTROL = 0x11;
    private const int VK_MENU = 0x12;
    private const int VK_LWIN = 0x5B;
    private const int VK_RWIN = 0x5C;

    public PopupWindow() : this(CalendarViewModel.CreateDefaultService())
    {
    }

    internal PopupWindow(ICalendarService calendarService)
    {
        _keyboardProc = OnLowLevelKeyboardEvent;

        // 加载设置
        _settings = AppSettings.Load();

        // 初始化 ViewModel
        _calendarViewModel = new CalendarViewModel(calendarService);

        InitializeComponent();

        // 字号和窗口尺寸必须在首次 Show 之前应用；Loaded 阶段再改 Width
        // 会被 SizeToContent 的首轮布局覆盖。
        ApplyFontSizeOffset();

        // 设置 DataContext
        DataContext = _calendarViewModel;

        // 绑定事件列表
        EventListControl.ItemsSource = _calendarViewModel.UpcomingEvents;

        // 订阅事件列表变化以更新空状态提示
        _calendarViewModel.UpcomingEvents.CollectionChanged += (_, _) => UpdateNoEventsVisibility();

        Loaded += OnLoaded;

        // 窗口尺寸变化时重新定位（异步加载事件后窗口变高）
        SizeChanged += OnSizeChanged;

        // 在 PopupWindow 级别捕获鼠标事件，显示事件详情
        PreviewMouseMove += OnPreviewMouseMove;

        // 弹窗内全局键盘导航：左右切月份，上下切年份
        PreviewKeyDown += OnPreviewKeyDown;
        Activated += (_, _) => FocusKeyboardNavigation();
        IsVisibleChanged += OnIsVisibleChanged;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (!_hasLoaded)
        {
            ApplySettings();
            _hasLoaded = true;
        }
        UpdateNoEventsVisibility();
    }

    /// <summary>
    /// 窗口尺寸变化后重新定位，确保不超出屏幕底部
    /// </summary>
    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        WindowPositionHelper.PositionNearTaskbar(this);
    }

    /// <summary>
    /// 应用用户设置到界面
    /// </summary>
    private void ApplySettings()
    {
        // #1 主题
        ThemeHelper.ApplyTheme(_settings.ThemeMode);

        // #6 周起始日
        _calendarViewModel.WeekStartDay = _settings.WeekStartDay == Core.Services.WeekStartDay.Monday ? 1 : 0;
        Calendar.UpdateWeekHeaders(_calendarViewModel.WeekStartDay);
    }

    /// <summary>
    /// 重新显示常驻弹窗前，在保留当前界面的情况下后台刷新数据。
    /// 首次显示时构造阶段已经启动刷新，不重复发起请求。
    /// </summary>
    public void PrepareForShow()
    {
        if (!_hasLoaded)
            return;

        bool isCurrentMonth =
            _calendarViewModel.Year == DateTime.Today.Year &&
            _calendarViewModel.Month == DateTime.Today.Month;

        // 每次重新打开都回到今天；已经是本月时保留当前网格，避免不必要的闪烁。
        _calendarViewModel.NavigateToToday(preserveExistingGrid: isCurrentMonth);
    }

    public void FocusKeyboardNavigation()
    {
        if (!IsVisible)
            return;

        Dispatcher.BeginInvoke(new Action(() =>
        {
            RootBorder.Focus();
            Keyboard.Focus(RootBorder);
        }), DispatcherPriority.Input);
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (NavigateByKey(e.Key))
            e.Handled = true;
    }

    private bool NavigateByKey(Key key)
    {
        switch (key)
        {
            case Key.Left:
                _calendarViewModel.NavigatePreviousMonth();
                break;
            case Key.Right:
                _calendarViewModel.NavigateNextMonth();
                break;
            case Key.Up:
                _calendarViewModel.NavigatePreviousYear();
                break;
            case Key.Down:
                _calendarViewModel.NavigateNextYear();
                break;
            default:
                return false;
        }

        return true;
    }

    private void OnIsVisibleChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (IsVisible)
            InstallKeyboardHook();
        else
            UninstallKeyboardHook();
    }

    private void InstallKeyboardHook()
    {
        if (_keyboardHook != IntPtr.Zero)
            return;

        _keyboardHook = SetWindowsHookEx(
            WH_KEYBOARD_LL,
            _keyboardProc,
            GetModuleHandle(null),
            0);
        _keyboardHookError = _keyboardHook == IntPtr.Zero ? Marshal.GetLastWin32Error() : 0;
    }

    private void UninstallKeyboardHook()
    {
        if (_keyboardHook == IntPtr.Zero)
            return;

        UnhookWindowsHookEx(_keyboardHook);
        _keyboardHook = IntPtr.Zero;
    }

    private IntPtr OnLowLevelKeyboardEvent(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code >= 0 && IsVisible)
        {
            var message = wParam.ToInt32();
            var virtualKey = (uint)Marshal.ReadInt32(lParam);
            var isArrow = virtualKey is VK_LEFT or VK_RIGHT or VK_UP or VK_DOWN;
            var hasModifier = IsKeyDown(VK_SHIFT) || IsKeyDown(VK_CONTROL) ||
                              IsKeyDown(VK_MENU) || IsKeyDown(VK_LWIN) || IsKeyDown(VK_RWIN);

            if (isArrow && !hasModifier)
            {
                if (message is WM_KEYDOWN or WM_SYSKEYDOWN)
                {
                    Dispatcher.BeginInvoke(new Action(() =>
                    {
                        switch (virtualKey)
                        {
                            case VK_LEFT:
                                _calendarViewModel.NavigatePreviousMonth();
                                break;
                            case VK_RIGHT:
                                _calendarViewModel.NavigateNextMonth();
                                break;
                            case VK_UP:
                                _calendarViewModel.NavigatePreviousYear();
                                break;
                            case VK_DOWN:
                                _calendarViewModel.NavigateNextYear();
                                break;
                        }
                    }), DispatcherPriority.Input);
                }

                if (message is WM_KEYDOWN or WM_KEYUP or WM_SYSKEYDOWN or WM_SYSKEYUP)
                    return new IntPtr(1);
            }
        }

        return CallNextHookEx(_keyboardHook, code, wParam, lParam);
    }

    private static bool IsKeyDown(int virtualKey) =>
        (GetAsyncKeyState(virtualKey) & 0x8000) != 0;

    /// <summary>
    /// 应用字体大小偏移（基于基准字号 + offset）
    /// </summary>
    private void ApplyFontSizeOffset()
    {
        var offset = FontScaleHelper.ClampOffset(_settings.FontSizeOffset);
        var scale = FontScaleHelper.GetScale(offset);

        // 直接改变实际字号和行高，让 WPF 重新排版并保持文字清晰。
        // 字号使用半个 DIP 为步长，配合 Display 模式减少亚像素发虚。
        double Font(double baseSize) => Math.Round(baseSize * scale * 2) / 2;
        var layoutScale = scale <= 1 ? 1 : 1 + (scale - 1) * 0.65;
        double Layout(double baseSize) => Math.Round(baseSize * layoutScale);

        Resources["CalendarMonthTitleFontSize"] = Font(19);
        Resources["CalendarNavigationFontSize"] = Font(18);
        Resources["CalendarButtonFontSize"] = Font(11);
        Resources["CalendarWeekFontSize"] = Font(10);
        Resources["CalendarDayFontSize"] = Font(13);
        Resources["CalendarSmallFontSize"] = Font(8);
        Resources["CalendarSectionTitleFontSize"] = Font(12);
        Resources["CalendarSectionHintFontSize"] = Font(9);
        Resources["CalendarEndMarkerFontSize"] = Font(8.5);
        Resources["CalendarEventTitleFontSize"] = Font(11.5);
        Resources["CalendarEventMetaFontSize"] = Font(9.5);

        Resources["CalendarMonthHeaderHeight"] = Layout(52);
        Resources["CalendarWeekHeaderHeight"] = Layout(23);
        Resources["CalendarDayCellHeight"] = Layout(44);
        Resources["CalendarDayCircleSize"] = Layout(28);
        Resources["CalendarBadgeSize"] = Layout(12);
        Resources["CalendarNavigationButtonSize"] = Layout(30);
        Resources["CalendarEventItemHeight"] = Layout(42);
        Resources["CalendarEventListHeight"] = new GridLength(Layout(180));
        Resources["CalendarHeaderActionSize"] = Layout(28);
        Resources["CalendarSectionAccentHeight"] = Layout(26);

        RootBorder.LayoutTransform = Transform.Identity;

        // 大字号同步扩大窗口，但尺寸增长略缓于字号，兼顾阅读空间与屏幕占用。
        const double baseWidth = 366;
        const double baseMaxHeight = 650;
        Width = Math.Ceiling(baseWidth * layoutScale);
        MaxHeight = Math.Ceiling(baseMaxHeight * layoutScale);
    }

    // Win32 API 用于焦点检测
    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr SetActiveWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr hWnd);

    private delegate IntPtr LowLevelKeyboardProc(int code, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(
        int hookId, LowLevelKeyboardProc callback, IntPtr moduleHandle, uint threadId);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnhookWindowsHookEx(IntPtr hook);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(
        IntPtr hook, int code, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int virtualKey);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? moduleName);

    [DllImport("user32.dll")]
    private static extern IntPtr GetWindow(IntPtr hWnd, uint uCmd);

    private const uint GW_OWNER = 4;

    /// <summary>
    /// 将弹窗真正切到 Windows 前台，并把 WPF 键盘焦点放到日历容器。
    /// 仅调用 Keyboard.Focus 只能设置当前线程的逻辑焦点，任务栏仍是前台窗口时
    /// 方向键不会进入本窗口。
    /// </summary>
    public void ActivateForKeyboardNavigation()
    {
        if (!IsVisible)
            return;

        ActivateWindowAndFocusRoot();

        // Shell/任务栏的点击处理可能比托盘回调更晚结束；待当前输入消息完成后
        // 再确认一次前台窗口，避免焦点被任务栏夺回。
        Dispatcher.BeginInvoke(
            new Action(ActivateWindowAndFocusRoot),
            DispatcherPriority.ContextIdle);
    }

    private void ActivateWindowAndFocusRoot()
    {
        if (!IsVisible)
            return;

        var hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero)
            return;

        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        RootBorder.Focus();
        Keyboard.Focus(RootBorder);
    }

    // 焦点丢失检测
    private DispatcherTimer? _focusTimer;
    private IntPtr _initialForegroundWindow; // 弹出时的前台窗口

    /// <summary>
    /// 启动焦点丢失检测
    /// 策略：记录弹出时的前台窗口，只有当前台窗口变为其他窗口时才关闭
    /// </summary>
    public void StartFocusTracking()
    {
        StopFocusTracking();

        // 记录当前前台窗口（通常是桌面/任务栏/ShellExperienceHost）
        _initialForegroundWindow = GetForegroundWindow();

        _focusTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(200)
        };
        _focusTimer.Tick += OnFocusCheckTick;
        _focusTimer.Start();
    }

    private void StopFocusTracking()
    {
        if (_focusTimer != null)
        {
            _focusTimer.Stop();
            _focusTimer.Tick -= OnFocusCheckTick;
            _focusTimer = null;
        }
    }

    private void OnFocusCheckTick(object? sender, EventArgs e)
    {
        var foreground = GetForegroundWindow();
        var myHwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;

        // 前台是我们自己 → 正常，不做任何操作
        if (foreground == myHwnd)
            return;

        // 前台是我们的子窗口（EventDetail）→ 正常
        var owner = GetWindow(foreground, GW_OWNER);
        if (owner == myHwnd)
            return;

        // 前台仍然是原来的窗口（桌面/任务栏）→ 用户还没点击其他地方，保持打开
        if (foreground == _initialForegroundWindow)
            return;

        // 前台变成了一个新的窗口 → 用户切换了焦点，关闭弹窗
        CloseDetailWindow();
        Hide();
        StopFocusTracking();
    }

    /// <summary>
    /// 失焦自动关闭（同时关闭详情窗口）
    /// 作为定时器检测的补充
    /// </summary>
    protected override void OnDeactivated(EventArgs e)
    {
        base.OnDeactivated(e);
        CloseDetailWindow();
        Hide();
        StopFocusTracking();
    }

    /// <summary>
    /// 鼠标移动时检测是否悬停在 EventItem 上
    /// </summary>
    private void OnPreviewMouseMove(object sender, MouseEventArgs e)
    {
        var pos = e.GetPosition(this);
        var hitResult = VisualTreeHelper.HitTest(this, pos);
        if (hitResult == null)
        {
            CloseDetailWindow();
            return;
        }

        // 忽略齿轮按钮区域
        if (IsMouseOverSettingsButton(hitResult.VisualHit))
        {
            CloseDetailWindow();
            return;
        }

        var eventItem = FindAncestor<EventItem>(hitResult.VisualHit);
        if (eventItem?.EventData is CalendarEvent evt)
        {
            ShowDetailWindow(evt);
        }
        else
        {
            CloseDetailWindow();
        }
    }

    /// <summary>
    /// 判断鼠标是否在齿轮按钮上
    /// </summary>
    private bool IsMouseOverSettingsButton(DependencyObject visualHit)
    {
        var btn = FindAncestor<Button>(visualHit);
        return btn == SettingsButton;
    }

    /// <summary>
    /// 显示事件详情窗口（在主面板左侧）
    /// </summary>
    private void ShowDetailWindow(CalendarEvent evt)
    {
        if (_detailWindow == null)
        {
            _detailWindow = new EventDetailWindow();
            _detailWindow.ShowActivated = false; // 不抢焦点
        }

        _detailWindow.ShowEvent(evt, this);
    }

    /// <summary>
    /// 关闭详情窗口
    /// </summary>
    private void CloseDetailWindow()
    {
        if (_detailWindow != null)
        {
            _detailWindow.Hide();
        }
    }

    /// <summary>
    /// 隐藏时同时关闭详情
    /// </summary>
    protected override void OnClosed(EventArgs e)
    {
        StopFocusTracking();
        UninstallKeyboardHook();
        if (_detailWindow != null)
        {
            _detailWindow.Close();
            _detailWindow = null;
        }
        _calendarViewModel.Dispose();
        DataContext = null;
        base.OnClosed(e);
    }

    private static T? FindAncestor<T>(DependencyObject current) where T : DependencyObject
    {
        while (current != null)
        {
            if (current is T ancestor)
                return ancestor;
            current = VisualTreeHelper.GetParent(current);
        }
        return null;
    }

    /// <summary>
    /// 更新事件列表区域的可见性（有事件显示列表，无事件显示占位文案）
    /// </summary>
    private void UpdateNoEventsVisibility()
    {
        var hasEvents = _calendarViewModel.UpcomingEvents.Count > 0;
        EventScrollViewer.Visibility = hasEvents ? Visibility.Visible : Visibility.Collapsed;
        NoEventsText.Visibility = hasEvents ? Visibility.Collapsed : Visibility.Visible;
        EndMarker.Visibility = hasEvents ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>
    /// 默认 ScrollViewer 一次滚动多行，对紧凑事件卡片来说跳动过大。
    /// 改为与滚轮增量成比例的像素滚动，一格约移动半张卡片。
    /// </summary>
    private void OnEventListMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (sender is not ScrollViewer viewer || viewer.ScrollableHeight <= 0)
            return;

        const double pixelsPerNotch = 24;
        var delta = e.Delta / 120d * pixelsPerNotch;
        var targetOffset = Math.Clamp(
            viewer.VerticalOffset - delta,
            0,
            viewer.ScrollableHeight);

        viewer.ScrollToVerticalOffset(targetOffset);
        e.Handled = true;
    }

    /// <summary>
    /// 点击刷新按钮强制刷新日历数据
    /// </summary>
    private async void OnRefreshClick(object sender, RoutedEventArgs e)
    {
        RefreshButton.IsEnabled = false;
        try
        {
            await _calendarViewModel.ForceRefreshAsync();
            UpdateNoEventsVisibility();
        }
        finally
        {
            RefreshButton.IsEnabled = true;
        }
    }

    /// <summary>
    /// 点击齿轮图标打开设置窗口（模态）
    /// </summary>
    private void OnSettingsClick(object sender, RoutedEventArgs e)
    {
        CloseDetailWindow();
        Hide(); // 先关闭弹窗，再打开独立设置窗口
        App.ShowSettings();
    }
}
