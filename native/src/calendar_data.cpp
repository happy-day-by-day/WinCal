#include "calendar_data.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Appointments.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

namespace
{
using wincal::CalendarEvent;

class InternetHandle
{
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle()
    {
        if (value_) WinHttpCloseHandle(value_);
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

private:
    HINTERNET value_{};
};

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), length);
    return result;
}

std::string DownloadUrl(const std::wstring& url, std::stop_token stopToken)
{
    if (url.empty() || stopToken.stop_requested())
        return {};

    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
    {
        return {};
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0)
        resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (resource.empty()) resource = L"/";

    InternetHandle session(WinHttpOpen(
        L"WinCal/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return {};
    WinHttpSetTimeouts(session.get(), 3000, 3000, 5000, 5000);

    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection || stopToken.stop_requested())
        return {};

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request)
        return {};

    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(
        request.get(), WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    if (!WinHttpSendRequest(
            request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        stopToken.stop_requested() || !WinHttpReceiveResponse(request.get(), nullptr))
    {
        return {};
    }

    DWORD status{};
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300)
    {
        return {};
    }

    constexpr size_t maximumSize = 10 * 1024 * 1024;
    std::string content;
    while (!stopToken.stop_requested())
    {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request.get(), &available))
            return {};
        if (available == 0)
            return content;
        if (content.size() + available > maximumSize)
            return {};

        const size_t offset = content.size();
        content.resize(offset + available);
        DWORD read{};
        if (!WinHttpReadData(request.get(), content.data() + offset, available, &read))
            return {};
        content.resize(offset + read);
        if (read == 0)
            return content;
    }
    return {};
}

bool IsValidIcs(std::string_view content)
{
    return content.find("BEGIN:VCALENDAR") != std::string_view::npos &&
           content.find("END:VCALENDAR") != std::string_view::npos;
}

bool ReplaceFileAtomically(const std::filesystem::path& path, std::string_view content)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    auto temporary = path;
    temporary += L".download";
    std::filesystem::remove(temporary, error);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream)
        {
            stream.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

void AppendUtf8(std::string& output, unsigned codepoint)
{
    if (codepoint <= 0x7F)
        output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7FF)
    {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

int HexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string DecodeJsonString(std::string_view value)
{
    std::string result;
    for (size_t index = 0; index < value.size(); ++index)
    {
        const char current = value[index];
        if (current != '\\' || index + 1 >= value.size())
        {
            result.push_back(current);
            continue;
        }

        const char escaped = value[++index];
        switch (escaped)
        {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u':
        {
            if (index + 4 >= value.size())
                break;
            unsigned codepoint{};
            bool valid = true;
            for (int digitIndex = 0; digitIndex < 4; ++digitIndex)
            {
                const int digit = HexDigit(value[index + 1 + digitIndex]);
                if (digit < 0)
                {
                    valid = false;
                    break;
                }
                codepoint = codepoint * 16 + static_cast<unsigned>(digit);
            }
            if (valid)
            {
                AppendUtf8(result, codepoint);
                index += 4;
            }
            break;
        }
        default: result.push_back(escaped); break;
        }
    }
    return result;
}

size_t FindJsonValue(std::string_view json, std::string_view key)
{
    const std::string quotedKey = "\"" + std::string(key) + "\"";
    const size_t keyPosition = json.find(quotedKey);
    if (keyPosition == std::string_view::npos)
        return keyPosition;
    const size_t colon = json.find(':', keyPosition + quotedKey.size());
    if (colon == std::string_view::npos)
        return colon;
    return json.find_first_not_of(" \t\r\n", colon + 1);
}

std::string JsonString(std::string_view json, std::string_view key)
{
    const size_t start = FindJsonValue(json, key);
    if (start == std::string_view::npos || json[start] != '"')
        return {};
    bool escaped = false;
    for (size_t index = start + 1; index < json.size(); ++index)
    {
        if (!escaped && json[index] == '"')
            return DecodeJsonString(json.substr(start + 1, index - start - 1));
        escaped = !escaped && json[index] == '\\';
        if (json[index] != '\\') escaped = false;
    }
    return {};
}

int JsonInteger(std::string_view json, std::string_view key, int fallback)
{
    const size_t start = FindJsonValue(json, key);
    if (start == std::string_view::npos)
        return fallback;
    size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-'))
        ++end;
    try { return std::stoi(std::string(json.substr(start, end - start))); }
    catch (...) { return fallback; }
}

std::vector<std::string> JsonStringArray(std::string_view json, std::string_view key)
{
    std::vector<std::string> values;
    size_t position = FindJsonValue(json, key);
    if (position == std::string_view::npos || json[position] != '[')
        return values;
    ++position;

    while (position < json.size())
    {
        position = json.find_first_not_of(" \t\r\n,", position);
        if (position == std::string_view::npos || json[position] == ']')
            break;
        if (json[position] != '"')
            break;

        const size_t start = ++position;
        bool escaped = false;
        for (; position < json.size(); ++position)
        {
            if (!escaped && json[position] == '"')
            {
                values.push_back(DecodeJsonString(json.substr(start, position - start)));
                ++position;
                break;
            }
            escaped = !escaped && json[position] == '\\';
            if (json[position] != '\\') escaped = false;
        }
    }
    return values;
}

std::wstring Sha256Prefix(std::string_view value)
{
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength{};
    DWORD copied{};
    std::vector<UCHAR> object;
    std::array<UCHAR, 32> digest{};

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    if (BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength), &copied, 0) < 0)
        goto cleanup;
    object.resize(objectLength);
    if (BCryptCreateHash(
            algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0)
        goto cleanup;
    if (BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()), 0) < 0)
        goto cleanup;
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        goto cleanup;

    {
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        std::wstring result;
        result.reserve(16);
        for (size_t index = 0; index < 8; ++index)
        {
            result.push_back(digits[digest[index] >> 4]);
            result.push_back(digits[digest[index] & 0x0F]);
        }
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    }

cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
}

std::filesystem::path LocalAppData()
{
    PWSTR value{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value)))
        return {};
    std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}

std::vector<std::string> UnfoldLines(std::string_view content)
{
    std::vector<std::string> lines;
    size_t position{};
    while (position <= content.size())
    {
        size_t end = content.find('\n', position);
        if (end == std::string_view::npos) end = content.size();
        std::string line(content.substr(position, end - position));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t') && !lines.empty())
            lines.back().append(line.substr(1));
        else
            lines.push_back(std::move(line));
        if (end == content.size()) break;
        position = end + 1;
    }
    return lines;
}

long long DayNumber(int year, unsigned month, unsigned day);
int DaysInMonth(int year, int month);
int Weekday(long long day);

std::string PropertyParameter(std::string_view property, std::string_view parameterName)
{
    const std::string marker = ";" + std::string(parameterName) + "=";
    const size_t start = property.find(marker);
    if (start == std::string_view::npos)
        return {};
    const size_t valueStart = start + marker.size();
    size_t valueEnd = property.find(';', valueStart);
    if (valueEnd == std::string_view::npos) valueEnd = property.size();
    std::string value(property.substr(valueStart, valueEnd - valueStart));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::wstring WindowsTimeZoneId(std::string_view timeZoneId)
{
    static constexpr std::pair<std::string_view, std::wstring_view> mappings[]{
        {"Asia/Shanghai", L"China Standard Time"},
        {"Asia/Hong_Kong", L"China Standard Time"},
        {"Asia/Taipei", L"Taipei Standard Time"},
        {"Asia/Tokyo", L"Tokyo Standard Time"},
        {"Asia/Seoul", L"Korea Standard Time"},
        {"Asia/Singapore", L"Singapore Standard Time"},
        {"Asia/Kolkata", L"India Standard Time"},
        {"Europe/London", L"GMT Standard Time"},
        {"Europe/Paris", L"Romance Standard Time"},
        {"Europe/Berlin", L"W. Europe Standard Time"},
        {"Europe/Moscow", L"Russian Standard Time"},
        {"America/New_York", L"Eastern Standard Time"},
        {"America/Chicago", L"Central Standard Time"},
        {"America/Denver", L"Mountain Standard Time"},
        {"America/Los_Angeles", L"Pacific Standard Time"},
        {"America/Toronto", L"Eastern Standard Time"},
        {"America/Vancouver", L"Pacific Standard Time"},
        {"Australia/Sydney", L"AUS Eastern Standard Time"},
        {"Pacific/Auckland", L"New Zealand Standard Time"},
        {"UTC", L"UTC"},
        {"Etc/UTC", L"UTC"},
    };

    for (const auto& [iana, windows] : mappings)
    {
        if (timeZoneId == iana)
            return std::wstring(windows);
    }
    return Utf8ToWide(timeZoneId);
}

std::optional<DYNAMIC_TIME_ZONE_INFORMATION> FindTimeZone(std::string_view timeZoneId)
{
    const std::wstring wanted = WindowsTimeZoneId(timeZoneId);
    if (wanted.empty())
        return std::nullopt;

    for (DWORD index{};; ++index)
    {
        DYNAMIC_TIME_ZONE_INFORMATION candidate{};
        const DWORD result = EnumDynamicTimeZoneInformation(index, &candidate);
        if (result == ERROR_NO_MORE_ITEMS)
            break;
        if (result != ERROR_SUCCESS)
            continue;
        if (_wcsicmp(candidate.TimeZoneKeyName, wanted.c_str()) == 0 ||
            _wcsicmp(candidate.StandardName, wanted.c_str()) == 0 ||
            _wcsicmp(candidate.DaylightName, wanted.c_str()) == 0)
        {
            return candidate;
        }
    }
    return std::nullopt;
}

struct TimeZoneObservance
{
    int startYear{};
    int startMonth{};
    int startDay{};
    int startHour{};
    int startMinute{};
    int offsetMinutes{};
    int ruleMonth{};
    int ruleMonthDay{};
    int ruleOrdinal{};
    int ruleWeekday{-1};
    bool recurring{};
};

struct EmbeddedTimeZone
{
    std::vector<TimeZoneObservance> observances;
};

using EmbeddedTimeZones = std::map<std::string, EmbeddedTimeZone>;

std::string_view RuleField(std::string_view rule, std::string_view name)
{
    const std::string marker = std::string(name) + "=";
    size_t start = rule.find(marker);
    if (start == std::string_view::npos) return {};
    start += marker.size();
    size_t end = rule.find(';', start);
    if (end == std::string_view::npos) end = rule.size();
    return rule.substr(start, end - start);
}

int ParseOffsetMinutes(std::string_view value)
{
    if (value.size() < 5 || (value.front() != '+' && value.front() != '-'))
        return 0;
    try
    {
        const int hours = std::stoi(std::string(value.substr(1, 2)));
        const int minutes = std::stoi(std::string(value.substr(3, 2)));
        return (value.front() == '-' ? -1 : 1) * (hours * 60 + minutes);
    }
    catch (...) { return 0; }
}

void ParseObservanceRule(std::string_view value, TimeZoneObservance& observance)
{
    observance.recurring = RuleField(value, "FREQ") == "YEARLY";
    try
    {
        const auto month = RuleField(value, "BYMONTH");
        if (!month.empty()) observance.ruleMonth = std::stoi(std::string(month));
        const auto monthDay = RuleField(value, "BYMONTHDAY");
        if (!monthDay.empty()) observance.ruleMonthDay = std::stoi(std::string(monthDay));
    }
    catch (...) {}

    const auto byDay = RuleField(value, "BYDAY");
    if (byDay.size() >= 2)
    {
        const auto code = byDay.substr(byDay.size() - 2);
        if (code == "SU") observance.ruleWeekday = 0;
        else if (code == "MO") observance.ruleWeekday = 1;
        else if (code == "TU") observance.ruleWeekday = 2;
        else if (code == "WE") observance.ruleWeekday = 3;
        else if (code == "TH") observance.ruleWeekday = 4;
        else if (code == "FR") observance.ruleWeekday = 5;
        else if (code == "SA") observance.ruleWeekday = 6;
        if (byDay.size() > 2)
        {
            try
            {
                observance.ruleOrdinal =
                    std::stoi(std::string(byDay.substr(0, byDay.size() - 2)));
            }
            catch (...) {}
        }
    }
}

EmbeddedTimeZones ParseEmbeddedTimeZones(std::string_view content)
{
    EmbeddedTimeZones timeZones;
    EmbeddedTimeZone currentZone;
    TimeZoneObservance currentObservance;
    std::string timeZoneId;
    bool insideZone{};
    bool insideObservance{};

    for (const auto& line : UnfoldLines(content))
    {
        if (line == "BEGIN:VTIMEZONE")
        {
            insideZone = true;
            currentZone = {};
            timeZoneId.clear();
            continue;
        }
        if (!insideZone) continue;
        if (line == "BEGIN:STANDARD" || line == "BEGIN:DAYLIGHT")
        {
            insideObservance = true;
            currentObservance = {};
            continue;
        }
        if (line == "END:STANDARD" || line == "END:DAYLIGHT")
        {
            if (insideObservance && currentObservance.startYear > 0)
                currentZone.observances.push_back(currentObservance);
            insideObservance = false;
            continue;
        }
        if (line == "END:VTIMEZONE")
        {
            if (!timeZoneId.empty() && !currentZone.observances.empty())
                timeZones[timeZoneId] = std::move(currentZone);
            insideZone = false;
            continue;
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string_view property(line.data(), colon);
        const std::string_view value(line.data() + colon + 1, line.size() - colon - 1);
        if (!insideObservance && property == "TZID")
            timeZoneId.assign(value);
        else if (insideObservance && property == "TZOFFSETTO")
            currentObservance.offsetMinutes = ParseOffsetMinutes(value);
        else if (insideObservance && property == "RRULE")
            ParseObservanceRule(value, currentObservance);
        else if (insideObservance && property == "DTSTART" && value.size() >= 8)
        {
            try
            {
                currentObservance.startYear = std::stoi(std::string(value.substr(0, 4)));
                currentObservance.startMonth = std::stoi(std::string(value.substr(4, 2)));
                currentObservance.startDay = std::stoi(std::string(value.substr(6, 2)));
                if (value.size() >= 13 && value[8] == 'T')
                {
                    currentObservance.startHour = std::stoi(std::string(value.substr(9, 2)));
                    currentObservance.startMinute = std::stoi(std::string(value.substr(11, 2)));
                }
            }
            catch (...) { currentObservance.startYear = 0; }
        }
    }
    return timeZones;
}

long long TransitionMinute(const TimeZoneObservance& observance, int year)
{
    int month = observance.ruleMonth ? observance.ruleMonth : observance.startMonth;
    int day = observance.ruleMonthDay ? observance.ruleMonthDay : observance.startDay;
    if (day < 0) day = DaysInMonth(year, month) + day + 1;
    if (observance.ruleWeekday >= 0)
    {
        const int ordinal = observance.ruleOrdinal == 0 ? 1 : observance.ruleOrdinal;
        if (ordinal > 0)
        {
            const int firstWeekday = Weekday(DayNumber(year, month, 1));
            day = 1 + (observance.ruleWeekday - firstWeekday + 7) % 7 + (ordinal - 1) * 7;
        }
        else
        {
            const int lastDay = DaysInMonth(year, month);
            const int lastWeekday = Weekday(DayNumber(year, month, lastDay));
            day = lastDay - (lastWeekday - observance.ruleWeekday + 7) % 7 + (ordinal + 1) * 7;
        }
    }
    return DayNumber(year, month, day) * 1440 + observance.startHour * 60 + observance.startMinute;
}

int EmbeddedOffsetMinutes(const EmbeddedTimeZone& timeZone, const SYSTEMTIME& local)
{
    const long long requested = DayNumber(local.wYear, local.wMonth, local.wDay) * 1440 +
                                local.wHour * 60 + local.wMinute;
    long long latest = std::numeric_limits<long long>::min();
    int offset = timeZone.observances.front().offsetMinutes;
    for (const auto& observance : timeZone.observances)
    {
        if (observance.recurring)
        {
            for (int year : {static_cast<int>(local.wYear) - 1, static_cast<int>(local.wYear)})
            {
                if (year < observance.startYear) continue;
                const long long transition = TransitionMinute(observance, year);
                if (transition <= requested && transition > latest)
                {
                    latest = transition;
                    offset = observance.offsetMinutes;
                }
            }
        }
        else
        {
            const long long transition =
                DayNumber(observance.startYear, observance.startMonth, observance.startDay) * 1440 +
                observance.startHour * 60 + observance.startMinute;
            if (transition <= requested && transition > latest)
            {
                latest = transition;
                offset = observance.offsetMinutes;
            }
        }
    }
    return offset;
}

bool ParseIcsDate(
    std::string_view value, std::string_view property,
    const EmbeddedTimeZones& embeddedTimeZones, CalendarEvent& event, bool start)
{
    if (value.size() < 8)
        return false;
    const auto number = [value](size_t offset, size_t length)
    {
        int result{};
        for (size_t index = offset; index < offset + length; ++index)
        {
            if (!std::isdigit(static_cast<unsigned char>(value[index]))) return -1;
            result = result * 10 + value[index] - '0';
        }
        return result;
    };

    SYSTEMTIME date{};
    date.wYear = static_cast<WORD>(number(0, 4));
    date.wMonth = static_cast<WORD>(number(4, 2));
    date.wDay = static_cast<WORD>(number(6, 2));
    const bool allDay = property.find("VALUE=DATE") != std::string_view::npos || value.size() == 8;
    if (!allDay && value.size() >= 13 && value[8] == 'T')
    {
        date.wHour = static_cast<WORD>(number(9, 2));
        date.wMinute = static_cast<WORD>(number(11, 2));
        if (value.size() >= 15) date.wSecond = static_cast<WORD>(number(13, 2));
    }

    const int year = date.wYear;
    const int month = date.wMonth;
    const int day = date.wDay;
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31)
        return false;

    if (!allDay)
    {
        SYSTEMTIME utc{};
        SYSTEMTIME local{};
        const bool isUtc = !value.empty() && value.back() == 'Z';
        bool hasUtc = isUtc;
        if (isUtc)
            utc = date;
        else
        {
            const std::string timeZoneId = PropertyParameter(property, "TZID");
            if (!timeZoneId.empty())
            {
                if (const auto zone = FindTimeZone(timeZoneId))
                    hasUtc = TzSpecificLocalTimeToSystemTimeEx(&*zone, &date, &utc) != FALSE;
                else if (const auto embedded = embeddedTimeZones.find(timeZoneId);
                         embedded != embeddedTimeZones.end())
                {
                    FILETIME localFileTime{};
                    if (SystemTimeToFileTime(&date, &localFileTime))
                    {
                        ULARGE_INTEGER fileTimeValue{};
                        fileTimeValue.LowPart = localFileTime.dwLowDateTime;
                        fileTimeValue.HighPart = localFileTime.dwHighDateTime;
                        const long long offsetTicks =
                            static_cast<long long>(EmbeddedOffsetMinutes(embedded->second, date)) *
                            60LL * 10000000LL;
                        if (offsetTicks < 0 ||
                            fileTimeValue.QuadPart >= static_cast<unsigned long long>(offsetTicks))
                        {
                            fileTimeValue.QuadPart = static_cast<unsigned long long>(
                                static_cast<long long>(fileTimeValue.QuadPart) - offsetTicks);
                            localFileTime.dwLowDateTime = fileTimeValue.LowPart;
                            localFileTime.dwHighDateTime = fileTimeValue.HighPart;
                            hasUtc = FileTimeToSystemTime(&localFileTime, &utc) != FALSE;
                        }
                    }
                }
            }
        }
        if (hasUtc && SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local))
            date = local;
    }

    if (start)
    {
        event.startYear = date.wYear;
        event.startMonth = date.wMonth;
        event.startDay = date.wDay;
        event.startHour = date.wHour;
        event.startMinute = date.wMinute;
    }
    else
    {
        event.endYear = date.wYear;
        event.endMonth = date.wMonth;
        event.endDay = date.wDay;
    }
    return true;
}

struct ByDay
{
    int ordinal{};
    int weekday{};
};

struct RecurrenceRule
{
    std::string frequency;
    int interval{1};
    int count{};
    int weekStart{1};
    std::optional<long long> untilDay;
    std::vector<int> months;
    std::vector<int> monthDays;
    std::vector<ByDay> weekdays;
    std::vector<int> setPositions;
};

std::vector<std::string_view> Split(std::string_view value, char separator)
{
    std::vector<std::string_view> parts;
    size_t start{};
    while (start <= value.size())
    {
        size_t end = value.find(separator, start);
        if (end == std::string_view::npos) end = value.size();
        parts.push_back(value.substr(start, end - start));
        if (end == value.size()) break;
        start = end + 1;
    }
    return parts;
}

int Weekday(long long day)
{
    int value = static_cast<int>((day + 4) % 7);
    return value < 0 ? value + 7 : value;
}

std::tuple<int, int, int> CivilFromDayNumber(long long days)
{
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    int year = static_cast<int>(yearOfEra) + static_cast<int>(era * 400);
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    const unsigned month = monthPrime + (monthPrime < 10 ? 3 : -9);
    year += month <= 2;
    return {year, static_cast<int>(month), static_cast<int>(day)};
}

RecurrenceRule ParseRecurrenceRule(std::string_view value)
{
    RecurrenceRule rule;
    for (const auto part : Split(value, ';'))
    {
        const size_t equal = part.find('=');
        if (equal == std::string_view::npos) continue;
        const auto name = part.substr(0, equal);
        const auto data = part.substr(equal + 1);
        if (name == "FREQ") rule.frequency = std::string(data);
        else if (name == "INTERVAL")
        {
            try { rule.interval = std::max(1, std::stoi(std::string(data))); } catch (...) {}
        }
        else if (name == "COUNT")
        {
            try { rule.count = std::max(0, std::stoi(std::string(data))); } catch (...) {}
        }
        else if (name == "WKST" && data.size() >= 2)
        {
            const auto code = data.substr(data.size() - 2);
            if (code == "SU") rule.weekStart = 0;
            else if (code == "MO") rule.weekStart = 1;
            else if (code == "TU") rule.weekStart = 2;
            else if (code == "WE") rule.weekStart = 3;
            else if (code == "TH") rule.weekStart = 4;
            else if (code == "FR") rule.weekStart = 5;
            else if (code == "SA") rule.weekStart = 6;
        }
        else if (name == "UNTIL" && data.size() >= 8)
        {
            try
            {
                rule.untilDay = DayNumber(
                    std::stoi(std::string(data.substr(0, 4))),
                    static_cast<unsigned>(std::stoi(std::string(data.substr(4, 2)))),
                    static_cast<unsigned>(std::stoi(std::string(data.substr(6, 2)))));
            }
            catch (...) {}
        }
        else if (name == "BYMONTH" || name == "BYMONTHDAY" || name == "BYSETPOS")
        {
            auto& output = name == "BYMONTH"
                ? rule.months
                : (name == "BYMONTHDAY" ? rule.monthDays : rule.setPositions);
            for (const auto item : Split(data, ','))
            {
                try { output.push_back(std::stoi(std::string(item))); } catch (...) {}
            }
        }
        else if (name == "BYDAY")
        {
            for (const auto item : Split(data, ','))
            {
                if (item.size() < 2) continue;
                const auto code = item.substr(item.size() - 2);
                int weekday = -1;
                if (code == "SU") weekday = 0;
                else if (code == "MO") weekday = 1;
                else if (code == "TU") weekday = 2;
                else if (code == "WE") weekday = 3;
                else if (code == "TH") weekday = 4;
                else if (code == "FR") weekday = 5;
                else if (code == "SA") weekday = 6;
                if (weekday < 0) continue;
                int ordinal{};
                if (item.size() > 2)
                {
                    try { ordinal = std::stoi(std::string(item.substr(0, item.size() - 2))); }
                    catch (...) {}
                }
                rule.weekdays.push_back({ordinal, weekday});
            }
        }
    }
    return rule;
}

bool MatchesMonthDay(const std::vector<int>& values, int year, int month, int day)
{
    if (values.empty()) return true;
    const int count = DaysInMonth(year, month);
    return std::any_of(values.begin(), values.end(), [=](int value)
    {
        return value == day || (value < 0 && count + value + 1 == day);
    });
}

bool MatchesByDay(const std::vector<ByDay>& values, int year, int month, int day)
{
    if (values.empty()) return true;
    const int weekday = Weekday(DayNumber(year, month, day));
    const int positiveOrdinal = (day - 1) / 7 + 1;
    const int negativeOrdinal = -((DaysInMonth(year, month) - day) / 7 + 1);
    return std::any_of(values.begin(), values.end(), [&](const ByDay& value)
    {
        return value.weekday == weekday &&
               (value.ordinal == 0 || value.ordinal == positiveOrdinal || value.ordinal == negativeOrdinal);
    });
}

bool MatchesRecurrence(
    const RecurrenceRule& rule, const CalendarEvent& base,
    long long baseDay, long long candidateDay, int year, int month, int day)
{
    if (!rule.months.empty() &&
        std::find(rule.months.begin(), rule.months.end(), month) == rule.months.end())
        return false;
    if (!MatchesMonthDay(rule.monthDays, year, month, day) ||
        !MatchesByDay(rule.weekdays, year, month, day))
        return false;

    const long long dayDifference = candidateDay - baseDay;
    if (rule.frequency == "DAILY")
        return dayDifference % rule.interval == 0;
    if (rule.frequency == "WEEKLY")
    {
        const long long baseWeek = baseDay - (Weekday(baseDay) - rule.weekStart + 7) % 7;
        const long long candidateWeek =
            candidateDay - (Weekday(candidateDay) - rule.weekStart + 7) % 7;
        if (((candidateWeek - baseWeek) / 7) % rule.interval != 0) return false;
        return rule.weekdays.empty() ? Weekday(candidateDay) == Weekday(baseDay) : true;
    }

    const int monthDifference = (year - base.startYear) * 12 + month - base.startMonth;
    if (rule.frequency == "MONTHLY")
    {
        if (monthDifference < 0 || monthDifference % rule.interval != 0) return false;
        if (rule.monthDays.empty() && rule.weekdays.empty()) return day == base.startDay;
        return true;
    }
    if (rule.frequency == "YEARLY")
    {
        if ((year - base.startYear) % rule.interval != 0) return false;
        if (rule.months.empty() && month != base.startMonth) return false;
        if (rule.monthDays.empty() && rule.weekdays.empty() && day != base.startDay) return false;
        return true;
    }
    return false;
}

bool MatchesSetPosition(
    const RecurrenceRule& rule, const CalendarEvent& base,
    long long baseDay, long long candidateDay)
{
    if (rule.setPositions.empty())
        return true;

    long long periodStart = candidateDay;
    long long periodEnd = candidateDay;
    const auto [year, month, day] = CivilFromDayNumber(candidateDay);
    static_cast<void>(day);
    if (rule.frequency == "WEEKLY")
    {
        periodStart = candidateDay - (Weekday(candidateDay) - rule.weekStart + 7) % 7;
        periodEnd = periodStart + 6;
    }
    else if (rule.frequency == "MONTHLY")
    {
        periodStart = DayNumber(year, month, 1);
        periodEnd = DayNumber(year, month, DaysInMonth(year, month));
    }
    else if (rule.frequency == "YEARLY")
    {
        periodStart = DayNumber(year, 1, 1);
        periodEnd = DayNumber(year, 12, 31);
    }

    std::vector<long long> candidates;
    for (long long value = std::max(periodStart, baseDay); value <= periodEnd; ++value)
    {
        if (rule.untilDay && value > *rule.untilDay) break;
        const auto [candidateYear, candidateMonth, candidateDate] = CivilFromDayNumber(value);
        if (MatchesRecurrence(
                rule, base, baseDay, value,
                candidateYear, candidateMonth, candidateDate))
        {
            candidates.push_back(value);
        }
    }

    const auto found = std::find(candidates.begin(), candidates.end(), candidateDay);
    if (found == candidates.end())
        return false;
    const int positive = static_cast<int>(std::distance(candidates.begin(), found)) + 1;
    const int negative = positive - static_cast<int>(candidates.size()) - 1;
    return std::any_of(rule.setPositions.begin(), rule.setPositions.end(), [=](int position)
    {
        return position == positive || position == negative;
    });
}

std::vector<CalendarEvent> ExpandRecurringEvent(
    const CalendarEvent& base, std::string_view ruleText,
    const std::set<long long>& excludedDays, const std::set<long long>& additionalDays)
{
    if (ruleText.empty() && additionalDays.empty())
        return {base};

    const RecurrenceRule rule = ParseRecurrenceRule(ruleText);
    if (rule.frequency.empty() && additionalDays.empty())
        return {base};

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const long long rangeStart = DayNumber(now.wYear - 2, 1, 1);
    const long long rangeEnd = DayNumber(now.wYear + 10, 12, 31);
    const long long baseDay = DayNumber(base.startYear, base.startMonth, base.startDay);
    long long endDay = DayNumber(base.endYear, base.endMonth, base.endDay);
    const long long durationDays = std::max(0LL, endDay - baseDay);
    const long long iterationStart = rule.count > 0 ? baseDay : std::max(baseDay, rangeStart);

    std::set<long long> occurrenceDays = additionalDays;
    int generated{};
    if (!rule.frequency.empty())
    {
        for (long long candidate = iterationStart; candidate <= rangeEnd; ++candidate)
        {
            if (rule.untilDay && candidate > *rule.untilDay) break;
            const auto [year, month, day] = CivilFromDayNumber(candidate);
            const bool matches = candidate == baseDay ||
                (MatchesRecurrence(rule, base, baseDay, candidate, year, month, day) &&
                 MatchesSetPosition(rule, base, baseDay, candidate));
            if (!matches) continue;
            ++generated;
            if (candidate >= rangeStart && !excludedDays.contains(candidate))
                occurrenceDays.insert(candidate);
            if ((rule.count > 0 && generated >= rule.count) || generated >= 20000)
                break;
        }
    }
    else if (!excludedDays.contains(baseDay))
        occurrenceDays.insert(baseDay);

    std::vector<CalendarEvent> events;
    events.reserve(occurrenceDays.size());
    for (const long long occurrenceDay : occurrenceDays)
    {
        if (occurrenceDay < rangeStart || occurrenceDay > rangeEnd || excludedDays.contains(occurrenceDay))
            continue;
        CalendarEvent event = base;
        std::tie(event.startYear, event.startMonth, event.startDay) = CivilFromDayNumber(occurrenceDay);
        std::tie(event.endYear, event.endMonth, event.endDay) =
            CivilFromDayNumber(occurrenceDay + durationDays);
        events.push_back(std::move(event));
    }
    return events;
}

std::wstring DecodeIcsText(std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] == '\\' && index + 1 < value.size())
        {
            const char next = value[++index];
            decoded.push_back(next == 'n' || next == 'N' ? ' ' : next);
        }
        else
            decoded.push_back(value[index]);
    }
    return Utf8ToWide(decoded);
}

std::vector<CalendarEvent> ParseIcs(std::string_view content)
{
    const EmbeddedTimeZones embeddedTimeZones = ParseEmbeddedTimeZones(content);
    std::vector<CalendarEvent> events;
    CalendarEvent current;
    bool insideEvent{};
    bool hasStart{};
    std::string recurrenceRule;
    std::set<long long> excludedDays;
    std::set<long long> additionalDays;

    for (const auto& line : UnfoldLines(content))
    {
        if (line == "BEGIN:VEVENT")
        {
            current = {};
            insideEvent = true;
            hasStart = false;
            recurrenceRule.clear();
            excludedDays.clear();
            additionalDays.clear();
            continue;
        }
        if (line == "END:VEVENT")
        {
            if (insideEvent && hasStart)
            {
                if (!current.endYear)
                {
                    current.endYear = current.startYear;
                    current.endMonth = current.startMonth;
                    current.endDay = current.startDay;
                }
                if (current.title.empty()) current.title = L"(无标题)";
                auto expanded = ExpandRecurringEvent(
                    current, recurrenceRule, excludedDays, additionalDays);
                events.insert(
                    events.end(),
                    std::make_move_iterator(expanded.begin()),
                    std::make_move_iterator(expanded.end()));
            }
            insideEvent = false;
            continue;
        }
        if (!insideEvent)
            continue;

        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string_view property(line.data(), colon);
        const std::string_view value(line.data() + colon + 1, line.size() - colon - 1);
        const size_t semicolon = property.find(';');
        const std::string_view name = property.substr(0, semicolon);

        if (name == "DTSTART")
        {
            current.allDay = property.find("VALUE=DATE") != std::string_view::npos || value.size() == 8;
            hasStart = ParseIcsDate(value, property, embeddedTimeZones, current, true);
        }
        else if (name == "DTEND")
            ParseIcsDate(value, property, embeddedTimeZones, current, false);
        else if (name == "SUMMARY")
            current.title = DecodeIcsText(value);
        else if (name == "RRULE")
            recurrenceRule.assign(value);
        else if (name == "EXDATE" || name == "RDATE")
        {
            for (const auto item : Split(value, ','))
            {
                CalendarEvent dateEvent;
                if (ParseIcsDate(item, property, embeddedTimeZones, dateEvent, true))
                {
                    const long long date = DayNumber(
                        dateEvent.startYear, dateEvent.startMonth, dateEvent.startDay);
                    (name == "EXDATE" ? excludedDays : additionalDays).insert(date);
                }
            }
        }
    }
    return events;
}

long long DayNumber(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097LL + static_cast<long long>(dayOfEra) - 719468;
}

bool OccursOn(const CalendarEvent& event, int year, int month, int day)
{
    const long long selected = DayNumber(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const long long start = DayNumber(
        event.startYear, static_cast<unsigned>(event.startMonth), static_cast<unsigned>(event.startDay));
    long long end = DayNumber(
        event.endYear, static_cast<unsigned>(event.endMonth), static_cast<unsigned>(event.endDay));
    if (end <= start) end = start + 1;
    return selected >= start && selected < end;
}

bool Contains(const std::wstring& text, std::wstring_view value)
{
    return text.find(value) != std::wstring::npos;
}

bool HasScheduleMarker(const std::wstring& title)
{
    return Contains(title, L"（休）") || Contains(title, L"(休)") ||
           Contains(title, L"（班）") || Contains(title, L"(班)") ||
           Contains(title, L" 假期 第") || Contains(title, L" 补班 第");
}

std::wstring NormalizedIdentityTitle(const std::wstring& title)
{
    std::wstring normalized = title;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), towlower);
    while (!normalized.empty() && iswspace(normalized.back())) normalized.pop_back();
    size_t start{};
    while (start < normalized.size() && iswspace(normalized[start])) ++start;
    if (start > 0) normalized.erase(0, start);

    if (HasScheduleMarker(title))
    {
        const size_t chineseBracket = normalized.find(L'（');
        const size_t asciiBracket = normalized.find(L'(');
        const size_t marker = std::min(chineseBracket, asciiBracket);
        if (marker != std::wstring::npos)
            normalized.erase(marker);
        const size_t holidaySuffix = normalized.find(L" 假期 第");
        const size_t workSuffix = normalized.find(L" 补班 第");
        const size_t suffix = std::min(holidaySuffix, workSuffix);
        if (suffix != std::wstring::npos)
            normalized.erase(suffix);
        while (!normalized.empty() && iswspace(normalized.back())) normalized.pop_back();
    }
    return normalized;
}

long long EventSpanDays(const CalendarEvent& event)
{
    return DayNumber(event.endYear, event.endMonth, event.endDay) -
           DayNumber(event.startYear, event.startMonth, event.startDay);
}

std::vector<CalendarEvent> DeduplicateEvents(std::vector<CalendarEvent> events)
{
    std::vector<CalendarEvent> result;
    std::map<std::tuple<int, int, int, int, int, std::wstring>, size_t> positions;
    for (auto& event : events)
    {
        const auto key = std::tuple{
            event.startYear, event.startMonth, event.startDay,
            event.startHour, event.startMinute, NormalizedIdentityTitle(event.title)};
        const auto found = positions.find(key);
        if (found == positions.end())
        {
            positions.emplace(key, result.size());
            result.push_back(std::move(event));
            continue;
        }

        auto& existing = result[found->second];
        const long long existingSpan = EventSpanDays(existing);
        const long long candidateSpan = EventSpanDays(event);
        if (candidateSpan > existingSpan ||
            (candidateSpan == existingSpan &&
             HasScheduleMarker(event.title) && !HasScheduleMarker(existing.title)))
        {
            existing = std::move(event);
        }
    }
    return result;
}

CalendarEvent FromSystemAppointment(
    const winrt::Windows::ApplicationModel::Appointments::Appointment& appointment)
{
    const auto toLocalParts = [](winrt::Windows::Foundation::DateTime value)
    {
        const std::time_t timestamp = winrt::clock::to_time_t(value);
        std::tm local{};
        localtime_s(&local, &timestamp);
        return local;
    };

    const auto start = appointment.StartTime();
    const auto end = winrt::Windows::Foundation::DateTime{
        start.time_since_epoch() + appointment.Duration()};
    const std::tm startParts = toLocalParts(start);
    const std::tm endParts = toLocalParts(end);

    CalendarEvent event;
    event.title = appointment.Subject().c_str();
    if (event.title.empty()) event.title = L"(无标题)";
    event.startYear = startParts.tm_year + 1900;
    event.startMonth = startParts.tm_mon + 1;
    event.startDay = startParts.tm_mday;
    event.startHour = startParts.tm_hour;
    event.startMinute = startParts.tm_min;
    event.endYear = endParts.tm_year + 1900;
    event.endMonth = endParts.tm_mon + 1;
    event.endDay = endParts.tm_mday;
    event.allDay = appointment.AllDay();
    return event;
}

int DaysInMonth(int year, int month)
{
    static constexpr std::array days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month != 2)
        return days[static_cast<size_t>(month - 1)];
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return leap ? 29 : 28;
}

winrt::Windows::Foundation::DateTime LocalMonthStart(int year, int month)
{
    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(year);
    local.wMonth = static_cast<WORD>(month);
    local.wDay = 1;
    SYSTEMTIME utc{};
    FILETIME fileTime{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) ||
        !SystemTimeToFileTime(&utc, &fileTime))
    {
        return winrt::clock::now();
    }

    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return winrt::clock::from_file_time(winrt::file_time{value.QuadPart});
}
}

namespace wincal
{
void CalendarData::Load()
{
    std::vector<CalendarEvent> loadedIcsEvents;
    std::vector<Subscription> loadedSubscriptions;
    bool loadedUsesIcs{};
    bool loadedUsesSystemCalendar{true};
    int loadedRefreshMinutes = 30;
    int loadedUpcomingDays = 3;
    try
    {
        const auto localAppData = LocalAppData();
        if (!localAppData.empty())
        {
            const std::string settings = ReadFile(localAppData / L"miniCal" / L"settings.json");
            const std::string source = JsonString(settings, "DataSource");
            loadedUsesIcs = source == "IcsUrl" || source == "Both";
            loadedUsesSystemCalendar = source.empty() || source == "SystemCalendar" || source == "Both";
            loadedUpcomingDays = std::clamp(JsonInteger(settings, "UpcomingDays", 3), 1, 30);
            loadedRefreshMinutes = std::clamp(JsonInteger(settings, "IcsRefreshMinutes", 30), 1, 1440);
            if (loadedUsesIcs)
            {
                const auto cacheDirectory = localAppData / L"WinCal" / L"cache";
                std::set<std::filesystem::path> cacheFiles;
                for (const auto& url : JsonStringArray(settings, "IcsUrls"))
                {
                    const std::wstring hash = Sha256Prefix(url);
                    const std::wstring wideUrl = Utf8ToWide(url);
                    if (!hash.empty() && !wideUrl.empty())
                    {
                        const auto path = cacheDirectory / (hash + L".ics");
                        loadedSubscriptions.push_back({wideUrl, path});
                        if (std::filesystem::exists(path)) cacheFiles.insert(path);
                    }
                }

                // 仅当设置无法映射 URL 时回退到旧缓存，避免新订阅误读已删除订阅的数据。
                if (loadedSubscriptions.empty() && std::filesystem::exists(cacheDirectory))
                {
                    for (const auto& item : std::filesystem::directory_iterator(cacheDirectory))
                    {
                        if (item.is_regular_file() && item.path().extension() == L".ics")
                            cacheFiles.insert(item.path());
                    }
                }

                for (const auto& path : cacheFiles)
                {
                    auto parsed = ParseIcs(ReadFile(path));
                    loadedIcsEvents.insert(
                        loadedIcsEvents.end(),
                        std::make_move_iterator(parsed.begin()),
                        std::make_move_iterator(parsed.end()));
                }
            }
        }

    }
    catch (...)
    {
        loadedIcsEvents.clear();
        loadedSubscriptions.clear();
    }

    loadedIcsEvents = DeduplicateEvents(std::move(loadedIcsEvents));

    const std::lock_guard lock(mutex_);
    icsEvents_ = std::move(loadedIcsEvents);
    subscriptions_ = std::move(loadedSubscriptions);
    usesIcs_ = loadedUsesIcs;
    if (!loadedUsesSystemCalendar)
    {
        systemEvents_.clear();
        systemEventsByMonth_.clear();
        systemState_ = SystemCalendarState::Disabled;
    }
    else if (!usesSystemCalendar_)
    {
        systemState_ = SystemCalendarState::Loading;
    }
    usesSystemCalendar_ = loadedUsesSystemCalendar;
    refreshMinutes_ = loadedRefreshMinutes;
    upcomingDays_ = loadedUpcomingDays;
    RebuildEventsLocked();
}

bool CalendarData::RefreshFromNetwork(std::stop_token stopToken)
{
    std::vector<Subscription> subscriptions;
    int refreshMinutes{};
    {
        const std::lock_guard lock(mutex_);
        subscriptions = subscriptions_;
        refreshMinutes = refreshMinutes_;
    }

    bool updated{};
    for (const auto& subscription : subscriptions)
    {
        if (stopToken.stop_requested())
            break;

        std::error_code error;
        const auto modified = std::filesystem::last_write_time(subscription.cachePath, error);
        if (!error)
        {
            const auto age = decltype(modified)::clock::now() - modified;
            if (age >= decltype(age)::zero() && age < std::chrono::minutes(refreshMinutes))
                continue;
        }

        const std::string content = DownloadUrl(subscription.url, stopToken);
        if (stopToken.stop_requested())
            break;
        if (!IsValidIcs(content))
            continue;

        // 完整下载并验证后才替换旧缓存，失败时继续使用原文件。
        if (ReplaceFileAtomically(subscription.cachePath, content))
            updated = true;
    }

    if (updated && !stopToken.stop_requested())
        Load();
    return updated;
}

bool CalendarData::RefreshSystemCalendar(int year, int month, std::stop_token stopToken)
{
    if (year < 1601 || month < 1 || month > 12)
        return false;

    const auto key = std::pair{year, month};
    {
        const std::lock_guard lock(mutex_);
        if (!usesSystemCalendar_)
            return false;
        if (systemEventsByMonth_.contains(key))
            return false;
        systemState_ = SystemCalendarState::Loading;
    }

    std::vector<CalendarEvent> loadedEvents;
    bool apartmentInitialized{};
    bool available{};
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
        using namespace winrt::Windows::ApplicationModel::Appointments;

        const auto store = AppointmentManager::RequestStoreAsync(
            AppointmentStoreAccessType::AllCalendarsReadOnly).get();
        if (store && !stopToken.stop_requested())
        {
            FindAppointmentsOptions options;
            options.MaxCount(1000);
            const auto rangeStart = LocalMonthStart(year, month) - std::chrono::hours(24 * 7);
            const auto rangeLength = std::chrono::duration_cast<
                winrt::Windows::Foundation::TimeSpan>(
                    std::chrono::hours(24 * (DaysInMonth(year, month) + 14)));
            const auto appointments = store.FindAppointmentsAsync(
                rangeStart, rangeLength, options).get();

            loadedEvents.reserve(appointments.Size());
            for (const auto& appointment : appointments)
            {
                if (stopToken.stop_requested())
                    break;
                loadedEvents.push_back(FromSystemAppointment(appointment));
            }
            available = !stopToken.stop_requested();
        }
    }
    catch (...)
    {
        available = false;
    }
    if (apartmentInitialized)
        winrt::uninit_apartment();

    if (stopToken.stop_requested())
        return false;

    const std::lock_guard lock(mutex_);
    if (available)
    {
        systemEventsByMonth_[key] = std::move(loadedEvents);
        systemState_ = SystemCalendarState::Available;
        RebuildEventsLocked();
    }
    else
    {
        // 权限拒绝或 API 不可用时保留上一次成功结果。
        systemState_ = SystemCalendarState::Unavailable;
    }
    return true;
}

void CalendarData::RebuildEventsLocked()
{
    systemEvents_.clear();
    std::set<std::tuple<
        int, int, int, int, int, int, int, int, bool, std::wstring>> uniqueSystemEvents;
    for (const auto& [key, monthlyEvents] : systemEventsByMonth_)
    {
        static_cast<void>(key);
        for (const auto& event : monthlyEvents)
        {
            const auto identity = std::tuple{
                event.startYear, event.startMonth, event.startDay,
                event.startHour, event.startMinute,
                event.endYear, event.endMonth, event.endDay,
                event.allDay, event.title};
            if (uniqueSystemEvents.insert(identity).second)
                systemEvents_.push_back(event);
        }
    }

    events_.clear();
    if (usesIcs_)
        events_.insert(events_.end(), icsEvents_.begin(), icsEvents_.end());
    if (usesSystemCalendar_)
        events_.insert(events_.end(), systemEvents_.begin(), systemEvents_.end());

    events_ = DeduplicateEvents(std::move(events_));

    std::sort(events_.begin(), events_.end(), [](const auto& left, const auto& right)
    {
        return std::tie(
                   left.startYear, left.startMonth, left.startDay,
                   left.startHour, left.startMinute, left.title) <
               std::tie(
                   right.startYear, right.startMonth, right.startDay,
                   right.startHour, right.startMinute, right.title);
    });
}

bool CalendarData::HasEvents(int year, int month, int day) const
{
    const std::lock_guard lock(mutex_);
    return std::any_of(events_.begin(), events_.end(), [=](const auto& event)
    {
        return OccursOn(event, year, month, day);
    });
}

size_t CalendarData::EventCountForDate(int year, int month, int day) const
{
    const std::lock_guard lock(mutex_);
    return static_cast<size_t>(std::count_if(events_.begin(), events_.end(), [=](const auto& event)
    {
        return OccursOn(event, year, month, day);
    }));
}

std::vector<CalendarEvent> CalendarData::EventsForDate(int year, int month, int day) const
{
    const std::lock_guard lock(mutex_);
    std::vector<CalendarEvent> result;
    std::copy_if(events_.begin(), events_.end(), std::back_inserter(result), [=](const auto& event)
    {
        return OccursOn(event, year, month, day);
    });
    return result;
}

ScheduleLabel CalendarData::ScheduleForDate(int year, int month, int day) const
{
    const std::lock_guard lock(mutex_);
    bool rest{};
    for (const auto& event : events_)
    {
        if (!OccursOn(event, year, month, day))
            continue;
        if ((Contains(event.title, L"（班）") || Contains(event.title, L"(班)") ||
             (Contains(event.title, L" 补班 第") && Contains(event.title, L"/共"))))
            return ScheduleLabel::Work;
        if (Contains(event.title, L"（休）") || Contains(event.title, L"(休)") ||
            (Contains(event.title, L" 假期 第") && Contains(event.title, L"/共")))
            rest = true;
    }
    return rest ? ScheduleLabel::Rest : ScheduleLabel::None;
}

std::vector<std::wstring> CalendarData::UpcomingLines(
    int year, int month, int day, size_t maximumLines) const
{
    const std::lock_guard lock(mutex_);
    std::vector<std::wstring> lines;
    std::set<std::pair<long long, std::wstring>> seen;
    const long long today = DayNumber(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const long long lastDay = today + upcomingDays_;

    for (const auto& event : events_)
    {
        const long long start = DayNumber(
            event.startYear, static_cast<unsigned>(event.startMonth), static_cast<unsigned>(event.startDay));
        if (start < today || start > lastDay || !seen.emplace(start, event.title).second)
            continue;

        std::wstring prefix;
        if (start == today) prefix = L"今天";
        else if (start == today + 1) prefix = L"明天";
        else prefix = std::to_wstring(event.startMonth) + L"月" + std::to_wstring(event.startDay) + L"日";

        lines.push_back(prefix + L"  " + event.title);
        if (lines.size() >= maximumLines)
            break;
    }
    return lines;
}

size_t CalendarData::EventCount() const
{
    const std::lock_guard lock(mutex_);
    return events_.size();
}

size_t CalendarData::IcsEventCount() const
{
    const std::lock_guard lock(mutex_);
    return icsEvents_.size();
}

size_t CalendarData::SystemEventCount() const
{
    const std::lock_guard lock(mutex_);
    return systemEvents_.size();
}

size_t CalendarData::SystemLoadedMonthCount() const
{
    const std::lock_guard lock(mutex_);
    return systemEventsByMonth_.size();
}

SystemCalendarState CalendarData::SystemState() const
{
    const std::lock_guard lock(mutex_);
    return systemState_;
}

bool CalendarData::UsesSystemCalendar() const
{
    const std::lock_guard lock(mutex_);
    return usesSystemCalendar_;
}
}
