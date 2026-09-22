#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace wincal
{
template<class Clock, class Duration>
bool CacheIsFresh(std::chrono::time_point<Clock, Duration> loaded,
                  std::chrono::time_point<Clock, Duration> now,
                  std::chrono::minutes lifetime)
{
    return now >= loaded && now - loaded < lifetime;
}

template<class Callback, class Rep, class Period>
void RunRefreshLoop(std::stop_token stop, std::chrono::duration<Rep, Period> interval,
                    Callback refresh)
{
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    while (!stop.stop_requested())
    {
        refresh();
        wake.wait_for(lock, stop, interval, [] { return false; });
    }
}
}
