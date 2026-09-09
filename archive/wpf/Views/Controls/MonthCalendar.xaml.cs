using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using WinCal.ViewModels;

namespace WinCal.Views.Controls;

public partial class MonthCalendar : UserControl
{
    private const int WheelNavigationThreshold = 120;
    private static readonly TimeSpan WheelNavigationCooldown = TimeSpan.FromMilliseconds(160);

    private int _wheelDeltaAccumulator;
    private DateTime _lastWheelNavigationTime = DateTime.MinValue;

    public MonthCalendar()
    {
        InitializeComponent();

        PrevButton.Click += (s, e) =>
        {
            if (DataContext is CalendarViewModel vm)
                vm.NavigatePreviousMonth();
        };

        NextButton.Click += (s, e) =>
        {
            if (DataContext is CalendarViewModel vm)
                vm.NavigateNextMonth();
        };

        TodayButton.Click += (s, e) =>
        {
            if (DataContext is CalendarViewModel vm)
                vm.NavigateToToday();
        };

        Loaded += OnLoaded;
        DataContextChanged += OnDataContextChanged;

        // 使用 PreviewMouseLeftButtonUp 在 MonthCalendar 级别捕获点击
        // 这比在单个 DayCell 上绑定事件更可靠
        PreviewMouseLeftButtonUp += OnPreviewMouseLeftButtonUp;
        PreviewMouseWheel += OnPreviewMouseWheel;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (DataContext is CalendarViewModel vm)
        {
            CalendarGrid.ItemsSource = vm.CalendarDays;
            UpdateWeekHeaders(vm.WeekStartDay);
        }
    }

    /// <summary>
    /// 更新星期标题行（支持周一起始或周日起始）
    /// </summary>
    public void UpdateWeekHeaders(int weekStartDay)
    {
        // 周一: 一二三四五六日, 周日: 日一二三四五六
        string[] monStart = { "一", "二", "三", "四", "五", "六", "日" };
        string[] sunStart = { "日", "一", "二", "三", "四", "五", "六" };
        var headers = weekStartDay == 1 ? monStart : sunStart;

        var textBlocks = new[] { H0, H1, H2, H3, H4, H5, H6 };
        for (int i = 0; i < 7; i++)
        {
            textBlocks[i].Text = headers[i];
            textBlocks[i].Foreground = Application.Current.TryFindResource(
                headers[i] is "六" or "日" ? "WeekendTextBrush" : "TextSecondaryBrush") as Brush;
        }
    }

    private void OnDataContextChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (e.NewValue is CalendarViewModel vm)
        {
            CalendarGrid.ItemsSource = vm.CalendarDays;
        }
    }

    private void OnPreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        // 从点击位置向上查找 DayCell
        var hitResult = VisualTreeHelper.HitTest(this, e.GetPosition(this));
        if (hitResult == null) return;

        var dayCell = FindAncestor<DayCell>(hitResult.VisualHit);
        if (dayCell?.DayData is { } dayData && DataContext is CalendarViewModel vm)
        {
            // 清除所有选中状态
            ClearAllSelections();

            // 选中当前
            dayCell.IsSelected = true;

            // 更新 ViewModel
            vm.SelectDate(dayData.Date);
        }
    }

    private void OnPreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (DataContext is not CalendarViewModel vm)
            return;

        // 触控板可能连续发送不足一个滚轮刻度的小增量，先按方向累计。
        if (_wheelDeltaAccumulator != 0 && Math.Sign(_wheelDeltaAccumulator) != Math.Sign(e.Delta))
            _wheelDeltaAccumulator = 0;
        _wheelDeltaAccumulator += e.Delta;

        if (Math.Abs(_wheelDeltaAccumulator) < WheelNavigationThreshold)
        {
            e.Handled = true;
            return;
        }

        var now = DateTime.UtcNow;
        if (now - _lastWheelNavigationTime < WheelNavigationCooldown)
        {
            _wheelDeltaAccumulator = 0;
            e.Handled = true;
            return;
        }

        if (_wheelDeltaAccumulator > 0)
            vm.NavigatePreviousMonth();
        else
            vm.NavigateNextMonth();

        _wheelDeltaAccumulator = 0;
        _lastWheelNavigationTime = now;
        e.Handled = true;
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
    /// 清除所有 DayCell 的选中状态
    /// </summary>
    private void ClearAllSelections()
    {
        ClearAllSelectionsRecursive(CalendarGrid);
    }

    private void ClearAllSelectionsRecursive(DependencyObject parent)
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is DayCell dayCell)
            {
                dayCell.IsSelected = false;
            }
            else
            {
                ClearAllSelectionsRecursive(child);
            }
        }
    }
}
