using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace WinCal.Core.Helpers;

public enum TaskbarEdge
{
    Unknown,
    Left,
    Top,
    Right,
    Bottom
}

/// <summary>
/// 弹出窗口定位工具：使用 Win32 API 获取正确的显示器工作区，处理 DPI 缩放
/// </summary>
public static class WindowPositionHelper
{
    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left, Top, Right, Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X, Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint dwFlags);

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromPoint(POINT pt, uint dwFlags);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);

    /// <summary>
    /// 根据任务栏所在边，将窗口定位到对应的系统托盘附近，同时限制窗口不超出工作区。
    /// 必须在窗口 Show() 之后调用（需要窗口句柄和 DPI 信息）。
    /// </summary>
    public static void PositionNearTaskbar(Window window, bool preferCursorMonitor = false)
    {
        var source = PresentationSource.FromVisual(window);
        if (source?.CompositionTarget == null) return;

        // DPI 缩放因子：物理像素 → DIPs
        double dpiScaleX = source.CompositionTarget.TransformFromDevice.M11;
        double dpiScaleY = source.CompositionTarget.TransformFromDevice.M22;

        // 首次弹出时使用鼠标所在显示器；尺寸变化时继续使用窗口当前显示器，避免鼠标移动后跳屏。
        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero) return;

        IntPtr hMonitor;
        if (preferCursorMonitor && GetCursorPos(out var cursorPosition))
            hMonitor = MonitorFromPoint(cursorPosition, 2 /* MONITOR_DEFAULTTONEAREST */);
        else
            hMonitor = MonitorFromWindow(hwnd, 2 /* MONITOR_DEFAULTTONEAREST */);

        var monitorInfo = new MONITORINFO { cbSize = Marshal.SizeOf(typeof(MONITORINFO)) };

        Rect workArea;
        TaskbarEdge taskbarEdge;

        if (GetMonitorInfo(hMonitor, ref monitorInfo))
        {
            // Win32 返回的是物理像素，转为 DIPs
            workArea = new Rect(
                monitorInfo.rcWork.Left * dpiScaleX,
                monitorInfo.rcWork.Top * dpiScaleY,
                (monitorInfo.rcWork.Right - monitorInfo.rcWork.Left) * dpiScaleX,
                (monitorInfo.rcWork.Bottom - monitorInfo.rcWork.Top) * dpiScaleY);
            taskbarEdge = DetectTaskbarEdge(
                new Rect(
                    monitorInfo.rcMonitor.Left,
                    monitorInfo.rcMonitor.Top,
                    monitorInfo.rcMonitor.Right - monitorInfo.rcMonitor.Left,
                    monitorInfo.rcMonitor.Bottom - monitorInfo.rcMonitor.Top),
                new Rect(
                    monitorInfo.rcWork.Left,
                    monitorInfo.rcWork.Top,
                    monitorInfo.rcWork.Right - monitorInfo.rcWork.Left,
                    monitorInfo.rcWork.Bottom - monitorInfo.rcWork.Top));
        }
        else
        {
            // 回退
            workArea = SystemParameters.WorkArea;
            taskbarEdge = TaskbarEdge.Unknown;
        }

        // 可用高度（工作区顶部到底部）
        double availableHeight = workArea.Height;
        const double margin = 8;

        // 限制窗口最大高度不超过可用高度
        double maxHeight = availableHeight - margin;
        if (window.MaxHeight > 0 && window.MaxHeight < maxHeight)
        {
            // XAML 中设置的 MaxHeight 更小则尊重它
            maxHeight = window.MaxHeight;
        }
        window.MaxHeight = maxHeight;

        // 使用窗口实际渲染尺寸
        double windowWidth = window.ActualWidth;
        double windowHeight = window.ActualHeight;

        if (windowWidth <= 0 || windowHeight <= 0)
        {
            window.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            windowWidth = window.DesiredSize.Width;
            windowHeight = window.DesiredSize.Height;
        }

        // 确保不会超出最大高度
        windowHeight = Math.Min(windowHeight, maxHeight);

        var position = CalculatePopupPosition(
            taskbarEdge,
            workArea,
            new Size(windowWidth, windowHeight));

        window.Left = position.X;
        window.Top = position.Y;
    }

    /// <summary>
    /// 将事件详情窗口放在主面板有空间的一侧，并限制在主面板所在显示器的工作区内。
    /// </summary>
    public static void PositionDetailWindow(Window detailWindow, Window mainWindow)
    {
        if (!detailWindow.IsVisible) return;

        var source = PresentationSource.FromVisual(mainWindow);
        if (source?.CompositionTarget == null) return;

        var hwnd = new WindowInteropHelper(mainWindow).Handle;
        if (hwnd == IntPtr.Zero) return;

        double dpiScaleX = source.CompositionTarget.TransformFromDevice.M11;
        double dpiScaleY = source.CompositionTarget.TransformFromDevice.M22;
        var hMonitor = MonitorFromWindow(hwnd, 2 /* MONITOR_DEFAULTTONEAREST */);
        var monitorInfo = new MONITORINFO { cbSize = Marshal.SizeOf(typeof(MONITORINFO)) };

        Rect workArea;
        if (GetMonitorInfo(hMonitor, ref monitorInfo))
        {
            workArea = new Rect(
                monitorInfo.rcWork.Left * dpiScaleX,
                monitorInfo.rcWork.Top * dpiScaleY,
                (monitorInfo.rcWork.Right - monitorInfo.rcWork.Left) * dpiScaleX,
                (monitorInfo.rcWork.Bottom - monitorInfo.rcWork.Top) * dpiScaleY);
        }
        else
        {
            workArea = SystemParameters.WorkArea;
        }

        var position = CalculateDetailPosition(
            workArea,
            new Rect(mainWindow.Left, mainWindow.Top, mainWindow.ActualWidth, mainWindow.ActualHeight),
            new Size(detailWindow.ActualWidth, detailWindow.ActualHeight));

        detailWindow.Left = position.X;
        detailWindow.Top = position.Y;
    }

    public static TaskbarEdge DetectTaskbarEdge(Rect monitorArea, Rect workArea)
    {
        var insets = new[]
        {
            (Edge: TaskbarEdge.Left, Size: workArea.Left - monitorArea.Left),
            (Edge: TaskbarEdge.Top, Size: workArea.Top - monitorArea.Top),
            (Edge: TaskbarEdge.Right, Size: monitorArea.Right - workArea.Right),
            (Edge: TaskbarEdge.Bottom, Size: monitorArea.Bottom - workArea.Bottom)
        };

        var largest = insets.OrderByDescending(item => item.Size).First();
        return largest.Size > 0.5 ? largest.Edge : TaskbarEdge.Unknown;
    }

    public static Point CalculatePopupPosition(TaskbarEdge taskbarEdge, Rect workArea, Size windowSize)
    {
        const double taskbarGap = 8;
        const double endMargin = 12;

        double left = taskbarEdge == TaskbarEdge.Left
            ? workArea.Left + taskbarGap
            : workArea.Right - windowSize.Width - (taskbarEdge == TaskbarEdge.Right ? taskbarGap : endMargin);

        double top = taskbarEdge == TaskbarEdge.Top
            ? workArea.Top + taskbarGap
            : workArea.Bottom - windowSize.Height - (taskbarEdge is TaskbarEdge.Left or TaskbarEdge.Right ? endMargin : taskbarGap);

        return ClampToWorkArea(new Point(left, top), workArea, windowSize);
    }

    public static Point CalculateDetailPosition(Rect workArea, Rect mainWindow, Size detailSize)
    {
        const double gap = 8;
        double leftCandidate = mainWindow.Left - detailSize.Width - gap;
        double rightCandidate = mainWindow.Right + gap;

        double left = leftCandidate >= workArea.Left
            ? leftCandidate
            : rightCandidate + detailSize.Width <= workArea.Right
                ? rightCandidate
                : leftCandidate;

        double top = mainWindow.Bottom - detailSize.Height;
        return ClampToWorkArea(new Point(left, top), workArea, detailSize);
    }

    public static bool IsFlyoutAlignedWithTaskbar(
        TaskbarEdge taskbarEdge,
        Rect flyoutArea,
        Rect workArea,
        double tolerance = 50)
    {
        bool leftAligned = Math.Abs(flyoutArea.Left - workArea.Left) < tolerance;
        bool topAligned = Math.Abs(flyoutArea.Top - workArea.Top) < tolerance;
        bool rightAligned = Math.Abs(flyoutArea.Right - workArea.Right) < tolerance;
        bool bottomAligned = Math.Abs(flyoutArea.Bottom - workArea.Bottom) < tolerance;

        return taskbarEdge switch
        {
            TaskbarEdge.Top => rightAligned && topAligned,
            TaskbarEdge.Left => leftAligned && bottomAligned,
            TaskbarEdge.Right => rightAligned && bottomAligned,
            _ => rightAligned && bottomAligned
        };
    }

    private static Point ClampToWorkArea(Point position, Rect workArea, Size windowSize)
    {
        double maxLeft = Math.Max(workArea.Left, workArea.Right - windowSize.Width);
        double maxTop = Math.Max(workArea.Top, workArea.Bottom - windowSize.Height);

        return new Point(
            Math.Clamp(position.X, workArea.Left, maxLeft),
            Math.Clamp(position.Y, workArea.Top, maxTop));
    }
}
