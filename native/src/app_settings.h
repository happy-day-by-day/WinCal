#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace wincal
{
struct AppSettings
{
    std::wstring themeMode{L"FollowSystem"};
    int fontSizeOffset{};
    bool autoStartup{};
    std::wstring dataSource{L"SystemCalendar"};
    std::vector<std::wstring> icsUrls;
    std::vector<std::wstring> icsAliases;
    int icsRefreshMinutes{30};
    int upcomingDays{3};
    std::wstring weekStartDay{L"Sunday"};
};

class SettingsStore
{
public:
    [[nodiscard]] static AppSettings Load();
    [[nodiscard]] static bool Save(const AppSettings& settings);
    [[nodiscard]] static std::filesystem::path SettingsPath();
    [[nodiscard]] static std::filesystem::path CacheDirectory();
};
}
