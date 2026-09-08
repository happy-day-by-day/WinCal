#include "app_settings.h"
#include "lunar_calendar.h"

#include <winrt/base.h>

#include <iostream>

int main()
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    wincal::LunarCalendar lunar;
    const std::wstring todayText = lunar.TextForDate(2026, 9, 8);
    if (todayText.empty())
    {
        std::wcerr << L"Chinese lunar calendar returned no text.\n";
        return 1;
    }

    const auto settings = wincal::SettingsStore::Load();
    if (settings.themeMode.empty() || settings.dataSource.empty() || settings.weekStartDay.empty())
    {
        std::wcerr << L"Settings defaults failed to load.\n";
        return 2;
    }
    if (!settings.icsAliases.empty() && settings.icsAliases.front().rfind(L"\\u", 0) == 0)
    {
        std::wcerr << L"ICS alias Unicode escape was not decoded.\n";
        return 3;
    }

    std::cout << "lunarLength=" << todayText.size()
              << "; sourceLength=" << settings.dataSource.size()
              << "; urls=" << settings.icsUrls.size()
              << "; aliases=" << settings.icsAliases.size() << "\n";
    return 0;
}
