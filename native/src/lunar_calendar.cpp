#include "lunar_calendar.h"

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>

#include <array>
#include <string_view>

namespace
{
using winrt::Windows::Globalization::Calendar;
using winrt::Windows::Globalization::CalendarIdentifiers;
using winrt::Windows::Globalization::ClockIdentifiers;

std::wstring_view LunarMonthName(int month)
{
    static constexpr std::array<std::wstring_view, 13> names{
        L"", L"正月", L"二月", L"三月", L"四月", L"五月", L"六月",
        L"七月", L"八月", L"九月", L"十月", L"冬月", L"腊月"};
    return month >= 1 && month <= 12 ? names[static_cast<size_t>(month)] : L"";
}

std::wstring_view LunarDayName(int day)
{
    static constexpr std::array<std::wstring_view, 31> names{
        L"", L"初一", L"初二", L"初三", L"初四", L"初五", L"初六", L"初七", L"初八", L"初九", L"初十",
        L"十一", L"十二", L"十三", L"十四", L"十五", L"十六", L"十七", L"十八", L"十九", L"二十",
        L"廿一", L"廿二", L"廿三", L"廿四", L"廿五", L"廿六", L"廿七", L"廿八", L"廿九", L"三十"};
    return day >= 1 && day <= 30 ? names[static_cast<size_t>(day)] : L"";
}

std::wstring FestivalName(int month, int day, bool leapMonth)
{
    if (leapMonth) return {};
    const int key = month * 100 + day;
    switch (key)
    {
    case 101: return L"春节";
    case 115: return L"元宵节";
    case 505: return L"端午节";
    case 707: return L"七夕";
    case 815: return L"中秋节";
    case 909: return L"重阳节";
    case 1230: return L"除夕";
    default: return {};
    }
}

winrt::Windows::Foundation::DateTime LocalNoon(int year, int month, int day)
{
    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(year);
    local.wMonth = static_cast<WORD>(month);
    local.wDay = static_cast<WORD>(day);
    local.wHour = 12;
    SYSTEMTIME utc{};
    FILETIME fileTime{};
    if (!TzSpecificLocalTimeToSystemTimeEx(nullptr, &local, &utc) ||
        !SystemTimeToFileTime(&utc, &fileTime))
    {
        return winrt::clock::now();
    }

    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return winrt::clock::from_file_time(winrt::file_time{value.QuadPart});
}

Calendar CreateChineseCalendar()
{
    auto languages = winrt::single_threaded_vector<winrt::hstring>();
    languages.Append(L"zh-Hans-CN");
    // 使用系统当前时区；Windows 时区键（例如 China Standard Time）不能直接传给此 API。
    return Calendar(languages, CalendarIdentifiers::ChineseLunar(), ClockIdentifiers::TwentyFourHour());
}
}

namespace wincal
{
std::wstring LunarCalendar::TextForDate(int year, int month, int day)
{
    const auto key = std::tuple{year, month, day};
    if (const auto found = cache_.find(key); found != cache_.end())
        return found->second;

    std::wstring text;
    try
    {
        static Calendar calendar = CreateChineseCalendar();
        calendar.SetDateTime(LocalNoon(year, month, day));
        const int lunarDay = calendar.Day();
        const std::wstring lunarMonthName = calendar.MonthAsString().c_str();
        const bool isLeapMonth = lunarMonthName.find(L"闰") != std::wstring::npos;
        const int lunarMonth = calendar.Month();

        if (const auto festival = FestivalName(lunarMonth, lunarDay, isLeapMonth); !festival.empty())
            text = festival;
        else if (lunarDay == 1 && lunarMonth > 0)
            text = isLeapMonth ? L"闰" + std::wstring(LunarMonthName(lunarMonth))
                               : std::wstring(LunarMonthName(lunarMonth));
        else
            text = LunarDayName(lunarDay);
    }
    catch (...)
    {
        // 不支持的日期范围或系统农历不可用时，保留日期主信息。
    }

    // 日历切换时只需保留有限的最近日期；避免长期浏览年份导致缓存无边界增长。
    if (cache_.size() >= 512)
        cache_.clear();
    cache_.emplace(key, text);
    return text;
}
}
