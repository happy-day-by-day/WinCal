#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace wincal
{
enum class ScheduleLabel
{
    None,
    Rest,
    Work,
};

enum class SystemCalendarState
{
    Disabled,
    Loading,
    Available,
    Unavailable,
};

struct CalendarEvent
{
    std::wstring title;
    int startYear{};
    int startMonth{};
    int startDay{};
    int startHour{};
    int startMinute{};
    int endYear{};
    int endMonth{};
    int endDay{};
    bool allDay{};
};

class CalendarData
{
public:
    void Load();
    [[nodiscard]] bool RefreshFromNetwork(std::stop_token stopToken);
    [[nodiscard]] bool RefreshSystemCalendar(int year, int month, std::stop_token stopToken);

    [[nodiscard]] bool HasEvents(int year, int month, int day) const;
    [[nodiscard]] size_t EventCountForDate(int year, int month, int day) const;
    [[nodiscard]] std::vector<CalendarEvent> EventsForDate(int year, int month, int day) const;
    [[nodiscard]] ScheduleLabel ScheduleForDate(int year, int month, int day) const;
    [[nodiscard]] std::vector<std::wstring> UpcomingLines(
        int year, int month, int day, size_t maximumLines = 2) const;
    [[nodiscard]] size_t EventCount() const;
    [[nodiscard]] size_t IcsEventCount() const;
    [[nodiscard]] size_t SystemEventCount() const;
    [[nodiscard]] size_t SystemLoadedMonthCount() const;
    [[nodiscard]] SystemCalendarState SystemState() const;
    [[nodiscard]] bool UsesSystemCalendar() const;

private:
    struct Subscription
    {
        std::wstring url;
        std::filesystem::path cachePath;
    };

    mutable std::mutex mutex_;
    std::vector<CalendarEvent> events_;
    std::vector<CalendarEvent> icsEvents_;
    std::vector<CalendarEvent> systemEvents_;
    std::map<std::pair<int, int>, std::vector<CalendarEvent>> systemEventsByMonth_;
    std::vector<Subscription> subscriptions_;
    bool usesIcs_{};
    bool usesSystemCalendar_{};
    SystemCalendarState systemState_{SystemCalendarState::Disabled};
    int refreshMinutes_{30};
    int upcomingDays_{3};

    void RebuildEventsLocked();
};
}
