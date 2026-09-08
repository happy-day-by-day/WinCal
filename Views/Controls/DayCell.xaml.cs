using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using WinCal.Core.Models;

namespace WinCal.Views.Controls;

public partial class DayCell : UserControl
{
    public DayCell()
    {
        InitializeComponent();
    }

    /// <summary>
    /// 绑定的日历日数据
    /// </summary>
    public static readonly DependencyProperty DayDataProperty =
        DependencyProperty.Register(
            nameof(DayData), typeof(CalendarDay), typeof(DayCell),
            new PropertyMetadata(null, OnDayDataChanged));

    public CalendarDay? DayData
    {
        get => (CalendarDay?)GetValue(DayDataProperty);
        set => SetValue(DayDataProperty, value);
    }

    /// <summary>
    /// 是否被选中
    /// </summary>
    public static readonly DependencyProperty IsSelectedProperty =
        DependencyProperty.Register(
            nameof(IsSelected), typeof(bool), typeof(DayCell),
            new PropertyMetadata(false, OnIsSelectedChanged));

    public bool IsSelected
    {
        get => (bool)GetValue(IsSelectedProperty);
        set => SetValue(IsSelectedProperty, value);
    }

    /// <summary>
    /// 日期被点击时触发的事件
    /// </summary>
    public event RoutedEventHandler? DayClicked;

    private static void OnDayDataChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var cell = (DayCell)d;
        if (e.NewValue is not CalendarDay day) return;

        // 日期数字
        cell.DayText.Text = day.Date.Day.ToString();

        // 今日高亮
        cell.TodayCircle.Visibility = day.IsToday ? Visibility.Visible : Visibility.Collapsed;
        cell.DayText.Foreground = day.IsToday
            ? FindResource(cell, "TextOnAccentBrush") as Brush
            : day.IsCurrentMonth
                ? FindResource(cell, "TextPrimaryBrush") as Brush
                : FindResource(cell, "TextDisabledBrush") as Brush;

        cell.DayText.FontWeight = day.IsToday ? FontWeights.Bold : FontWeights.Normal;

        // 节假日 / 补班角标
        cell.ScheduleLabelText.Text = day.ScheduleLabel;
        cell.ScheduleBadge.Visibility = day.ScheduleLabel is "休" or "班"
            ? Visibility.Visible
            : Visibility.Collapsed;
        cell.ScheduleBadge.Opacity = day.IsCurrentMonth ? 1 : 0.45;

        if (day.ScheduleLabel == "休")
        {
            cell.ScheduleBadge.Background = FindResource(cell, "RestDayBadgeBackgroundBrush") as Brush;
            cell.ScheduleLabelText.Foreground = FindResource(cell, "RestDayBadgeForegroundBrush") as Brush;
        }
        else if (day.ScheduleLabel == "班")
        {
            cell.ScheduleBadge.Background = FindResource(cell, "WorkdayBadgeBackgroundBrush") as Brush;
            cell.ScheduleLabelText.Foreground = FindResource(cell, "WorkdayBadgeForegroundBrush") as Brush;
        }

        // 农历
        cell.LunarText.Text = day.LunarDate;
        cell.LunarText.Visibility = string.IsNullOrEmpty(day.LunarDate)
            ? Visibility.Collapsed
            : Visibility.Visible;
        cell.LunarText.Opacity = day.IsCurrentMonth ? 0.9 : 0.42;

        // 事件圆点 - 最多3个，按日历去重显示不同颜色
        if (day.HasEvents && day.Events.Count > 0)
        {
            // 按颜色去重，最多3种颜色
            var distinctColors = day.Events
                .Select(e => e.Color)
                .Distinct()
                .Take(3)
                .ToList();

            cell.EventDots.Visibility = Visibility.Visible;
            cell.EventDots.Opacity = day.IsCurrentMonth ? 1 : 0.4;
            var dots = new[] { cell.Dot1, cell.Dot2, cell.Dot3 };
            for (int i = 0; i < dots.Length; i++)
            {
                if (i < distinctColors.Count)
                {
                    dots[i].Visibility = Visibility.Visible;
                    try
                    {
                        var brush = new SolidColorBrush(
                            (Color)ColorConverter.ConvertFromString(distinctColors[i]));
                        brush.Freeze();
                        dots[i].Fill = brush;
                    }
                    catch
                    {
                        dots[i].Fill = FindResource(cell, "EventDotBrush") as Brush;
                    }
                }
                else
                {
                    dots[i].Visibility = Visibility.Collapsed;
                }
            }
        }
        else
        {
            cell.EventDots.Visibility = Visibility.Collapsed;
            cell.Dot1.Visibility = Visibility.Collapsed;
            cell.Dot2.Visibility = Visibility.Collapsed;
            cell.Dot3.Visibility = Visibility.Collapsed;
        }
    }

    private static void OnIsSelectedChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var cell = (DayCell)d;
        bool isSelected = (bool)e.NewValue;

        // 选中状态覆盖整个日期格；今天仍保留日期数字后方的实心强调块。
        cell.AnimateSelectedOpacity(isSelected ? 1 : 0, isSelected ? 220 : 260);

        if (cell.DayData is { } day)
        {
            cell.DayText.Foreground = day.IsToday
                ? FindResource(cell, "TextOnAccentBrush") as Brush
                : isSelected
                ? FindResource(cell, "TodayAccentBrush") as Brush
                : day.IsCurrentMonth
                    ? FindResource(cell, "TextPrimaryBrush") as Brush
                    : FindResource(cell, "TextDisabledBrush") as Brush;
        }
    }

    private void OnDayClick(object sender, MouseButtonEventArgs e)
    {
        DayClicked?.Invoke(this, new RoutedEventArgs { Source = this });
    }

    private void OnMouseEnter(object sender, MouseEventArgs e)
    {
        AnimateHoverOpacity(0.78, 300);
    }

    private void OnMouseLeave(object sender, MouseEventArgs e)
    {
        AnimateHoverOpacity(0, 380);
    }

    private void AnimateHoverOpacity(double targetOpacity, int durationMilliseconds)
    {
        HoverSurface.BeginAnimation(
            OpacityProperty,
            new DoubleAnimation
            {
                To = targetOpacity,
                Duration = TimeSpan.FromMilliseconds(durationMilliseconds),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            },
            HandoffBehavior.SnapshotAndReplace);
    }

    private void AnimateSelectedOpacity(double targetOpacity, int durationMilliseconds)
    {
        SelectedSurface.BeginAnimation(
            OpacityProperty,
            new DoubleAnimation
            {
                To = targetOpacity,
                Duration = TimeSpan.FromMilliseconds(durationMilliseconds),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            },
            HandoffBehavior.SnapshotAndReplace);
    }

    private static object? FindResource(DependencyObject obj, string key)
    {
        return Application.Current.TryFindResource(key);
    }
}
