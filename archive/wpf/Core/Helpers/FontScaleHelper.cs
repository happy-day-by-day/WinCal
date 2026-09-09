namespace WinCal.Core.Helpers;

/// <summary>
/// 日历字号档位与缩放比例的统一定义。
/// </summary>
public static class FontScaleHelper
{
    public const int MinimumOffset = -2;
    public const int MaximumOffset = 10;
    public const double ScalePerStep = 0.06;

    public static int ClampOffset(int offset) =>
        Math.Clamp(offset, MinimumOffset, MaximumOffset);

    public static double GetScale(int offset) =>
        1.0 + ClampOffset(offset) * ScalePerStep;

    public static string GetLabel(int offset) => ClampOffset(offset) switch
    {
        -2 => "最小",
        -1 => "较小",
        0 => "标准",
        1 => "较大",
        2 => "大",
        3 => "很大",
        4 => "超大",
        5 or 6 => "特大",
        7 or 8 => "极大",
        _ => "最大"
    };

    public static string GetPercentageText(int offset) =>
        $"{GetScale(offset):P0}";
}
