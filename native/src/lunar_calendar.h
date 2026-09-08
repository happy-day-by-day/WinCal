#pragma once

#include <map>
#include <string>
#include <tuple>

namespace wincal
{
class LunarCalendar
{
public:
    [[nodiscard]] std::wstring TextForDate(int year, int month, int day);

private:
    std::map<std::tuple<int, int, int>, std::wstring> cache_;
};
}
