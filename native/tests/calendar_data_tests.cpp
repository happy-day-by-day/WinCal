// Include the parser implementation to exercise internal ICS helpers without I/O.
#include "../src/calendar_data.cpp"
#include <iostream>
#include <thread>
#include <future>

int main()
{
    int failures = 0;
    const auto check = [&](bool condition, const char* name)
    {
        if (!condition) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
    };
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::string year = std::to_string(now.wYear);
    const auto parse = [&](std::string body)
    {
        size_t position{};
        while ((position = body.find("YYYY", position)) != std::string::npos)
            body.replace(position, 4, year);
        return ParseIcs("BEGIN:VCALENDAR\n" + body + "END:VCALENDAR\n");
    };
    const auto event = [](const std::string& fields)
    {
        return "BEGIN:VEVENT\n" + fields + "END:VEVENT\n";
    };
    auto overnight = parse(event("DTSTART:YYYY0922T230000\nDTEND:YYYY0923T020000\n"));
    check(OccursOn(overnight.at(0), now.wYear, 9, 23), "overnight includes final day");
    auto midnight = parse(event("DTSTART:YYYY0922T230000\nDTEND:YYYY0923T000000\n"));
    check(!OccursOn(midnight.at(0), now.wYear, 9, 23), "midnight end remains exclusive");
    auto seconds = parse(event("DTSTART:YYYY0922T230000\nDTEND:YYYY0923T000001\n"));
    check(OccursOn(seconds.at(0), now.wYear, 9, 23), "end seconds preserved");
    auto allDay = parse(event("DTSTART;VALUE=DATE:YYYY0922\nDTEND;VALUE=DATE:YYYY0924\n"));
    check(OccursOn(allDay.at(0), now.wYear, 9, 23) &&
        !OccursOn(allDay.at(0), now.wYear, 9, 24), "all-day exclusive end");
    const auto master = event("UID:series\nDTSTART:YYYY0922T090000\nDTEND:YYYY0922T100000\nRRULE:FREQ=DAILY;COUNT=2\nSUMMARY:Meeting\n");
    const auto exception = event("UID:series\nRECURRENCE-ID:YYYY0923T090000\nDTSTART:YYYY0924T110000\nDTEND:YYYY0924T120000\nSUMMARY:Moved\n");
    for (const auto& body : {master + exception, exception + master})
    {
        auto moved = parse(body);
        check(moved.size() == 2, "exception replaces rather than duplicates");
        check(std::none_of(moved.begin(), moved.end(), [&](const auto& e) {
            return OccursOn(e, now.wYear, 9, 23); }), "moved original date removed");
        check(std::any_of(moved.begin(), moved.end(), [&](const auto& e) {
            return OccursOn(e, now.wYear, 9, 24) && e.startHour == 11;
        }), "replacement date and time retained");
    }
    auto sameDay = parse(master + event("UID:series\nRECURRENCE-ID:YYYY0923T090000\nDTSTART:YYYY0923T110000\nDTEND:YYYY0923T120000\n"));
    check(sameDay.size() == 2 && std::count_if(sameDay.begin(), sameDay.end(), [&](const auto& e) {
        return OccursOn(e, now.wYear, 9, 23) && e.startHour == 11;
    }) == 1, "same-day reschedule retains only replacement");
    auto recurringOvernight = parse(event("DTSTART:YYYY0922T230000\nDTEND:YYYY0923T020000\nRRULE:FREQ=DAILY;COUNT=2\n"));
    check(recurringOvernight.size() == 2 && OccursOn(recurringOvernight.at(1), now.wYear, 9, 24),
        "recurring overnight retains end time");
    auto cancelled = parse(master + event("UID:series\nRECURRENCE-ID:YYYY0923T090000\nSTATUS:CANCELLED\n"));
    check(cancelled.size() == 1, "cancelled exception without DTSTART removes occurrence");
    auto unrelated = parse(master + event("UID:other\nRECURRENCE-ID:YYYY0923T090000\nDTSTART:YYYY0923T110000\n"));
    check(unrelated.size() == 3, "exceptions do not affect other UIDs");
    const auto loaded = std::chrono::steady_clock::now();
    check(wincal::CacheIsFresh(loaded, loaded + std::chrono::seconds(59), std::chrono::minutes(1)), "fresh system cache");
    check(!wincal::CacheIsFresh(loaded, loaded + std::chrono::minutes(1), std::chrono::minutes(1)), "expired system cache");
    check(!wincal::CacheIsFresh(loaded, loaded - std::chrono::seconds(1), std::chrono::minutes(1)), "future cache is stale");
    const auto fileLoaded = std::filesystem::file_time_type::clock::now();
    check(!wincal::CacheIsFresh(fileLoaded, fileLoaded + std::chrono::minutes(30), std::chrono::minutes(30)), "ICS interval expires");
    std::stop_source stop;
    int calls = 0;
    wincal::RunRefreshLoop(stop.get_token(), std::chrono::milliseconds(1), [&] {
        if (++calls == 3) stop.request_stop();
    });
    check(calls == 3, "refresh repeats without user input");
    std::promise<void> started;
    auto ready = started.get_future();
    std::jthread worker([&](std::stop_token token) {
        wincal::RunRefreshLoop(token, std::chrono::hours(1), [&] { started.set_value(); });
    });
    ready.wait();
    const auto shutdown = std::chrono::steady_clock::now();
    worker.request_stop();
    worker.join();
    check(std::chrono::steady_clock::now() - shutdown < std::chrono::seconds(2), "shutdown interrupts refresh wait");
    std::cout << "Calendar regression failures: " << failures << '\n';
    return failures ? 1 : 0;
}
