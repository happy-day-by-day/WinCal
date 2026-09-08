#include "app_settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace
{
std::filesystem::path LocalAppData()
{
    PWSTR value{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value)))
        return {};
    std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return stream ? std::string(std::istreambuf_iterator<char>(stream), {}) : std::string{};
}

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

size_t FindValue(std::string_view json, std::string_view key)
{
    const auto quoted = "\"" + std::string(key) + "\"";
    const auto keyPos = json.find(quoted);
    if (keyPos == std::string_view::npos) return keyPos;
    const auto colon = json.find(':', keyPos + quoted.size());
    return colon == std::string_view::npos ? colon : json.find_first_not_of(" \t\r\n", colon + 1);
}

int HexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void AppendUtf8(std::string& output, unsigned codepoint)
{
    if (codepoint <= 0x7F) output.push_back(static_cast<char>(codepoint));
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

std::string DecodeJsonString(std::string_view value)
{
    std::string result;
    for (size_t index = 0; index < value.size(); ++index)
    {
        const char current = value[index];
        if (current != '\\' || index + 1 >= value.size()) { result += current; continue; }
        const char escaped = value[++index];
        switch (escaped)
        {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u':
        {
            if (index + 4 >= value.size()) break;
            unsigned codepoint{};
            bool valid = true;
            for (int digit = 0; digit < 4; ++digit)
            {
                const int valueDigit = HexDigit(value[index + 1 + digit]);
                if (valueDigit < 0) { valid = false; break; }
                codepoint = codepoint * 16 + static_cast<unsigned>(valueDigit);
            }
            if (valid) { AppendUtf8(result, codepoint); index += 4; }
            break;
        }
        default: result += escaped; break;
        }
    }
    return result;
}

std::string StringValue(std::string_view json, std::string_view key, std::string fallback = {})
{
    const auto start = FindValue(json, key);
    if (start == std::string_view::npos || json[start] != '"') return fallback;
    bool escaped{};
    for (size_t i = start + 1; i < json.size(); ++i)
    {
        const char c = json[i];
        if (!escaped && c == '"') return DecodeJsonString(json.substr(start + 1, i - start - 1));
        if (!escaped && c == '\\') { escaped = true; continue; }
        if (escaped) escaped = false;
    }
    return fallback;
}

int IntegerValue(std::string_view json, std::string_view key, int fallback)
{
    const auto start = FindValue(json, key);
    if (start == std::string_view::npos) return fallback;
    size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) ++end;
    try { return std::stoi(std::string(json.substr(start, end - start))); } catch (...) { return fallback; }
}

bool BoolValue(std::string_view json, std::string_view key, bool fallback)
{
    const auto start = FindValue(json, key);
    if (start == std::string_view::npos) return fallback;
    if (json.substr(start, 4) == "true") return true;
    if (json.substr(start, 5) == "false") return false;
    return fallback;
}

std::vector<std::wstring> StringArray(std::string_view json, std::string_view key)
{
    std::vector<std::wstring> values;
    auto position = FindValue(json, key);
    if (position == std::string_view::npos || json[position] != '[') return values;
    for (++position; position < json.size(); )
    {
        position = json.find_first_not_of(" \t\r\n,", position);
        if (position == std::string_view::npos || json[position] == ']') break;
        if (json[position] != '"') break;
        const auto start = ++position;
        bool escaped{};
        for (; position < json.size(); ++position)
        {
            if (!escaped && json[position] == '"')
            {
                values.push_back(Utf8ToWide(DecodeJsonString(json.substr(start, position - start))));
                ++position;
                break;
            }
            escaped = !escaped && json[position] == '\\';
            if (json[position] != '\\') escaped = false;
        }
    }
    return values;
}

std::string EscapeJson(std::wstring_view value)
{
    std::string output;
    for (const char c : WideToUtf8(value))
    {
        switch (c)
        {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output += c; break;
        }
    }
    return output;
}

void AppendArray(std::string& json, std::string_view name, const std::vector<std::wstring>& values)
{
    json += "  \"" + std::string(name) + "\": [";
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i) json += ", ";
        json += "\"" + EscapeJson(values[i]) + "\"";
    }
    json += "],\n";
}
}

namespace wincal
{
std::filesystem::path SettingsStore::SettingsPath()
{
    const auto base = LocalAppData();
    return base.empty() ? std::filesystem::path{} : base / L"miniCal" / L"settings.json";
}

std::filesystem::path SettingsStore::CacheDirectory()
{
    const auto base = LocalAppData();
    return base.empty() ? std::filesystem::path{} : base / L"WinCal" / L"cache";
}

AppSettings SettingsStore::Load()
{
    AppSettings settings;
    const auto content = ReadFile(SettingsPath());
    if (content.empty()) return settings;
    settings.themeMode = Utf8ToWide(StringValue(content, "ThemeMode", "FollowSystem"));
    settings.fontSizeOffset = std::clamp(IntegerValue(content, "FontSizeOffset", 0), -2, 10);
    settings.autoStartup = BoolValue(content, "AutoStartup", false);
    settings.dataSource = Utf8ToWide(StringValue(content, "DataSource", "SystemCalendar"));
    settings.icsUrls = StringArray(content, "IcsUrls");
    settings.icsAliases = StringArray(content, "IcsAliases");
    settings.icsRefreshMinutes = std::clamp(IntegerValue(content, "IcsRefreshMinutes", 30), 1, 1440);
    settings.upcomingDays = std::clamp(IntegerValue(content, "UpcomingDays", 3), 1, 30);
    settings.weekStartDay = Utf8ToWide(StringValue(content, "WeekStartDay", "Sunday"));
    return settings;
}

bool SettingsStore::Save(const AppSettings& settings)
{
    const auto path = SettingsPath();
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::string json = "{\n";
    json += "  \"ThemeMode\": \"" + EscapeJson(settings.themeMode) + "\",\n";
    json += "  \"FontSizeOffset\": " + std::to_string(std::clamp(settings.fontSizeOffset, -2, 10)) + ",\n";
    json += "  \"AutoStartup\": " + std::string(settings.autoStartup ? "true" : "false") + ",\n";
    json += "  \"DataSource\": \"" + EscapeJson(settings.dataSource) + "\",\n";
    AppendArray(json, "IcsUrls", settings.icsUrls);
    AppendArray(json, "IcsAliases", settings.icsAliases);
    json += "  \"IcsRefreshMinutes\": " + std::to_string(settings.icsRefreshMinutes) + ",\n";
    json += "  \"UpcomingDays\": " + std::to_string(settings.upcomingDays) + ",\n";
    json += "  \"WeekStartDay\": \"" + EscapeJson(settings.weekStartDay) + "\"\n}\n";
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(json.data(), static_cast<std::streamsize>(json.size()));
    return static_cast<bool>(stream);
}
}
