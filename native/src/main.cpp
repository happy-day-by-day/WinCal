#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <imm.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "calendar_data.h"
#include "app_settings.h"
#include "lunar_calendar.h"
#include "../resources/resource.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kWindowClass[] = L"WinCalNativePopup";
constexpr wchar_t kSettingsWindowClass[] = L"WinCalNativeSettings";
constexpr wchar_t kDetailWindowClass[] = L"WinCalNativeEventDetails";
constexpr wchar_t kWindowTitle[] = L"WinCal";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowPopupMessage = WM_APP + 2;
constexpr UINT kScheduleOutsideHideMessage = WM_APP + 3;
constexpr UINT kCalendarDataUpdatedMessage = WM_APP + 4;
constexpr UINT kSettingsChangedMessage = WM_APP + 5;
constexpr UINT kShowSettingsMessage = WM_APP + 6;
constexpr UINT kAnimationTimer = 1;
constexpr UINT kOutsideClickTimer = 2;
constexpr DWORD kEventObjectUncloak = 0x8018;
constexpr UINT kMenuSettings = 1001;
constexpr UINT kMenuExit = 1002;
constexpr int kLogicalWidth = 430;
constexpr int kLogicalHeight = 650;
constexpr int kSettingsLogicalWidth = 514;
constexpr int kSettingsLogicalHeight = 592;
constexpr int kDetailLogicalWidth = 360;
constexpr int kDetailLogicalHeight = 286;
constexpr int kHeaderTop = 22;
constexpr int kWeekTop = 96;
constexpr int kGridTop = 124;
constexpr int kCellWidth = 58;
constexpr int kCellHeight = 49;
constexpr int kGridLeft = 12;
constexpr int kScheduleTop = 432;
constexpr int kScheduleBottom = 630;
constexpr int kScheduleFirstRow = 480;
constexpr int kScheduleRowHeight = 27;
constexpr int kVisibleScheduleRows = 5;
constexpr int kSettingTheme = 2001;
constexpr int kSettingFontOffset = 2002;
constexpr int kSettingFontMinus = 2013;
constexpr int kSettingFontPlus = 2014;
constexpr int kSettingThemeLight = 2015;
constexpr int kSettingThemeDark = 2016;
constexpr int kSettingPageAppearance = 2020;
constexpr int kSettingPageSource = 2021;
constexpr int kSettingPageGeneral = 2022;
constexpr int kSettingAutoStartup = 2003;
constexpr int kSettingDataSource = 2004;
constexpr int kSettingIcsUrls = 2005;
constexpr int kSettingRefresh = 2006;
constexpr int kSettingWeekStart = 2007;
constexpr int kSettingSave = 2010;
constexpr int kSettingDefaults = 2011;
constexpr int kSettingCancel = 2012;

struct Date
{
    int year{};
    int month{};
    int day{};

    auto operator<=>(const Date&) const = default;
};

enum class HideReason : ULONG_PTR
{
    None,
    Tray,
    OutsideClick,
    ContextMenu,
    EscapeKey,
};

enum class TaskbarEdge
{
    Unknown,
    Left,
    Top,
    Right,
    Bottom,
};

struct Theme
{
    D2D1_COLOR_F background;
    D2D1_COLOR_F card;
    D2D1_COLOR_F primary;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F muted;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F separator;
    D2D1_COLOR_F eventDot;
    D2D1_COLOR_F restBadgeBackground;
    D2D1_COLOR_F restBadgeForeground;
    D2D1_COLOR_F workBadgeBackground;
    D2D1_COLOR_F workBadgeForeground;
};

struct SettingsPalette
{
    COLORREF background;
    COLORREF card;
    COLORREF input;
    COLORREF primary;
    COLORREF secondary;
    COLORREF muted;
    COLORREF accent;
    COLORREF border;
};

long long DaysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468;
}

Date CivilFromDays(long long days)
{
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
    return {year, static_cast<int>(month), static_cast<int>(day)};
}

Date Today()
{
    SYSTEMTIME value{};
    GetLocalTime(&value);
    return {value.wYear, value.wMonth, value.wDay};
}

std::wstring EventTimeText(const wincal::CalendarEvent& event, const Date& date)
{
    if (event.allDay)
        return L"全天";
    if (event.startYear != date.year || event.startMonth != date.month || event.startDay != date.day)
    {
        return std::to_wstring(event.startMonth) + L"/" + std::to_wstring(event.startDay) + L" " +
               std::to_wstring(event.startHour) + L":" +
               (event.startMinute < 10 ? L"0" : L"") + std::to_wstring(event.startMinute);
    }
    wchar_t time[8]{};
    swprintf_s(time, L"%02d:%02d", event.startHour, event.startMinute);
    return time;
}

std::wstring ReadControlText(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0)
        GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::vector<std::wstring> LinesFromText(std::wstring_view value)
{
    std::vector<std::wstring> lines;
    size_t start{};
    while (start < value.size())
    {
        const size_t end = value.find_first_of(L"\r\n", start);
        std::wstring line(value.substr(start, end == std::wstring_view::npos ? end : end - start));
        const auto first = line.find_first_not_of(L" \t");
        if (first != std::wstring::npos)
        {
            line.erase(0, first);
            const auto last = line.find_last_not_of(L" \t");
            line.erase(last + 1);
            if (line.rfind(L"http://", 0) == 0 || line.rfind(L"https://", 0) == 0)
                lines.push_back(std::move(line));
        }
        if (end == std::wstring_view::npos) break;
        start = end + 1;
        if (start < value.size() && value[end] == L'\r' && value[start] == L'\n') ++start;
    }
    return lines;
}

bool IsDarkMode()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    const auto status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    return status == ERROR_SUCCESS && value == 0;
}

Theme GetTheme(bool dark)
{
    if (dark)
    {
        return {
            D2D1::ColorF(0x17191D), D2D1::ColorF(0x202329), D2D1::ColorF(0xF4F6F8),
            D2D1::ColorF(0xABB2BD), D2D1::ColorF(0x6F7782), D2D1::ColorF(0x65A5FF),
            D2D1::ColorF(0x2A3039), D2D1::ColorF(0x31363E), D2D1::ColorF(0x4CC2FF),
            D2D1::ColorF(0x294735), D2D1::ColorF(0xA9D6B3),
            D2D1::ColorF(0x55491F), D2D1::ColorF(0xF2D46B)};
    }

    return {
        D2D1::ColorF(0xF8F9FB), D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0x20242A),
        D2D1::ColorF(0x6C737F), D2D1::ColorF(0xA1A7B0), D2D1::ColorF(0x3478F6),
        D2D1::ColorF(0xEDF3FF), D2D1::ColorF(0xE7E9ED), D2D1::ColorF(0x3478F6),
        D2D1::ColorF(0xDDEBE0), D2D1::ColorF(0x2F6840),
        D2D1::ColorF(0xFFF0B8), D2D1::ColorF(0x806000)};
}

class App
{
public:
#ifdef WINCAL_SETTINGS_PREVIEW
    // Test-only: real controls, no visible window, user settings or network.
    bool RenderSettingsPreview(HINSTANCE instance, int dpi, int page, bool dark, int offset, const wchar_t* path)
    {
        instance_ = instance;
        settingsPreviewDpi_ = dpi;
        settingsDraft_.themeMode = dark ? L"Dark" : L"Light";
        settingsDraft_.fontSizeOffset = offset;
        settingsDraft_.dataSource = L"IcsUrl";
        settingsDraft_.icsUrls = {L"https://example.com/calendar.ics"};
        if (!RegisterSettingsWindowClass()) return false;
        HWND window = CreateWindowExW(0, kSettingsWindowClass, L"WinCal Settings Preview",
            WS_POPUP, 0, 0, MulDiv(kSettingsLogicalWidth, dpi, 96), MulDiv(kSettingsLogicalHeight, dpi, 96), nullptr, nullptr, instance, this);
        if (!window) return false;
        const auto original = settingsDraft_;
        const auto originalRuntime = settings_;
        settingsBeforeEdit_ = settings_;
        settingsLivePreview_ = true;
        const auto click = [&](int id) { HandleSettingsMessage(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0); };
        bool controlsOk = true;
        SetDlgItemInt(window, kSettingFontOffset, static_cast<UINT>(-2), TRUE);
        click(kSettingFontMinus);
        controlsOk &= static_cast<int>(GetDlgItemInt(window, kSettingFontOffset, nullptr, TRUE)) == -2;
        click(kSettingFontPlus);
        controlsOk &= static_cast<int>(GetDlgItemInt(window, kSettingFontOffset, nullptr, TRUE)) == -1;
        SetDlgItemInt(window, kSettingFontOffset, 10, TRUE);
        click(kSettingFontPlus);
        controlsOk &= GetDlgItemInt(window, kSettingFontOffset, nullptr, TRUE) == 10;
        controlsOk &= settings_.fontSizeOffset == 10;
        click(kSettingThemeDark);
        controlsOk &= settingsWindowDark_;
        controlsOk &= settings_.themeMode == L"Dark";
        click(kSettingThemeLight);
        controlsOk &= !settingsWindowDark_;
        click(kSettingPageSource);
        controlsOk &= settingsPage_ == 1;
        SetSettingSelection(window, kSettingDataSource, 0);
        UpdateSettingsSourceControls(window);
        controlsOk &= !IsWindowEnabled(GetDlgItem(window, kSettingIcsUrls));
        SetSettingSelection(window, kSettingDataSource, 2);
        UpdateSettingsSourceControls(window);
        controlsOk &= IsWindowEnabled(GetDlgItem(window, kSettingIcsUrls)) != 0;
        click(kSettingPageGeneral);
        const int before = SettingSelection(window, kSettingAutoStartup);
        click(kSettingAutoStartup);
        controlsOk &= SettingSelection(window, kSettingAutoStartup) != before;
        click(kSettingDefaults);
        controlsOk &= GetDlgItemInt(window, kSettingFontOffset, nullptr, TRUE) == 0;
        SetSettingSelection(window, kSettingWeekStart, 1);
        ApplyLiveSettingsPreview(window);
        controlsOk &= settings_.weekStartDay == L"Monday";
        FinishLiveSettingsPreview();
        controlsOk &= settings_.themeMode == originalRuntime.themeMode &&
                      settings_.fontSizeOffset == originalRuntime.fontSizeOffset &&
                      settings_.weekStartDay == originalRuntime.weekStartDay;
        settingsBeforeEdit_ = settings_;
        settingsLivePreview_ = true;
        settingsSaveAccepted_ = true;
        settingsDraft_ = original;
        FinishLiveSettingsPreview();
        controlsOk &= settings_.themeMode == original.themeMode &&
                      settings_.fontSizeOffset == original.fontSizeOffset &&
                      settings_.weekStartDay == original.weekStartDay;
        settingsSaveAccepted_ = false;
        settingsDraft_ = original;
        PopulateSettingsControls(window);
        UpdateSettingsFontControls(window);
        ApplySettingsWindowAppearance(window);
        SelectSettingsPage(window, page);
        for (const auto& control : settingsControls_)
            controlsOk &= control.x >= 0 && control.y >= 0 &&
                          control.x + control.width <= kSettingsLogicalWidth && control.y + control.height <= kSettingsLogicalHeight;
        // Even at a small viewport, the bottom-right save action must be reachable.
        SetWindowPos(window, nullptr, 0, 0, SettingsPx(360), SettingsPx(400), SWP_NOZORDER | SWP_NOMOVE);
        settingsScrollX_ = SettingsPx(kSettingsLogicalWidth);
        settingsScrollY_ = SettingsPx(kSettingsLogicalHeight);
        LayoutSettingsViewport(window);
        RECT smallClient{};
        GetClientRect(window, &smallClient);
        controlsOk &= settingsScrollX_ > 0 && settingsScrollY_ > 0;
        controlsOk &= SettingsPx(486) - settingsScrollX_ <= smallClient.right;
        controlsOk &= SettingsPx(568) - settingsScrollY_ <= smallClient.bottom;
        SetWindowPos(window, nullptr, 0, 0, SettingsPx(kSettingsLogicalWidth), SettingsPx(kSettingsLogicalHeight), SWP_NOZORDER | SWP_NOMOVE);
        settingsScrollX_ = settingsScrollY_ = 0;
        LayoutSettingsViewport(window);
        RECT restoredClient{};
        GetClientRect(window, &restoredClient);
        controlsOk &= restoredClient.right == SettingsPx(kSettingsLogicalWidth) && restoredClient.bottom == SettingsPx(kSettingsLogicalHeight);
        if (!controlsOk) { DestroyWindow(window); return false; }
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = SettingsPx(kSettingsLogicalWidth);
        info.bmiHeader.biHeight = SettingsPx(kSettingsLogicalHeight);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* pixels{};
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) { DeleteDC(dc); DestroyWindow(window); return false; }
        const auto previous = SelectObject(dc, bitmap);
        RenderSettingsWindow(window, dc);
        for (const auto& control : settingsControls_)
        {
            if (control.page >= 0 && control.page != page) continue;
            const int saved = SaveDC(dc);
            SetViewportOrgEx(dc, SettingsPx(control.x), SettingsPx(control.y), nullptr);
            IntersectClipRect(dc, 0, 0, SettingsPx(control.width), SettingsPx(control.height));
            SendMessageW(control.window, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                         PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
            RestoreDC(dc, saved);
        }
        GdiFlush();
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        const DWORD bytes = info.bmiHeader.biWidth * info.bmiHeader.biHeight * 4;
        header.bfSize = header.bfOffBits + bytes;
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written{};
        const bool ok = file != INVALID_HANDLE_VALUE &&
            WriteFile(file, &header, sizeof(header), &written, nullptr) &&
            WriteFile(file, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr) &&
            WriteFile(file, pixels, bytes, &written, nullptr);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        DestroyWindow(window);
        return ok;
    }
#endif
    int Run(HINSTANCE instance, int, bool openSettings)
    {
        instance_ = instance;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (!RegisterWindowClass() || !RegisterSettingsWindowClass() ||
            !RegisterDetailWindowClass() || !CreateMainWindow())
            return 1;

        calendarData_.Load();
        UpdateCalendarDiagnostics();
        AddTrayIcon();
        StartSystemCalendarInterceptor();
        StartGlobalMouseMonitor();
        StartCalendarRefresh();
        RequestSystemCalendarMonth(displayYear_, displayMonth_);
        if (openSettings)
            PostMessageW(window_, kShowSettingsMessage, 0, 0);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (IsWindow(settingsWindow_) && IsDialogMessageW(settingsWindow_, &message))
                continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        for (auto& thread : calendarRefreshThreads_)
        {
            if (thread.joinable())
            {
                thread.request_stop();
                thread.join();
            }
        }
        if (systemCalendarThread_.joinable())
        {
            systemCalendarThread_.request_stop();
            systemCalendarThread_.join();
        }
        StopGlobalMouseMonitor();
        StopSystemCalendarInterceptor();
        RemoveTrayIcon();
        DiscardDeviceResources();
        CoUninitialize();
        return static_cast<int>(message.wParam);
    }

private:
    void UpdateCalendarDiagnostics() const
    {
        const Date today = Today();
        const Date tomorrow = CivilFromDays(DaysFromCivil(today.year, today.month, today.day) + 1);
        SetPropW(
            window_, L"WinCal.EventCount",
            reinterpret_cast<HANDLE>(calendarData_.EventCount() + 1));
        SetPropW(
            window_, L"WinCal.IcsEventCount",
            reinterpret_cast<HANDLE>(calendarData_.IcsEventCount() + 1));
        SetPropW(
            window_, L"WinCal.SystemEventCount",
            reinterpret_cast<HANDLE>(calendarData_.SystemEventCount() + 1));
        SetPropW(
            window_, L"WinCal.SystemLoadedMonthCount",
            reinterpret_cast<HANDLE>(calendarData_.SystemLoadedMonthCount() + 1));
        SetPropW(
            window_, L"WinCal.SystemCalendarState",
            reinterpret_cast<HANDLE>(
                static_cast<ULONG_PTR>(calendarData_.SystemState()) + 1));
        SetPropW(
            window_, L"WinCal.TodayEventCount",
            reinterpret_cast<HANDLE>(
                calendarData_.EventCountForDate(today.year, today.month, today.day) + 1));
        SetPropW(
            window_, L"WinCal.TomorrowEventCount",
            reinterpret_cast<HANDLE>(
                calendarData_.EventCountForDate(tomorrow.year, tomorrow.month, tomorrow.day) + 1));
    }

    void StartCalendarRefresh()
    {
        for (auto& thread : calendarRefreshThreads_)
        {
            if (thread.joinable())
                thread.request_stop();
        }
        calendarRefreshThreads_.emplace_back([this](std::stop_token stopToken)
        {
            try
            {
                const auto notifyUpdated = [this, &stopToken]()
                {
                    if (!stopToken.stop_requested() && IsWindow(window_))
                        PostMessageW(window_, kCalendarDataUpdatedMessage, 0, 0);
                };

                if (calendarData_.RefreshFromNetwork(stopToken))
                    notifyUpdated();
            }
            catch (...)
            {
                // 后台刷新失败不影响已加载的本地缓存和主消息循环。
            }
        });
    }

    void RequestSystemCalendarMonth(int year, int month)
    {
        if (!calendarData_.UsesSystemCalendar())
            return;

        {
            const std::lock_guard lock(systemCalendarRequestMutex_);
            pendingSystemCalendarMonth_ = std::pair{year, month};
            if (systemCalendarWorkerRunning_)
                return;
            systemCalendarWorkerRunning_ = true;
        }

        if (systemCalendarThread_.joinable())
            systemCalendarThread_.join();
        systemCalendarThread_ = std::jthread([this](std::stop_token stopToken)
        {
            while (!stopToken.stop_requested())
            {
                std::optional<std::pair<int, int>> request;
                {
                    const std::lock_guard lock(systemCalendarRequestMutex_);
                    if (!pendingSystemCalendarMonth_)
                    {
                        systemCalendarWorkerRunning_ = false;
                        return;
                    }
                    request = pendingSystemCalendarMonth_;
                    pendingSystemCalendarMonth_.reset();
                }

                if (calendarData_.RefreshSystemCalendar(
                        request->first, request->second, stopToken) &&
                    !stopToken.stop_requested() && IsWindow(window_))
                {
                    PostMessageW(window_, kCalendarDataUpdatedMessage, 0, 0);
                }
            }

            const std::lock_guard lock(systemCalendarRequestMutex_);
            systemCalendarWorkerRunning_ = false;
        });
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        App* app = nullptr;
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->window_ = window;
        }
        else
        {
            app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        return app ? app->HandleMessage(message, wParam, lParam)
                   : DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        App* app = nullptr;
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->settingsWindow_ = window;
        }
        else
        {
            app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return app ? app->HandleSettingsMessage(window, message, wParam, lParam)
                   : DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK DetailWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        App* app = nullptr;
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->detailWindow_ = window;
        }
        else
        {
            app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return app ? app->HandleDetailMessage(window, message, wParam, lParam)
                   : DefWindowProcW(window, message, wParam, lParam);
    }

    bool RegisterWindowClass() const
    {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClass;
        windowClass.hIconSm = windowClass.hIcon;
        return RegisterClassExW(&windowClass) != 0;
    }

    bool RegisterSettingsWindowClass() const
    {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = SettingsWindowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kSettingsWindowClass;
        windowClass.hIconSm = windowClass.hIcon;
        return RegisterClassExW(&windowClass) != 0;
    }

    bool RegisterDetailWindowClass() const
    {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = DetailWindowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kDetailWindowClass;
        windowClass.hIconSm = windowClass.hIcon;
        return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    bool CreateMainWindow()
    {
        window_ = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kWindowClass,
            kWindowTitle,
            WS_POPUP,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            kLogicalWidth,
            kLogicalHeight,
            nullptr,
            nullptr,
            instance_,
            this);
        if (!window_)
            return false;

        // 日历面板没有文本输入，避免第三方输入法把整套 TSF/插件注入常驻进程。
        ImmAssociateContextEx(window_, nullptr, IACE_IGNORENOCONTEXT);
        dpi_ = GetDpiForWindow(window_);
        UpdateWindowSize();
        settings_ = wincal::SettingsStore::Load();
        if (settings_.autoStartup)
            ApplyAutoStartup(true);
        ApplyDwmAppearance();
        const auto today = Today();
        displayYear_ = today.year;
        displayMonth_ = today.month;
        selected_ = today;
        return true;
    }

    void ApplyDwmAppearance()
    {
        dark_ = settings_.themeMode == L"Dark" ||
                (settings_.themeMode != L"Light" && IsDarkMode());
        fontScale_ = 1.0f + static_cast<float>(std::clamp(settings_.fontSizeOffset, -2, 10)) * 0.06f;
        weekStartsMonday_ = settings_.weekStartDay != L"Sunday";
        const BOOL darkValue = dark_ ? TRUE : FALSE;
        DwmSetWindowAttribute(window_, 20, &darkValue, sizeof(darkValue));
        const DWORD roundPreference = 2;
        DwmSetWindowAttribute(window_, 33, &roundPreference, sizeof(roundPreference));
    }

    float TextScale(float value) const
    {
        return Scale(value * fontScale_);
    }

    // All settings geometry uses DIPs. Fonts and controls are scaled together.
    int SettingsPx(int value) const { return MulDiv(value, settingsDpi_, 96); }

    RECT SettingsRect(int x, int y, int width, int height) const
    {
        return {SettingsPx(x), SettingsPx(y), SettingsPx(x + width), SettingsPx(y + height)};
    }

    static int SettingSelection(HWND window, int id)
    {
        return static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(GetDlgItem(window, id), L"Selection")));
    }

    static std::vector<const wchar_t*> SettingOptions(int id)
    {
        switch (id)
        {
        case kSettingTheme: return {L"跟随系统", L"浅色", L"深色"};
        case kSettingDataSource: return {L"系统日历", L"ICS 订阅", L"系统日历 + ICS"};
        case kSettingRefresh: return {L"10 分钟", L"30 分钟", L"60 分钟", L"120 分钟", L"1 天"};
        case kSettingWeekStart: return {L"周日", L"周一"};
        default: return {};
        }
    }

    void SetSettingSelection(HWND window, int id, int index)
    {
        HWND control = GetDlgItem(window, id);
        SetPropW(control, L"Selection", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(index)));
        const auto options = SettingOptions(id);
        if (id == kSettingTheme)
            SetWindowTextW(control, L"跟随系统");
        else if (!options.empty())
            SetWindowTextW(control, options[std::clamp(index, 0, static_cast<int>(options.size()) - 1)]);
        InvalidateRect(control, nullptr, FALSE);
    }

    static LRESULT CALLBACK SettingsControlProc(HWND window, UINT message, WPARAM wParam,
                                                LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        auto* app = reinterpret_cast<App*>(data);
        if (message == WM_MOUSEMOVE)
        {
            if (app->settingsHotControl_ != window)
            {
                if (app->settingsHotControl_) InvalidateRect(app->settingsHotControl_, nullptr, FALSE);
                app->settingsHotControl_ = window;
                InvalidateRect(window, nullptr, FALSE);
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
            }
        }
        else if (message == WM_MOUSELEAVE)
        {
            if (app->settingsHotControl_ == window) app->settingsHotControl_ = nullptr;
            InvalidateRect(window, nullptr, FALSE);
        }
        else if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
            InvalidateRect(GetParent(window), nullptr, FALSE);
        else if (message == WM_NCDESTROY)
            RemoveWindowSubclass(window, SettingsControlProc, 1);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    HWND AddSettingControl(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
                           int id, int x, int y, int width, int height, int page = -1)
    {
        RECT rect = SettingsRect(x, y, width, height);
        HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(settingsBodyFont_), FALSE);
        SetWindowSubclass(control, SettingsControlProc, 1, reinterpret_cast<DWORD_PTR>(this));
        ImmAssociateContextEx(control, nullptr, IACE_IGNORENOCONTEXT);
        settingsControls_.push_back({control, x, y, width, height, page});
        return control;
    }

    void PopulateSettingsControls(HWND window)
    {
        SetSettingSelection(window, kSettingTheme, settingsDraft_.themeMode == L"Light" ? 1 : settingsDraft_.themeMode == L"Dark" ? 2 : 0);
        SetDlgItemInt(window, kSettingFontOffset, static_cast<UINT>(settingsDraft_.fontSizeOffset), TRUE);
        SetSettingSelection(window, kSettingAutoStartup, settingsDraft_.autoStartup ? 1 : 0);
        SetSettingSelection(window, kSettingDataSource, settingsDraft_.dataSource == L"IcsUrl" ? 1 : settingsDraft_.dataSource == L"Both" ? 2 : 0);
        std::wstring urls;
        for (const auto& url : settingsDraft_.icsUrls)
        {
            if (!urls.empty()) urls += L"\r\n";
            urls += url;
        }
        SetDlgItemTextW(window, kSettingIcsUrls, urls.c_str());
        SetSettingSelection(window, kSettingRefresh,
            settingsDraft_.icsRefreshMinutes == 10 ? 0 : settingsDraft_.icsRefreshMinutes == 60 ? 2 :
            settingsDraft_.icsRefreshMinutes == 120 ? 3 : settingsDraft_.icsRefreshMinutes == 1440 ? 4 : 1);
        SetSettingSelection(window, kSettingWeekStart, settingsDraft_.weekStartDay == L"Monday" ? 1 : 0);
    }

    void SelectSettingsPage(HWND window, int page)
    {
        settingsPage_ = page;
        for (const auto& control : settingsControls_)
            ShowWindow(control.window, control.page < 0 || control.page == page ? SW_SHOW : SW_HIDE);
        UpdateSettingsSourceControls(window);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void LayoutSettingsViewport(HWND window)
    {
        if (settingsLayoutActive_) return;
        settingsLayoutActive_ = true;
        RECT client{};
        GetClientRect(window, &client);
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        const UINT nonClientDpi = GetDpiForWindow(window);
        const int barWidth = GetSystemMetricsForDpi(SM_CXVSCROLL, nonClientDpi);
        const int barHeight = GetSystemMetricsForDpi(SM_CYHSCROLL, nonClientDpi);
        const int availableWidth = client.right + ((style & WS_VSCROLL) ? barWidth : 0);
        const int availableHeight = client.bottom + ((style & WS_HSCROLL) ? barHeight : 0);
        bool needHorizontal = availableWidth < SettingsPx(kSettingsLogicalWidth);
        bool needVertical = availableHeight < SettingsPx(kSettingsLogicalHeight);
        for (int pass = 0; pass < 2; ++pass)
        {
            needHorizontal = availableWidth - (needVertical ? barWidth : 0) < SettingsPx(kSettingsLogicalWidth);
            needVertical = availableHeight - (needHorizontal ? barHeight : 0) < SettingsPx(kSettingsLogicalHeight);
        }
        ShowScrollBar(window, SB_HORZ, needHorizontal);
        ShowScrollBar(window, SB_VERT, needVertical);
        GetClientRect(window, &client);
        SCROLLINFO horizontal{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS,
            0, SettingsPx(kSettingsLogicalWidth) - 1, static_cast<UINT>(client.right), settingsScrollX_};
        SetScrollInfo(window, SB_HORZ, &horizontal, TRUE);
        // Adding one scrollbar reduces the space available to the other.
        GetClientRect(window, &client);
        SCROLLINFO vertical{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS,
            0, SettingsPx(kSettingsLogicalHeight) - 1, static_cast<UINT>(client.bottom), settingsScrollY_};
        SetScrollInfo(window, SB_VERT, &vertical, TRUE);
        GetClientRect(window, &client);
        horizontal.nPage = client.right;
        SetScrollInfo(window, SB_HORZ, &horizontal, TRUE);
        settingsScrollX_ = GetScrollPos(window, SB_HORZ);
        settingsScrollY_ = GetScrollPos(window, SB_VERT);
        for (const auto& control : settingsControls_)
        {
            RECT rect = SettingsRect(control.x, control.y, control.width, control.height);
            SetWindowPos(control.window, nullptr, rect.left - settingsScrollX_, rect.top - settingsScrollY_,
                rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        settingsLayoutActive_ = false;
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void UpdateSettingsSourceControls(HWND window)
    {
        const bool useIcs = SettingSelection(window, kSettingDataSource) != 0;
        EnableWindow(GetDlgItem(window, kSettingIcsUrls), useIcs);
        EnableWindow(GetDlgItem(window, kSettingRefresh), useIcs);
    }

    void CreateSettingsControls(HWND window)
    {
        settingsDpi_ = GetDpiForWindow(window);
#ifdef WINCAL_SETTINGS_PREVIEW
        settingsDpi_ = settingsPreviewDpi_;
#endif
        EnsureSettingsFonts(window);
        const auto button = [&](const wchar_t* text, int id, int x, int y, int w, int h, int page = -1)
        {
            return AddSettingControl(window, L"BUTTON", text, BS_OWNERDRAW, id, x, y, w, h, page);
        };
        button(L"外观", kSettingPageAppearance, 28, 84, 104, 36);
        button(L"日历来源", kSettingPageSource, 140, 84, 112, 36);
        button(L"常规", kSettingPageGeneral, 260, 84, 104, 36);

        button(L"跟随系统", kSettingTheme, 48, 218, 134, 42, 0);
        button(L"浅色", kSettingThemeLight, 190, 218, 134, 42, 0);
        button(L"深色", kSettingThemeDark, 332, 218, 134, 42, 0);
        button(L"−", kSettingFontMinus, 326, 316, 36, 36, 0);
        AddSettingControl(window, L"EDIT", L"0", ES_CENTER | ES_AUTOHSCROLL,
                          kSettingFontOffset, 370, 324, 52, 22, 0);
        button(L"+", kSettingFontPlus, 430, 316, 36, 36, 0);

        button(L"", kSettingDataSource, 48, 208, 254, 38, 1);
        button(L"", kSettingRefresh, 318, 208, 148, 38, 1);
        AddSettingControl(window, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                          kSettingIcsUrls, 60, 318, 394, 98, 1);

        button(L"开机启动", kSettingAutoStartup, 410, 190, 56, 30, 2);
        button(L"", kSettingWeekStart, 266, 270, 200, 38, 2);

        button(L"恢复默认", kSettingDefaults, 28, 532, 104, 36);
        button(L"取消", kSettingCancel, 272, 532, 88, 36);
        button(L"保存设置", kSettingSave, 372, 532, 114, 36);
        PopulateSettingsControls(window);
        UpdateSettingsFontControls(window);
        SelectSettingsPage(window, 0);
    }

    void UpdateSettingsFontControls(HWND window)
    {
        BOOL valid{};
        const int offset = std::clamp(static_cast<int>(GetDlgItemInt(window, kSettingFontOffset, &valid, TRUE)), -2, 10);
        EnableWindow(GetDlgItem(window, kSettingFontMinus), offset > -2);
        EnableWindow(GetDlgItem(window, kSettingFontPlus), offset < 10);
        ApplyLiveSettingsPreview(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    void RefreshCalendarAppearance()
    {
        if (!IsWindow(window_)) return;
        ApplyDwmAppearance();
        DiscardDeviceResources();
        InvalidateRect(window_, nullptr, FALSE);
        if (IsWindow(detailWindow_))
        {
            BOOL darkValue = dark_ ? TRUE : FALSE;
            DwmSetWindowAttribute(detailWindow_, 20, &darkValue, sizeof(darkValue));
            InvalidateRect(detailWindow_, nullptr, FALSE);
        }
    }

    void ApplyLiveSettingsPreview(HWND window)
    {
        if (!settingsLivePreview_) return;
        const int theme = SettingSelection(window, kSettingTheme);
        settings_.themeMode = theme == 1 ? L"Light" : theme == 2 ? L"Dark" : L"FollowSystem";
        BOOL valid{};
        const int offset = static_cast<int>(GetDlgItemInt(window, kSettingFontOffset, &valid, TRUE));
        settings_.fontSizeOffset = std::clamp(valid ? offset : 0, -2, 10);
        settings_.weekStartDay = SettingSelection(window, kSettingWeekStart) == 1 ? L"Monday" : L"Sunday";
        RefreshCalendarAppearance();
        if (IsWindow(window_))
        {
            KillTimer(window_, kOutsideClickTimer);
            ShowWindow(window_, SW_SHOWNOACTIVATE);
        }
    }

    void FinishLiveSettingsPreview()
    {
        if (!settingsLivePreview_) return;
        settingsLivePreview_ = false;
        settings_ = settingsSaveAccepted_ ? settingsDraft_ : settingsBeforeEdit_;
        settingsBeforeEdit_ = {};
        RefreshCalendarAppearance();
    }

    void PositionLivePreview()
    {
        if (!settingsLivePreview_ || !IsWindow(settingsWindow_) || !IsWindow(window_)) return;
        RECT settingsRect{};
        GetWindowRect(settingsWindow_, &settingsRect);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(settingsWindow_, MONITOR_DEFAULTTONEAREST), &monitor);
        const UINT dpi = GetDpiForWindow(settingsWindow_);
        const int width = MulDiv(kLogicalWidth, dpi, 96);
        const int height = MulDiv(kLogicalHeight, dpi, 96);
        const int gap = MulDiv(16, dpi, 96);
        LONG x = settingsRect.right + gap;
        if (x + width > monitor.rcWork.right)
            x = settingsRect.left - width - gap;
        x = std::clamp(x, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
        const LONG y = std::clamp(settingsRect.top, monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
        SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }

    void ApplySettingsWindowAppearance(HWND window)
    {
        const int theme = SettingSelection(window, kSettingTheme);
        settingsWindowDark_ = theme == 2 || (theme != 1 && IsDarkMode());
        const BOOL darkValue = settingsWindowDark_;
        DwmSetWindowAttribute(window, 20, &darkValue, sizeof(darkValue));
        if (settingsInputBrush_) DeleteObject(settingsInputBrush_);
        if (settingsCardBrush_) DeleteObject(settingsCardBrush_);
        const auto palette = GetSettingsPalette();
        settingsInputBrush_ = CreateSolidBrush(palette.input);
        settingsCardBrush_ = CreateSolidBrush(palette.card);
        ApplyLiveSettingsPreview(window);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    SettingsPalette GetSettingsPalette() const
    {
        if (settingsWindowDark_)
            return {RGB(24,25,31), RGB(33,35,43), RGB(40,42,52), RGB(237,238,245),
                    RGB(164,169,186), RGB(114,121,142), RGB(162,155,255), RGB(57,60,75)};
        return {RGB(246,247,251), RGB(255,255,255), RGB(249,250,253), RGB(33,38,57),
                RGB(113,121,144), RGB(157,163,180), RGB(103,91,218), RGB(229,232,242)};
    }

    void EnsureSettingsFonts(HWND)
    {
        if (settingsBodyFont_) return;
        const auto makeFont = [&](int size, int weight)
        {
            return CreateFontW(-SettingsPx(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Microsoft YaHei UI");
        };
        settingsTitleFont_ = makeFont(24, FW_SEMIBOLD);
        settingsBodyFont_ = makeFont(13, FW_NORMAL);
        settingsLabelFont_ = makeFont(15, FW_SEMIBOLD);
        settingsCaptionFont_ = makeFont(11, FW_NORMAL);
    }

    void SettingsBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius = 10) const
    {
        const auto brush = CreateSolidBrush(fill);
        const auto pen = CreatePen(PS_SOLID, std::max(1, SettingsPx(1)), border);
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, SettingsPx(radius), SettingsPx(radius));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void SettingsText(HDC dc, const wchar_t* text, int x, int y, int width, int height,
                      HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) const
    {
        RECT rect = SettingsRect(x, y, width, height);
        const auto oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        ::DrawTextW(dc, text, -1, &rect, flags | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    void PaintSettingsWindow(HWND window)
    {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RenderSettingsWindow(window, dc);
        EndPaint(window, &paint);
    }

    void RenderSettingsWindow(HWND window, HDC dc)
    {
        const auto p = GetSettingsPalette();
        RECT client{};
        GetClientRect(window, &client);
        const auto bg = CreateSolidBrush(p.background);
        FillRect(dc, &client, bg);
        DeleteObject(bg);
        const int savedDc = SaveDC(dc);
        SetViewportOrgEx(dc, -settingsScrollX_, -settingsScrollY_, nullptr);

        SettingsBox(dc, SettingsRect(28, 24, 36, 36), p.accent, p.accent, 12);
        DrawIconEx(dc, SettingsPx(34), SettingsPx(30), LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON)),
                   SettingsPx(24), SettingsPx(24), 0, nullptr, DI_NORMAL);
        SettingsText(dc, L"设置", 78, 20, 180, 32, settingsTitleFont_, p.primary);
        SettingsText(dc, L"让每一天，都按你的习惯呈现。", 78, 54, 360, 18, settingsCaptionFont_, p.secondary);

        SettingsBox(dc, SettingsRect(28, 136, 458, 364), p.card, p.border, 16);
        if (settingsPage_ == 0)
        {
            SettingsText(dc, L"外观与字号", 48, 154, 280, 24, settingsLabelFont_, p.primary);
            SettingsText(dc, L"主题模式", 48, 190, 220, 20, settingsBodyFont_, p.secondary);
            SettingsText(dc, L"文字大小", 48, 306, 220, 24, settingsLabelFont_, p.primary);
            SettingsText(dc, L"仅调整日历中的文字", 48, 334, 230, 20, settingsCaptionFont_, p.secondary);
            SettingsBox(dc, SettingsRect(367, 316, 58, 36), p.input,
                        GetFocus() == GetDlgItem(window, kSettingFontOffset) ? p.accent : p.border, 8);
            SettingsText(dc, L"可调范围 −2 至 +10", 304, 362, 162, 20, settingsCaptionFont_, p.secondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            SettingsBox(dc, SettingsRect(48, 406, 418, 66), p.input, p.input, 12);
            SettingsText(dc, L"在日历窗口中实时预览", 64, 416, 360, 22, settingsBodyFont_, p.primary);
            SettingsText(dc, L"保存以保留修改；取消会恢复原来的效果。", 64, 440, 376, 20, settingsCaptionFont_, p.secondary);
        }
        else if (settingsPage_ == 1)
        {
            const bool useIcs = SettingSelection(window, kSettingDataSource) != 0;
            SettingsText(dc, L"日历与订阅", 48, 154, 280, 24, settingsLabelFont_, p.primary);
            SettingsText(dc, L"日历来源", 48, 182, 240, 20, settingsCaptionFont_, p.secondary);
            SettingsText(dc, L"自动刷新", 318, 182, 148, 20, settingsCaptionFont_, p.secondary);
            SettingsText(dc, L"ICS 订阅链接", 48, 264, 300, 24, settingsLabelFont_, p.primary);
            SettingsText(dc, L"每行一条公开的 .ics 链接", 48, 288, 400, 20, settingsCaptionFont_, p.secondary);
            SettingsBox(dc, SettingsRect(48, 310, 418, 118), p.input,
                        GetFocus() == GetDlgItem(window, kSettingIcsUrls) ? p.accent : p.border, 10);
            SettingsText(dc, useIcs ? L"保存后后台刷新，已有缓存会立即用于展示。" : L"当前使用系统日历，切换来源后可编辑订阅。",
                         48, 444, 418, 26, settingsCaptionFont_, p.secondary);
        }
        else
        {
            SettingsText(dc, L"启动与日历布局", 48, 154, 320, 24, settingsLabelFont_, p.primary);
            SettingsText(dc, L"开机启动", 48, 186, 320, 24, settingsBodyFont_, p.primary);
            SettingsText(dc, L"登录 Windows 后自动运行 WinCal", 48, 212, 338, 20, settingsCaptionFont_, p.secondary);
            SettingsText(dc, L"每周开始", 48, 268, 200, 24, settingsBodyFont_, p.primary);
            SettingsText(dc, L"选择日历每一行的第一天", 48, 294, 210, 20, settingsCaptionFont_, p.secondary);
            SettingsText(dc, L"显示顺序会立即同步到日历窗口。", 48, 446, 418, 24, settingsCaptionFont_, p.secondary);
        }

        RECT line = SettingsRect(28, 516, 458, 1);
        const auto lineBrush = CreateSolidBrush(p.border);
        FillRect(dc, &line, lineBrush);
        DeleteObject(lineBrush);
        RestoreDC(dc, savedDc);
    }

    LRESULT DrawSettingsButton(const DRAWITEMSTRUCT& item) const
    {
        const auto p = GetSettingsPalette();
        const int id = item.CtlID;
        const bool enabled = IsWindowEnabled(item.hwndItem);
        const bool hot = settingsHotControl_ == item.hwndItem;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool tab = id >= kSettingPageAppearance && id <= kSettingPageGeneral;
        const bool theme = id == kSettingTheme || id == kSettingThemeLight || id == kSettingThemeDark;
        const bool toggle = id == kSettingAutoStartup;
        const bool dropdown = !theme && !SettingOptions(id).empty();
        const bool active = (tab && settingsPage_ == id - kSettingPageAppearance) ||
            (theme && SettingSelection(settingsWindow_, kSettingTheme) == (id == kSettingTheme ? 0 : id == kSettingThemeLight ? 1 : 2));
        const bool primary = id == kSettingSave;
        const auto tint = settingsWindowDark_ ? RGB(49,46,71) : RGB(239,237,252);
        const auto fill = primary ? p.accent : active || hot || pressed ? tint : tab ? p.background : p.input;
        const auto background = CreateSolidBrush(tab || id == kSettingDefaults || id == kSettingCancel || primary ? p.background : p.card);
        FillRect(item.hDC, &item.rcItem, background);
        DeleteObject(background);
        if (toggle)
        {
            const bool on = SettingSelection(settingsWindow_, id) != 0;
            RECT track{SettingsPx(8),SettingsPx(4),SettingsPx(52),SettingsPx(26)};
            SettingsBox(item.hDC, track, on ? p.accent : p.muted, on ? p.accent : p.muted, 22);
            const int knob = on ? 33 : 11;
            SettingsBox(item.hDC, {SettingsPx(knob),SettingsPx(7),SettingsPx(knob+16),SettingsPx(23)},
                        RGB(255,255,255), RGB(255,255,255), 16);
        }
        else
        {
            SettingsBox(item.hDC, item.rcItem, fill, primary ? p.accent : active ? p.accent : tab ? fill : p.border, 10);
            wchar_t text[96]{};
            // The first segment keeps its label even when a different theme is selected.
            if (id == kSettingTheme) wcscpy_s(text, L"跟随系统");
            else GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
            RECT rect = item.rcItem;
            if (dropdown) { rect.left += SettingsPx(12); rect.right -= SettingsPx(30); }
            const auto oldFont = SelectObject(item.hDC, settingsBodyFont_);
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, !enabled ? p.muted : primary ?
                         (settingsWindowDark_ ? RGB(24,25,31) : RGB(255,255,255)) : active ? p.accent : p.primary);
            ::DrawTextW(item.hDC, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (dropdown ? DT_LEFT : DT_CENTER));
            SelectObject(item.hDC, oldFont);
            if (dropdown)
            {
                const int x = item.rcItem.right - SettingsPx(18);
                const int y = (item.rcItem.top + item.rcItem.bottom) / 2;
                const auto pen = CreatePen(PS_SOLID, std::max(1, SettingsPx(1)), p.secondary);
                const auto oldPen = SelectObject(item.hDC, pen);
                MoveToEx(item.hDC, x - SettingsPx(4), y - SettingsPx(2), nullptr);
                LineTo(item.hDC, x, y + SettingsPx(2));
                LineTo(item.hDC, x + SettingsPx(4), y - SettingsPx(2));
                SelectObject(item.hDC, oldPen);
                DeleteObject(pen);
            }
        }
        if (item.itemState & ODS_FOCUS)
        {
            RECT focus = item.rcItem;
            InflateRect(&focus, -SettingsPx(3), -SettingsPx(3));
            DrawFocusRect(item.hDC, &focus);
        }
        return TRUE;
    }

    void AddTrayIcon()
    {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        wcscpy_s(tray_.szTip, L"WinCal 原生日历");
        Shell_NotifyIconW(NIM_ADD, &tray_);
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    void RemoveTrayIcon()
    {
        if (tray_.hWnd)
            Shell_NotifyIconW(NIM_DELETE, &tray_);
    }

    void TogglePopup()
    {
        if (IsWindowVisible(window_))
        {
            HidePopup(HideReason::Tray);
            return;
        }

        ShowPopup(false);
    }

    void ShowPopup(bool preferCursorMonitor)
    {
        KillTimer(window_, kOutsideClickTimer);
        if (IsWindowVisible(window_))
        {
            SetForegroundWindow(window_);
            SetFocus(window_);
            return;
        }

        const auto today = Today();
        displayYear_ = today.year;
        displayMonth_ = today.month;
        selected_ = today;
        selectedEventScroll_ = 0;
        hoveredCell_ = -1;
        RequestSystemCalendarMonth(displayYear_, displayMonth_);
        transition_ = 0.0f;
        SetTimer(window_, kAnimationTimer, 16, nullptr);
        PositionNearTray(preferCursorMonitor);
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        SetFocus(window_);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void HidePopup(HideReason reason)
    {
        SetPropW(window_, L"WinCal.LastHideReason", reinterpret_cast<HANDLE>(reason));
        KillTimer(window_, kAnimationTimer);
        KillTimer(window_, kOutsideClickTimer);
        ShowWindow(window_, SW_HIDE);
        CloseDetailWindow();
        hoveredCell_ = -1;
        DiscardDeviceResources();
    }

    void PositionNearTray(bool preferCursorMonitor)
    {
        HMONITOR monitor{};
        POINT cursor{};
        if (preferCursorMonitor && GetCursorPos(&cursor))
        {
            monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        }
        else
        {
            RECT iconRect{};
            NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
            identifier.hWnd = window_;
            identifier.uID = tray_.uID;
            if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &iconRect)))
                monitor = MonitorFromRect(&iconRect, MONITOR_DEFAULTTONEAREST);
            else if (GetCursorPos(&cursor))
                monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            else
                monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        }

        MONITORINFO info{sizeof(info)};
        if (!monitor || !GetMonitorInfoW(monitor, &info))
            return;

        const auto taskbarForMonitor = [monitor]() -> HWND
        {
            const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (primary && MonitorFromWindow(primary, MONITOR_DEFAULTTONULL) == monitor)
                return primary;

            HWND secondary{};
            while ((secondary = FindWindowExW(
                        nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
            {
                if (MonitorFromWindow(secondary, MONITOR_DEFAULTTONULL) == monitor)
                    return secondary;
            }
            return nullptr;
        }();

        TaskbarEdge edge = TaskbarEdge::Unknown;
        RECT taskbarRect{};
        if (taskbarForMonitor && GetWindowRect(taskbarForMonitor, &taskbarRect))
        {
            const int taskbarWidth = taskbarRect.right - taskbarRect.left;
            const int taskbarHeight = taskbarRect.bottom - taskbarRect.top;
            if (taskbarWidth >= taskbarHeight)
            {
                const int topDistance = std::abs(taskbarRect.top - info.rcMonitor.top);
                const int bottomDistance = std::abs(info.rcMonitor.bottom - taskbarRect.bottom);
                edge = topDistance <= bottomDistance ? TaskbarEdge::Top : TaskbarEdge::Bottom;
            }
            else
            {
                const int leftDistance = std::abs(taskbarRect.left - info.rcMonitor.left);
                const int rightDistance = std::abs(info.rcMonitor.right - taskbarRect.right);
                edge = leftDistance <= rightDistance ? TaskbarEdge::Left : TaskbarEdge::Right;
            }
        }
        else
        {
            const std::array insets{
                std::pair{TaskbarEdge::Left, info.rcWork.left - info.rcMonitor.left},
                std::pair{TaskbarEdge::Top, info.rcWork.top - info.rcMonitor.top},
                std::pair{TaskbarEdge::Right, info.rcMonitor.right - info.rcWork.right},
                std::pair{TaskbarEdge::Bottom, info.rcMonitor.bottom - info.rcWork.bottom},
            };
            const auto largest = std::max_element(
                insets.begin(), insets.end(),
                [](const auto& left, const auto& right) { return left.second < right.second; });
            if (largest != insets.end() && largest->second > 0)
                edge = largest->first;
        }

        const UINT taskbarDpi = taskbarForMonitor ? GetDpiForWindow(taskbarForMonitor) : 0;
        const UINT targetDpi = taskbarDpi ? taskbarDpi : (dpi_ ? dpi_ : 96);
        const int width = MulDiv(kLogicalWidth, targetDpi, 96);
        const int height = MulDiv(kLogicalHeight, targetDpi, 96);
        const int taskbarGap = MulDiv(8, targetDpi, 96);
        const int endMargin = MulDiv(12, targetDpi, 96);

        int x{};
        int y{};
        switch (edge)
        {
        case TaskbarEdge::Top:
            x = info.rcWork.right - width - endMargin;
            y = info.rcWork.top + taskbarGap;
            break;
        case TaskbarEdge::Left:
            x = info.rcWork.left + taskbarGap;
            y = info.rcWork.bottom - height - endMargin;
            break;
        case TaskbarEdge::Right:
            x = info.rcWork.right - width - taskbarGap;
            y = info.rcWork.bottom - height - endMargin;
            break;
        case TaskbarEdge::Bottom:
        case TaskbarEdge::Unknown:
            x = info.rcWork.right - width - endMargin;
            y = info.rcWork.bottom - height - taskbarGap;
            break;
        }

        const int maximumX = std::max(info.rcWork.left, info.rcWork.right - width);
        const int maximumY = std::max(info.rcWork.top, info.rcWork.bottom - height);
        x = std::clamp(x, static_cast<int>(info.rcWork.left), maximumX);
        y = std::clamp(y, static_cast<int>(info.rcWork.top), maximumY);

        SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }

    static LRESULT CALLBACK MouseHookProc(int code, WPARAM message, LPARAM data)
    {
        if (code >= HC_ACTION && activeInstance_)
            activeInstance_->HandleGlobalMouse(message, *reinterpret_cast<const MSLLHOOKSTRUCT*>(data));
        return CallNextHookEx(nullptr, code, message, data);
    }

    void StartGlobalMouseMonitor()
    {
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance_, 0);
    }

    void StopGlobalMouseMonitor()
    {
        if (mouseHook_)
        {
            UnhookWindowsHookEx(mouseHook_);
            mouseHook_ = nullptr;
        }
    }

    bool IsPointInsideTrayIcon(POINT point) const
    {
        NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
        identifier.hWnd = window_;
        identifier.uID = tray_.uID;
        RECT iconRect{};
        return SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &iconRect)) &&
               PtInRect(&iconRect, point);
    }

    void HandleGlobalMouse(WPARAM message, const MSLLHOOKSTRUCT& mouse) const
    {
        if (settingsLivePreview_) return;
        if (message != WM_LBUTTONDOWN && message != WM_RBUTTONDOWN && message != WM_MBUTTONDOWN)
            return;
        if (!IsWindowVisible(window_))
            return;

        RECT popupRect{};
        if (GetWindowRect(window_, &popupRect) && PtInRect(&popupRect, mouse.pt))
            return;
        RECT detailRect{};
        if (IsWindow(detailWindow_) && GetWindowRect(detailWindow_, &detailRect) &&
            PtInRect(&detailRect, mouse.pt))
            return;
        if (IsPointInsideTrayIcon(mouse.pt))
            return;

        PostMessageW(window_, kScheduleOutsideHideMessage, 0, 0);
    }

    void BeginTrayClick()
    {
        trayPointerDown_ = true;
        trayClickWasVisible_ = IsWindowVisible(window_) != FALSE;
        KillTimer(window_, kOutsideClickTimer);
    }

    void EndTrayClick()
    {
        const bool shouldClose = trayPointerDown_
            ? trayClickWasVisible_
            : IsWindowVisible(window_) != FALSE;
        trayPointerDown_ = false;
        lastTrayMouseUpTick_ = GetTickCount64();

        if (shouldClose)
            HidePopup(HideReason::Tray);
        else
            ShowPopup(false);
    }

    void HandleTraySelect()
    {
        if (GetTickCount64() - lastTrayMouseUpTick_ < 250)
            return;
        TogglePopup();
    }

    static void CALLBACK WinEventProc(
        HWINEVENTHOOK,
        DWORD event,
        HWND eventWindow,
        LONG objectId,
        LONG,
        DWORD,
        DWORD)
    {
        if (activeInstance_)
            activeInstance_->HandleShellWinEvent(event, eventWindow, objectId);
    }

    void StartSystemCalendarInterceptor()
    {
        activeInstance_ = this;
        calendarHook_ = SetWinEventHook(
            EVENT_OBJECT_CREATE,
            kEventObjectUncloak,
            nullptr,
            WinEventProc,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }

    void StopSystemCalendarInterceptor()
    {
        if (calendarHook_)
        {
            UnhookWinEvent(calendarHook_);
            calendarHook_ = nullptr;
        }

        const std::lock_guard lock(hiddenCalendarWindowsMutex_);
        for (const auto hiddenWindow : hiddenCalendarWindows_)
        {
            if (IsWindow(hiddenWindow))
                ShowWindow(hiddenWindow, SW_SHOW);
        }
        hiddenCalendarWindows_.clear();
        activeInstance_ = nullptr;
    }

    static bool IsCalendarShellProcess(HWND eventWindow)
    {
        DWORD processId{};
        GetWindowThreadProcessId(eventWindow, &processId);
        if (!processId)
            return false;

        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process)
            return false;

        wchar_t path[MAX_PATH]{};
        DWORD length = ARRAYSIZE(path);
        const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
        CloseHandle(process);
        if (!queried)
            return false;

        const wchar_t* fileName = PathFindFileNameW(path);
        return _wcsicmp(fileName, L"ShellExperienceHost.exe") == 0 ||
               _wcsicmp(fileName, L"ShellHost.exe") == 0;
    }

    static bool IsFlyoutAlignedWithTaskbar(HWND eventWindow)
    {
        RECT flyout{};
        if (!GetWindowRect(eventWindow, &flyout))
            return false;

        const int width = flyout.right - flyout.left;
        const int height = flyout.bottom - flyout.top;
        if (width <= 200 || height <= 100)
            return false;

        const HMONITOR monitor = MonitorFromWindow(eventWindow, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (!monitor || !GetMonitorInfoW(monitor, &info))
            return false;

        const int leftInset = info.rcWork.left - info.rcMonitor.left;
        const int topInset = info.rcWork.top - info.rcMonitor.top;
        const int rightInset = info.rcMonitor.right - info.rcWork.right;
        const int bottomInset = info.rcMonitor.bottom - info.rcWork.bottom;
        const int tolerance = MulDiv(50, GetDpiForWindow(eventWindow), 96);

        const bool leftAligned = std::abs(flyout.left - info.rcWork.left) < tolerance;
        const bool topAligned = std::abs(flyout.top - info.rcWork.top) < tolerance;
        const bool rightAligned = std::abs(flyout.right - info.rcWork.right) < tolerance;
        const bool bottomAligned = std::abs(flyout.bottom - info.rcWork.bottom) < tolerance;

        if (topInset >= leftInset && topInset >= rightInset && topInset >= bottomInset && topInset > 0)
            return rightAligned && topAligned;
        if (leftInset >= topInset && leftInset >= rightInset && leftInset >= bottomInset && leftInset > 0)
            return leftAligned && bottomAligned;
        if (rightInset >= leftInset && rightInset >= topInset && rightInset >= bottomInset && rightInset > 0)
            return rightAligned && bottomAligned;
        return rightAligned && bottomAligned;
    }

    void HandleShellWinEvent(DWORD event, HWND eventWindow, LONG objectId)
    {
        if (objectId != OBJID_WINDOW || !eventWindow)
            return;
        if (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW &&
            event != EVENT_OBJECT_STATECHANGE && event != EVENT_OBJECT_NAMECHANGE &&
            event != kEventObjectUncloak)
            return;
        if (!IsCalendarShellProcess(eventWindow))
            return;

        wchar_t className[128]{};
        GetClassNameW(eventWindow, className, ARRAYSIZE(className));
        constexpr wchar_t coreWindowClass[] = L"Windows.UI.Core.CoreWindow";
        if (_wcsnicmp(className, coreWindowClass, wcslen(coreWindowClass)) != 0)
            return;
        if (!IsFlyoutAlignedWithTaskbar(eventWindow))
            return;

        ShowWindow(eventWindow, SW_HIDE);
        {
            const std::lock_guard lock(hiddenCalendarWindowsMutex_);
            if (std::find(hiddenCalendarWindows_.begin(), hiddenCalendarWindows_.end(), eventWindow) ==
                hiddenCalendarWindows_.end())
            {
                hiddenCalendarWindows_.push_back(eventWindow);
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - lastCalendarInterceptTick_ < 500)
            return;
        lastCalendarInterceptTick_ = now;
        PostMessageW(window_, kShowPopupMessage, 0, 0);
    }

    void ShowContextMenu()
    {
        if (IsWindowVisible(window_))
            HidePopup(HideReason::ContextMenu);

        POINT point{};
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            point.x,
            point.y,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);

        if (command == kMenuSettings)
        {
            ShowSettingsWindow();
        }
        else if (command == kMenuExit)
        {
            DestroyWindow(window_);
        }
    }

    void ShowSettingsWindow()
    {
        if (IsWindow(settingsWindow_))
        {
            ShowWindow(settingsWindow_, SW_RESTORE);
            ShowWindow(window_, SW_SHOWNOACTIVATE);
            PositionLivePreview();
            SetForegroundWindow(settingsWindow_);
            return;
        }
        settingsDraft_ = wincal::SettingsStore::Load();
        settingsBeforeEdit_ = settings_;
        settingsSaveAccepted_ = false;
        settingsDpi_ = GetDpiForWindow(window_);
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT bounds = SettingsRect(0, 0, kSettingsLogicalWidth, kSettingsLogicalHeight);
        AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_APPWINDOW, settingsDpi_);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = std::min(static_cast<int>(bounds.right - bounds.left),
                                   static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        const int height = std::min(static_cast<int>(bounds.bottom - bounds.top),
                                    static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        const int pairWidth = width + SettingsPx(kLogicalWidth + 20);
        HWND window = CreateWindowExW(
            WS_EX_APPWINDOW,
            kSettingsWindowClass,
            L"WinCal 设置",
            style,
            monitor.rcWork.left + std::max(0L, (monitor.rcWork.right - monitor.rcWork.left - pairWidth) / 2),
            monitor.rcWork.top + std::max(0L, (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2),
            width, height,
            window_, nullptr, instance_, this);
        if (window)
        {
            settingsLivePreview_ = true;
            ShowPopup(false);
            ShowWindow(window, SW_SHOW);
            ApplyLiveSettingsPreview(window);
            PositionLivePreview();
            SetForegroundWindow(window);
            UpdateWindow(window);
        }
    }

    bool ApplyAutoStartup(bool enable) const
    {
        HKEY key{};
        const LSTATUS openResult = RegCreateKeyExW(
            HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (openResult != ERROR_SUCCESS)
            return false;

        LSTATUS result{};
        if (enable)
        {
            wchar_t executable[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
            if (length == 0 || length >= ARRAYSIZE(executable))
            {
                RegCloseKey(key);
                return false;
            }
            const std::wstring command = L"\"" + std::wstring(executable) + L"\"";
            result = RegSetValueExW(
                key, L"miniCal", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            result = RegDeleteValueW(key, L"miniCal");
            if (result == ERROR_FILE_NOT_FOUND)
                result = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    void SaveSettingsFromControls(HWND window)
    {
        const int theme = SettingSelection(window, kSettingTheme);
        settingsDraft_.themeMode = theme == 1 ? L"Light" : theme == 2 ? L"Dark" : L"FollowSystem";
        BOOL valid{};
        const int offset = static_cast<int>(GetDlgItemInt(window, kSettingFontOffset, &valid, TRUE));
        settingsDraft_.fontSizeOffset = std::clamp(valid ? offset : 0, -2, 10);
        settingsDraft_.autoStartup = SettingSelection(window, kSettingAutoStartup) != 0;
        const int source = SettingSelection(window, kSettingDataSource);
        settingsDraft_.dataSource = source == 1 ? L"IcsUrl" : source == 2 ? L"Both" : L"SystemCalendar";
        settingsDraft_.icsUrls = LinesFromText(ReadControlText(GetDlgItem(window, kSettingIcsUrls)));
        settingsDraft_.icsAliases.resize(settingsDraft_.icsUrls.size());
        constexpr std::array<int, 5> refreshValues{10, 30, 60, 120, 1440};
        const int refresh = SettingSelection(window, kSettingRefresh);
        settingsDraft_.icsRefreshMinutes = refreshValues[std::clamp(refresh, 0, 4)];
        settingsDraft_.weekStartDay = SettingSelection(window, kSettingWeekStart) == 1 ? L"Monday" : L"Sunday";
        if (!ApplyAutoStartup(settingsDraft_.autoStartup))
        {
            MessageBoxW(window, L"更新开机启动项失败。", L"WinCal", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!wincal::SettingsStore::Save(settingsDraft_))
        {
            MessageBoxW(window, L"保存设置失败。", L"WinCal", MB_OK | MB_ICONWARNING);
            return;
        }
        settingsSaveAccepted_ = true;
        PostMessageW(window_, kSettingsChangedMessage, 0, 0);
        DestroyWindow(window);
    }

    LRESULT HandleSettingsMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            CreateSettingsControls(window);
            ApplySettingsWindowAppearance(window);
            return 0;
        case WM_DPICHANGED:
        {
            settingsDpi_ = HIWORD(wParam);
            DeleteObject(settingsTitleFont_);
            DeleteObject(settingsBodyFont_);
            DeleteObject(settingsLabelFont_);
            DeleteObject(settingsCaptionFont_);
            settingsTitleFont_ = settingsBodyFont_ = settingsLabelFont_ = settingsCaptionFont_ = nullptr;
            EnsureSettingsFonts(window);
            const auto* bounds = reinterpret_cast<RECT*>(lParam);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromRect(bounds, MONITOR_DEFAULTTONEAREST), &monitor);
            const LONG width = std::min(bounds->right - bounds->left, monitor.rcWork.right - monitor.rcWork.left);
            const LONG height = std::min(bounds->bottom - bounds->top, monitor.rcWork.bottom - monitor.rcWork.top);
            SetWindowPos(window, nullptr,
                         std::clamp(bounds->left, monitor.rcWork.left, monitor.rcWork.right - width),
                         std::clamp(bounds->top, monitor.rcWork.top, monitor.rcWork.bottom - height),
                         width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            for (const auto& control : settingsControls_)
            {
                RECT rect = SettingsRect(control.x, control.y, control.width, control.height);
                SetWindowPos(control.window, nullptr, rect.left, rect.top, rect.right - rect.left,
                             rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
                SendMessageW(control.window, WM_SETFONT, reinterpret_cast<WPARAM>(settingsBodyFont_), FALSE);
            }
            UpdateSettingsFontControls(window);
            LayoutSettingsViewport(window);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0;
        }
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) LayoutSettingsViewport(window);
            return 0;
        case WM_MOVE:
            if (settingsLivePreview_) PositionLivePreview();
            return 0;
        case WM_VSCROLL:
        case WM_HSCROLL:
        case WM_MOUSEWHEEL:
        {
            const bool horizontal = message == WM_HSCROLL;
            const int bar = horizontal ? SB_HORZ : SB_VERT;
            SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL};
            GetScrollInfo(window, bar, &info);
            int position = info.nPos;
            if (message == WM_MOUSEWHEEL)
                position -= GET_WHEEL_DELTA_WPARAM(wParam) * SettingsPx(48) / WHEEL_DELTA;
            else
            {
                switch (LOWORD(wParam))
                {
                case SB_LINEUP: position -= SettingsPx(24); break;
                case SB_LINEDOWN: position += SettingsPx(24); break;
                case SB_PAGEUP: position -= info.nPage; break;
                case SB_PAGEDOWN: position += info.nPage; break;
                case SB_THUMBTRACK: position = info.nTrackPos; break;
                default: return 0;
                }
            }
            if (horizontal) settingsScrollX_ = position;
            else settingsScrollY_ = position;
            LayoutSettingsViewport(window);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintSettingsWindow(window);
            return 0;
        case WM_CTLCOLORSTATIC:
        {
            const auto palette = GetSettingsPalette();
            SetTextColor(reinterpret_cast<HDC>(wParam), palette.secondary);
            SetBkColor(reinterpret_cast<HDC>(wParam), palette.input);
            return reinterpret_cast<LRESULT>(settingsInputBrush_);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        {
            const auto palette = GetSettingsPalette();
            SetTextColor(reinterpret_cast<HDC>(wParam), palette.primary);
            SetBkColor(reinterpret_cast<HDC>(wParam), palette.input);
            return reinterpret_cast<LRESULT>(settingsInputBrush_);
        }
        case WM_CTLCOLORBTN:
        {
            const auto palette = GetSettingsPalette();
            SetTextColor(reinterpret_cast<HDC>(wParam), palette.primary);
            SetBkColor(reinterpret_cast<HDC>(wParam), palette.card);
            return reinterpret_cast<LRESULT>(settingsCardBrush_);
        }
        case WM_DRAWITEM:
            if (const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
                item && item->CtlType == ODT_BUTTON)
                return DrawSettingsButton(*item);
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == kSettingFontOffset && HIWORD(wParam) == EN_CHANGE)
            {
                UpdateSettingsFontControls(window);
                return 0;
            }
            if (LOWORD(wParam) == kSettingFontOffset && HIWORD(wParam) == EN_KILLFOCUS)
            {
                BOOL valid{};
                const int value = static_cast<int>(GetDlgItemInt(window, kSettingFontOffset, &valid, TRUE));
                SetDlgItemInt(window, kSettingFontOffset, static_cast<UINT>(std::clamp(valid ? value : 0, -2, 10)), TRUE);
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED)
            {
                const int id = LOWORD(wParam);
                if (id >= kSettingPageAppearance && id <= kSettingPageGeneral)
                {
                    SelectSettingsPage(window, id - kSettingPageAppearance);
                    return 0;
                }
                if (id == kSettingTheme || id == kSettingThemeLight || id == kSettingThemeDark)
                {
                    SetSettingSelection(window, kSettingTheme, id == kSettingTheme ? 0 : id == kSettingThemeLight ? 1 : 2);
                    ApplySettingsWindowAppearance(window);
                    return 0;
                }
                if (id == kSettingFontMinus || id == kSettingFontPlus)
                {
                    BOOL valid{};
                    const int value = static_cast<int>(GetDlgItemInt(window, kSettingFontOffset, &valid, TRUE));
                    SetDlgItemInt(window, kSettingFontOffset, static_cast<UINT>(
                        std::clamp((valid ? value : 0) + (id == kSettingFontPlus ? 1 : -1), -2, 10)), TRUE);
                    return 0;
                }
                if (id == kSettingAutoStartup)
                {
                    SetSettingSelection(window, id, !SettingSelection(window, id));
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                const auto options = SettingOptions(id);
                if (!options.empty())
                {
                    HMENU menu = CreatePopupMenu();
                    for (size_t i = 0; i < options.size(); ++i)
                        AppendMenuW(menu, MF_STRING | (SettingSelection(window, id) == static_cast<int>(i) ? MF_CHECKED : 0),
                                    i + 1, options[i]);
                    RECT anchor{};
                    GetWindowRect(GetDlgItem(window, id), &anchor);
                    const int result = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                       anchor.left, anchor.bottom, window, nullptr);
                    DestroyMenu(menu);
                    if (result)
                    {
                        SetSettingSelection(window, id, result - 1);
                        UpdateSettingsSourceControls(window);
                        ApplyLiveSettingsPreview(window);
                        InvalidateRect(window, nullptr, FALSE);
                    }
                    return 0;
                }
            }
            if (HIWORD(wParam) == BN_CLICKED && (LOWORD(wParam) == kSettingSave || LOWORD(wParam) == IDOK))
            {
                SaveSettingsFromControls(window);
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == kSettingDefaults)
            {
                settingsDraft_ = {};
                PopulateSettingsControls(window);
                UpdateSettingsFontControls(window);
                UpdateSettingsSourceControls(window);
                ApplySettingsWindowAppearance(window);
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED && (LOWORD(wParam) == kSettingCancel || LOWORD(wParam) == IDCANCEL))
            {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            FinishLiveSettingsPreview();
            if (settingsTitleFont_) DeleteObject(settingsTitleFont_);
            if (settingsBodyFont_) DeleteObject(settingsBodyFont_);
            if (settingsLabelFont_) DeleteObject(settingsLabelFont_);
            if (settingsCaptionFont_) DeleteObject(settingsCaptionFont_);
            if (settingsInputBrush_) DeleteObject(settingsInputBrush_);
            if (settingsCardBrush_) DeleteObject(settingsCardBrush_);
            settingsTitleFont_ = nullptr;
            settingsBodyFont_ = nullptr;
            settingsLabelFont_ = nullptr;
            settingsCaptionFont_ = nullptr;
            settingsControls_.clear();
            settingsControls_.shrink_to_fit();
            settingsHotControl_ = nullptr;
            settingsScrollX_ = settingsScrollY_ = 0;
            settingsInputBrush_ = nullptr;
            settingsCardBrush_ = nullptr;
            settingsWindow_ = nullptr;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void ChangeMonth(int delta)
    {
        int value = displayYear_ * 12 + displayMonth_ - 1 + delta;
        displayYear_ = value / 12;
        displayMonth_ = value % 12 + 1;
        if (displayMonth_ <= 0)
        {
            displayMonth_ += 12;
            --displayYear_;
        }
        RequestSystemCalendarMonth(displayYear_, displayMonth_);
        BeginTransition();
    }

    void ChangeYear(int delta)
    {
        displayYear_ += delta;
        RequestSystemCalendarMonth(displayYear_, displayMonth_);
        BeginTransition();
    }

    void BeginTransition()
    {
        transition_ = 0.0f;
        hoveredCell_ = -1;
        SetTimer(window_, kAnimationTimer, 16, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }

    float Scale(float value) const
    {
        return value * static_cast<float>(dpi_) / 96.0f;
    }

    void UpdateWindowSize()
    {
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            static_cast<int>(Scale(kLogicalWidth)),
            static_cast<int>(Scale(kLogicalHeight)),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    int HitTestCell(int x, int y) const
    {
        const float logicalX = x * 96.0f / dpi_;
        const float logicalY = y * 96.0f / dpi_;
        const int column = static_cast<int>((logicalX - kGridLeft) / kCellWidth);
        const int row = static_cast<int>((logicalY - kGridTop) / kCellHeight);
        if (logicalX < kGridLeft || column < 0 || column >= 7 || row < 0 || row >= 6)
            return -1;
        return row * 7 + column;
    }

    bool IsOverSchedule(int x, int y) const
    {
        const float logicalX = x * 96.0f / dpi_;
        const float logicalY = y * 96.0f / dpi_;
        return logicalX >= 18 && logicalX <= 412 && logicalY >= kScheduleTop && logicalY <= kScheduleBottom;
    }

    int HitTestScheduleRow(int x, int y) const
    {
        const float logicalX = x * 96.0f / dpi_;
        const float logicalY = y * 96.0f / dpi_;
        if (logicalX < 24 || logicalX > 406 || logicalY < kScheduleFirstRow)
            return -1;
        const int row = static_cast<int>((logicalY - kScheduleFirstRow) / kScheduleRowHeight);
        return row >= 0 && row < kVisibleScheduleRows ? row : -1;
    }

    int DetailPx(int value) const
    {
        return MulDiv(value, detailDpi_ ? detailDpi_ : 96, 96);
    }

    void EnsureDetailFonts(HWND window)
    {
        if (detailTitleFont_) return;
        detailDpi_ = GetDpiForWindow(window);
        const auto makeFont = [&](int points, int weight)
        {
            return CreateFontW(-MulDiv(points, detailDpi_, 72), 0, 0, 0, weight,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        };
        detailTitleFont_ = makeFont(16, FW_SEMIBOLD);
        detailBodyFont_ = makeFont(11, FW_NORMAL);
        detailCaptionFont_ = makeFont(9, FW_NORMAL);
    }

    void DetailText(HDC dc, const wchar_t* text, RECT rect, HFONT font,
                    COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) const
    {
        const auto oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        ::DrawTextW(dc, text, -1, &rect, flags | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }

    void PaintDetailWindow(HWND window)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        const bool isDark = dark_;
        const COLORREF background = isDark ? RGB(24, 25, 31) : RGB(246, 247, 251);
        const COLORREF card = isDark ? RGB(33, 35, 43) : RGB(255, 255, 255);
        const COLORREF primary = isDark ? RGB(237, 238, 245) : RGB(33, 38, 57);
        const COLORREF secondary = isDark ? RGB(164, 169, 186) : RGB(113, 121, 144);
        const COLORREF border = isDark ? RGB(57, 60, 75) : RGB(229, 232, 242);
        const COLORREF accent = isDark ? RGB(162, 155, 255) : RGB(103, 91, 218);
        RECT client{};
        GetClientRect(window, &client);
        const auto backgroundBrush = CreateSolidBrush(background);
        FillRect(dc, &client, backgroundBrush);
        DeleteObject(backgroundBrush);
        RECT panel{DetailPx(1), DetailPx(1), client.right - DetailPx(1), client.bottom - DetailPx(1)};
        const auto panelBrush = CreateSolidBrush(card);
        const auto panelPen = CreatePen(PS_SOLID, DetailPx(1), border);
        const auto oldBrush = SelectObject(dc, panelBrush);
        const auto oldPen = SelectObject(dc, panelPen);
        RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, DetailPx(14), DetailPx(14));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(panelBrush);
        DeleteObject(panelPen);

        RECT closeRect{DetailPx(312), DetailPx(14), DetailPx(344), DetailPx(46)};
        if (detailCloseHot_)
        {
            const auto hoverBrush = CreateSolidBrush(isDark ? RGB(66, 61, 82) : RGB(242, 240, 253));
            FillRect(dc, &closeRect, hoverBrush);
            DeleteObject(hoverBrush);
        }
        DetailText(dc, L"×", closeRect, detailBodyFont_, secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT titleRect{DetailPx(24), DetailPx(22), DetailPx(294), DetailPx(76)};
        DetailText(dc, detailEvent_.title.c_str(), titleRect, detailTitleFont_, primary,
                   DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

        wchar_t dateText[48]{};
        swprintf_s(dateText, L"%d年%d月%d日", detailDate_.year, detailDate_.month, detailDate_.day);
        DetailText(dc, dateText, {DetailPx(24), DetailPx(92), DetailPx(310), DetailPx(118)},
                   detailBodyFont_, accent);

        std::wstring timeText;
        if (detailEvent_.allDay)
            timeText = L"全天日程";
        else
            timeText = L"开始：" + EventTimeText(detailEvent_, detailDate_);
        if (detailEvent_.endYear || detailEvent_.endMonth || detailEvent_.endDay)
        {
            timeText += L"  · 结束：" + std::to_wstring(detailEvent_.endMonth) +
                        L"月" + std::to_wstring(detailEvent_.endDay) + L"日";
        }
        DetailText(dc, timeText.c_str(), {DetailPx(24), DetailPx(126), DetailPx(332), DetailPx(154)},
                   detailCaptionFont_, secondary);

        const auto separator = CreateSolidBrush(border);
        RECT line{DetailPx(24), DetailPx(174), client.right - DetailPx(24), DetailPx(175)};
        FillRect(dc, &line, separator);
        DeleteObject(separator);
        DetailText(dc, L"日程详情", {DetailPx(24), DetailPx(192), DetailPx(300), DetailPx(218)},
                   detailCaptionFont_, secondary);
        DetailText(dc, L"点击右上角 × 或按 Esc 关闭", {DetailPx(24), DetailPx(236), DetailPx(320), DetailPx(260)},
                   detailCaptionFont_, secondary);
        EndPaint(window, &paint);
    }

    void PositionDetailWindow()
    {
        if (!IsWindow(detailWindow_)) return;
        RECT mainRect{};
        GetWindowRect(window_, &mainRect);
        HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        GetMonitorInfoW(monitor, &monitorInfo);
        const int width = DetailPx(kDetailLogicalWidth);
        const int height = DetailPx(kDetailLogicalHeight);
        const int gap = DetailPx(12);
        LONG x = mainRect.right + gap;
        if (x + width > monitorInfo.rcWork.right)
            x = mainRect.left - width - gap;
        x = std::clamp(x, monitorInfo.rcWork.left, std::max(monitorInfo.rcWork.left, monitorInfo.rcWork.right - width));
        LONG y = std::clamp(mainRect.top, monitorInfo.rcWork.top, std::max(monitorInfo.rcWork.top, monitorInfo.rcWork.bottom - height));
        SetWindowPos(detailWindow_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    }

    void CloseDetailWindow()
    {
        if (IsWindow(detailWindow_))
            DestroyWindow(detailWindow_);
    }

    LRESULT HandleDetailMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
        {
            detailDpi_ = GetDpiForWindow(window);
            EnsureDetailFonts(window);
            BOOL darkValue = dark_ ? TRUE : FALSE;
            DwmSetWindowAttribute(window, 20, &darkValue, sizeof(darkValue));
            return 0;
        }
        case WM_PAINT:
            PaintDetailWindow(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            const bool hot = GET_X_LPARAM(lParam) >= DetailPx(304) &&
                             GET_X_LPARAM(lParam) <= DetailPx(352) &&
                             GET_Y_LPARAM(lParam) >= DetailPx(8) &&
                             GET_Y_LPARAM(lParam) <= DetailPx(52);
            if (hot != detailCloseHot_)
            {
                detailCloseHot_ = hot;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            detailCloseHot_ = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            if (GET_X_LPARAM(lParam) >= DetailPx(304) &&
                GET_X_LPARAM(lParam) <= DetailPx(352) &&
                GET_Y_LPARAM(lParam) >= DetailPx(8) &&
                GET_Y_LPARAM(lParam) <= DetailPx(52))
                CloseDetailWindow();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                CloseDetailWindow();
                return 0;
            }
            break;
        case WM_DPICHANGED:
        {
            detailDpi_ = HIWORD(wParam);
            if (detailTitleFont_) DeleteObject(detailTitleFont_);
            if (detailBodyFont_) DeleteObject(detailBodyFont_);
            if (detailCaptionFont_) DeleteObject(detailCaptionFont_);
            detailTitleFont_ = detailBodyFont_ = detailCaptionFont_ = nullptr;
            EnsureDetailFonts(window);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            PositionDetailWindow();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_NCDESTROY:
            if (detailTitleFont_) DeleteObject(detailTitleFont_);
            if (detailBodyFont_) DeleteObject(detailBodyFont_);
            if (detailCaptionFont_) DeleteObject(detailCaptionFont_);
            detailTitleFont_ = detailBodyFont_ = detailCaptionFont_ = nullptr;
            detailWindow_ = nullptr;
            detailCloseHot_ = false;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void ShowEventDetails(int row)
    {
        const auto events = calendarData_.EventsForDate(selected_.year, selected_.month, selected_.day);
        const int index = selectedEventScroll_ + row;
        if (index < 0 || index >= static_cast<int>(events.size()))
            return;
        detailEvent_ = events[static_cast<size_t>(index)];
        detailDate_ = selected_;
        if (!IsWindow(detailWindow_))
        {
            detailWindow_ = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                kDetailWindowClass,
                L"日程详情",
                WS_POPUP,
                0, 0, DetailPx(kDetailLogicalWidth), DetailPx(kDetailLogicalHeight),
                window_, nullptr, instance_, this);
        }
        if (detailWindow_)
        {
            EnsureDetailFonts(detailWindow_);
            PositionDetailWindow();
            ShowWindow(detailWindow_, SW_SHOWNOACTIVATE);
            SetWindowPos(detailWindow_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
            InvalidateRect(detailWindow_, nullptr, FALSE);
        }
    }

    Date DateForCell(int index) const
    {
        const long long first = DaysFromCivil(displayYear_, static_cast<unsigned>(displayMonth_), 1);
        const int mondayBasedWeekday = static_cast<int>((first + 3) % 7 + 7) % 7;
        const int firstColumn = weekStartsMonday_ ? mondayBasedWeekday : (mondayBasedWeekday + 1) % 7;
        return CivilFromDays(first - firstColumn + index);
    }

    void ReportRenderFailure(const wchar_t* stage, HRESULT result)
    {
        wchar_t message[96]{};
        swprintf_s(message, L"WinCal - %s 0x%08X", stage, static_cast<unsigned>(result));
        SetWindowTextW(window_, message);
    }

    bool EnsureDeviceResources()
    {
        if (!factory_)
        {
            const HRESULT result = D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                factory_.ReleaseAndGetAddressOf());
            if (FAILED(result))
            {
                ReportRenderFailure(L"D2D factory", result);
                return false;
            }
        }
        if (!writeFactory_)
        {
            const HRESULT result = DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(writeFactory_.ReleaseAndGetAddressOf()));
            if (FAILED(result))
            {
                ReportRenderFailure(L"DWrite factory", result);
                return false;
            }
        }

        if (!renderTarget_)
        {
            RECT rect{};
            GetClientRect(window_, &rect);
            const D2D1_SIZE_U size{static_cast<UINT32>(rect.right), static_cast<UINT32>(rect.bottom)};
            const HRESULT result = factory_->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(),
                    96.0f,
                    96.0f),
                D2D1::HwndRenderTargetProperties(window_, size),
                renderTarget_.ReleaseAndGetAddressOf());
            if (FAILED(result))
            {
                ReportRenderFailure(L"render target", result);
                return false;
            }
        }

        if (!titleFormat_)
        {
            HRESULT result = writeFactory_->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, TextScale(20), L"zh-CN",
                titleFormat_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) result = writeFactory_->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, TextScale(12), L"zh-CN",
                bodyFormat_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) result = writeFactory_->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, TextScale(13), L"zh-CN",
                dayFormat_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) result = writeFactory_->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, TextScale(9), L"zh-CN",
                lunarFormat_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) result = writeFactory_->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, TextScale(9), L"zh-CN",
                badgeFormat_.ReleaseAndGetAddressOf());
            if (FAILED(result))
            {
                ReportRenderFailure(L"text format", result);
                return false;
            }
            bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dayFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dayFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            lunarFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            lunarFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return true;
    }

    ComPtr<ID2D1SolidColorBrush> Brush(const D2D1_COLOR_F& color, float opacity = 1.0f)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        auto actual = color;
        actual.a *= opacity;
        renderTarget_->CreateSolidColorBrush(actual, brush.ReleaseAndGetAddressOf());
        return brush;
    }

    void DrawText(
        const wchar_t* text,
        IDWriteTextFormat* format,
        const D2D1_RECT_F& rect,
        const D2D1_COLOR_F& color,
        float opacity = 1.0f)
    {
        const auto brush = Brush(color, opacity);
        renderTarget_->DrawTextW(
            text,
            static_cast<UINT32>(wcslen(text)),
            format,
            rect,
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Paint()
    {
        if (!EnsureDeviceResources() || !renderTarget_)
            return;

        const auto theme = GetTheme(dark_);
        renderTarget_->BeginDraw();
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        renderTarget_->Clear(theme.background);

        wchar_t title[64]{};
        swprintf_s(title, L"%d年%d月", displayYear_, displayMonth_);
        DrawText(title, titleFormat_.Get(), D2D1::RectF(Scale(22), Scale(kHeaderTop), Scale(260), Scale(62)), theme.primary);

        const auto today = Today();

        const auto separator = Brush(theme.separator);
        renderTarget_->DrawLine(
            D2D1::Point2F(Scale(18), Scale(78)),
            D2D1::Point2F(Scale(412), Scale(78)),
            separator.Get(),
            Scale(1));

        static constexpr std::array<const wchar_t*, 7> mondayWeekdays{
            L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
        static constexpr std::array<const wchar_t*, 7> sundayWeekdays{
            L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
        const auto& weekdays = weekStartsMonday_ ? mondayWeekdays : sundayWeekdays;
        for (int column = 0; column < 7; ++column)
        {
            const float left = Scale(static_cast<float>(kGridLeft + column * kCellWidth));
            DrawText(
                weekdays[column],
                bodyFormat_.Get(),
                D2D1::RectF(left, Scale(kWeekTop), left + Scale(kCellWidth), Scale(kGridTop)),
                (weekStartsMonday_ ? column >= 5 : column == 0 || column == 6) ? theme.accent : theme.secondary);
        }

        const float contentOpacity = 0.45f + transition_ * 0.55f;
        for (int index = 0; index < 42; ++index)
        {
            const int row = index / 7;
            const int column = index % 7;
            const float left = Scale(static_cast<float>(kGridLeft + column * kCellWidth));
            const float top = Scale(static_cast<float>(kGridTop + row * kCellHeight));
            const auto rect = D2D1::RectF(
                left + Scale(3), top + Scale(3), left + Scale(kCellWidth - 3), top + Scale(kCellHeight - 3));
            const auto date = DateForCell(index);
            const bool inMonth = date.month == displayMonth_;
            const bool isToday = date == today;
            const bool selected = date == selected_;
            const bool hasEvents = calendarData_.HasEvents(date.year, date.month, date.day);
            const auto schedule = calendarData_.ScheduleForDate(date.year, date.month, date.day);

            if (index == hoveredCell_)
            {
                const auto hover = Brush(theme.hover, contentOpacity);
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect, Scale(9), Scale(9)), hover.Get());
            }
            if (selected)
            {
                const auto accent = Brush(theme.accent, contentOpacity);
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect, Scale(9), Scale(9)), accent.Get());
            }
            else if (isToday)
            {
                const auto accent = Brush(theme.accent, contentOpacity);
                renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect, Scale(9), Scale(9)), accent.Get(), Scale(1.5f));
            }

            wchar_t number[4]{};
            swprintf_s(number, L"%d", date.day);
            DrawText(
                number,
                dayFormat_.Get(),
                D2D1::RectF(rect.left, rect.top + Scale(1), rect.right, rect.top + Scale(25)),
                selected ? D2D1::ColorF(0xFFFFFF) : (inMonth ? theme.primary : theme.muted),
                contentOpacity);

            const auto lunar = lunarCalendar_.TextForDate(date.year, date.month, date.day);
            if (!lunar.empty())
            {
                DrawText(
                    lunar.c_str(),
                    lunarFormat_.Get(),
                    D2D1::RectF(rect.left + Scale(1), rect.top + Scale(22), rect.right - Scale(1), rect.bottom - Scale(7)),
                    selected ? D2D1::ColorF(0xFFFFFF) : (inMonth ? theme.secondary : theme.muted),
                    contentOpacity);
            }

            if (schedule != wincal::ScheduleLabel::None)
            {
                const bool isRest = schedule == wincal::ScheduleLabel::Rest;
                const auto badgeRect = D2D1::RectF(
                    rect.right - Scale(17), rect.top + Scale(1),
                    rect.right - Scale(1), rect.top + Scale(17));
                const auto badgeBackground = Brush(
                    isRest ? theme.restBadgeBackground : theme.workBadgeBackground,
                    contentOpacity);
                renderTarget_->FillRoundedRectangle(
                    D2D1::RoundedRect(badgeRect, Scale(4), Scale(4)), badgeBackground.Get());
                DrawText(
                    isRest ? L"休" : L"班",
                    badgeFormat_.Get(), badgeRect,
                    isRest ? theme.restBadgeForeground : theme.workBadgeForeground,
                    contentOpacity);
            }

            if (hasEvents)
            {
                const auto dot = Brush(
                    selected ? D2D1::ColorF(0xFFFFFF) : theme.eventDot,
                    contentOpacity);
                renderTarget_->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F((rect.left + rect.right) / 2, rect.bottom - Scale(3)),
                        Scale(1.75f), Scale(1.75f)),
                    dot.Get());
            }
        }

        const float cardTop = Scale(kScheduleTop);
        const auto cardBrush = Brush(theme.card);
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(Scale(18), cardTop, Scale(412), Scale(kScheduleBottom)), Scale(12), Scale(12)),
            cardBrush.Get());
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        wchar_t scheduleTitle[64]{};
        swprintf_s(scheduleTitle, L"%d月%d日 · 日程", selected_.month, selected_.day);
        DrawText(
            scheduleTitle, bodyFormat_.Get(),
            D2D1::RectF(Scale(34), Scale(kScheduleTop + 12), Scale(250), Scale(kScheduleTop + 40)),
            theme.primary);
        const auto selectedEvents = calendarData_.EventsForDate(selected_.year, selected_.month, selected_.day);
        if (selectedEvents.empty())
        {
            wchar_t status[128]{};
            const size_t eventCount = calendarData_.EventCount();
            const size_t icsCount = calendarData_.IcsEventCount();
            const size_t systemCount = calendarData_.SystemEventCount();
            const auto systemState = calendarData_.SystemState();
            if (systemState == wincal::SystemCalendarState::Loading)
                swprintf_s(status, L"已加载 %zu 个事件 · 系统日历加载中", eventCount);
            else if (systemState == wincal::SystemCalendarState::Available)
                swprintf_s(status, L"已加载 %zu 个事件 · 系统 %zu / ICS %zu", eventCount, systemCount, icsCount);
            else if (systemState == wincal::SystemCalendarState::Unavailable)
                swprintf_s(status, L"已加载 %zu 个事件 · 系统日历不可用", eventCount);
            else if (eventCount > 0)
                swprintf_s(status, L"近期没有日程 · 已从缓存加载 %zu 个事件", eventCount);
            else
                wcscpy_s(status, L"未找到可用的 ICS 缓存");
            DrawText(
                status, bodyFormat_.Get(),
                D2D1::RectF(Scale(34), Scale(kScheduleFirstRow), Scale(390), Scale(kScheduleFirstRow + 40)),
                theme.secondary);
        }
        else
        {
            const int lastScroll = std::max(0, static_cast<int>(selectedEvents.size()) - kVisibleScheduleRows);
            selectedEventScroll_ = std::clamp(selectedEventScroll_, 0, lastScroll);
            for (int row = 0; row < kVisibleScheduleRows; ++row)
            {
                const int index = selectedEventScroll_ + row;
                if (index >= static_cast<int>(selectedEvents.size()))
                    break;
                const float top = Scale(static_cast<float>(kScheduleFirstRow + row * kScheduleRowHeight));
                const auto& event = selectedEvents[static_cast<size_t>(index)];
                const auto line = EventTimeText(event, selected_) + L"  " + event.title;
                DrawText(
                    line.c_str(), bodyFormat_.Get(),
                    D2D1::RectF(Scale(34), top, Scale(394), top + Scale(kScheduleRowHeight - 2)),
                    theme.secondary);
            }
            if (selectedEvents.size() > kVisibleScheduleRows)
            {
                wchar_t indicator[32]{};
                swprintf_s(indicator, L"%d-%d / %zu", selectedEventScroll_ + 1,
                    std::min(selectedEventScroll_ + kVisibleScheduleRows, static_cast<int>(selectedEvents.size())),
                    selectedEvents.size());
                bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                DrawText(
                    indicator, bodyFormat_.Get(),
                    D2D1::RectF(Scale(300), Scale(kScheduleTop + 12), Scale(394), Scale(kScheduleTop + 40)),
                    theme.muted);
                bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        const HRESULT result = renderTarget_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET)
            DiscardDeviceResources();
    }

    void DiscardDeviceResources()
    {
        titleFormat_.Reset();
        bodyFormat_.Reset();
        dayFormat_.Reset();
        lunarFormat_.Reset();
        badgeFormat_.Reset();
        renderTarget_.Reset();
        writeFactory_.Reset();
        factory_.Reset();
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == taskbarCreatedMessage_)
        {
            AddTrayIcon();
            return 0;
        }

        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            Paint();
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (renderTarget_)
                renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            return 0;
        case WM_DPICHANGED:
        {
            dpi_ = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(
                window_, HWND_TOPMOST, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOACTIVATE);
            DiscardDeviceResources();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (!settingsLivePreview_) settings_ = wincal::SettingsStore::Load();
            ApplyDwmAppearance();
            DiscardDeviceResources();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case kSettingsChangedMessage:
            settings_ = wincal::SettingsStore::Load();
            ApplyDwmAppearance();
            DiscardDeviceResources();
            calendarData_.Load();
            StartCalendarRefresh();
            UpdateCalendarDiagnostics();
            RequestSystemCalendarMonth(displayYear_, displayMonth_);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            TrackMouseEvent(&tracking);
            const int hit = HitTestCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit != hoveredCell_)
            {
                hoveredCell_ = hit;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hoveredCell_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        {
            if (const int scheduleRow = HitTestScheduleRow(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); scheduleRow >= 0)
            {
                ShowEventDetails(scheduleRow);
                return 0;
            }
            const int hit = HitTestCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit >= 0)
            {
                CloseDetailWindow();
                selected_ = DateForCell(hit);
                selectedEventScroll_ = 0;
                if (selected_.year != displayYear_ || selected_.month != displayMonth_)
                {
                    displayYear_ = selected_.year;
                    displayMonth_ = selected_.month;
                    BeginTransition();
                }
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window_, &point);
            if (IsOverSchedule(point.x, point.y))
            {
                selectedEventScroll_ += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1;
                InvalidateRect(window_, nullptr, FALSE);
            }
            else
            {
                ChangeMonth(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_LEFT) ChangeMonth(-1);
            else if (wParam == VK_RIGHT) ChangeMonth(1);
            else if (wParam == VK_UP) ChangeYear(-1);
            else if (wParam == VK_DOWN) ChangeYear(1);
            else if (wParam == VK_ESCAPE) HidePopup(HideReason::EscapeKey);
            else return DefWindowProcW(window_, message, wParam, lParam);
            return 0;
        case WM_TIMER:
            if (wParam == kAnimationTimer)
            {
                transition_ = std::min(1.0f, transition_ + 0.055f);
                if (transition_ >= 1.0f)
                    KillTimer(window_, kAnimationTimer);
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == kOutsideClickTimer)
            {
                KillTimer(window_, kOutsideClickTimer);
                if (IsWindowVisible(window_))
                    HidePopup(HideReason::OutsideClick);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            // 弹窗收起由全局鼠标监听处理，避免点击托盘时的焦点消息与
            // Shell_NotifyIcon 回调交错，造成一次点击执行两次显示切换。
            return 0;
        case kTrayMessage:
            switch (LOWORD(lParam))
            {
            case WM_LBUTTONDOWN:
                BeginTrayClick();
                break;
            case WM_LBUTTONUP:
                EndTrayClick();
                break;
            case NIN_SELECT:
                HandleTraySelect();
                break;
            case NIN_KEYSELECT:
                TogglePopup();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowContextMenu();
                break;
            default:
                break;
            }
            return 0;
        case kShowPopupMessage:
            ShowPopup(true);
            return 0;
        case kScheduleOutsideHideMessage:
            if (!settingsLivePreview_) SetTimer(window_, kOutsideClickTimer, 120, nullptr);
            return 0;
        case kCalendarDataUpdatedMessage:
            UpdateCalendarDiagnostics();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case kShowSettingsMessage:
            ShowSettingsWindow();
            return 0;
        case WM_DESTROY:
            CloseDetailWindow();
            for (auto& thread : calendarRefreshThreads_)
                if (thread.joinable()) thread.request_stop();
            if (systemCalendarThread_.joinable())
                systemCalendarThread_.request_stop();
            RemovePropW(window_, L"WinCal.LastHideReason");
            RemovePropW(window_, L"WinCal.EventCount");
            RemovePropW(window_, L"WinCal.IcsEventCount");
            RemovePropW(window_, L"WinCal.SystemEventCount");
            RemovePropW(window_, L"WinCal.SystemLoadedMonthCount");
            RemovePropW(window_, L"WinCal.SystemCalendarState");
            RemovePropW(window_, L"WinCal.TodayEventCount");
            RemovePropW(window_, L"WinCal.TomorrowEventCount");
            StopGlobalMouseMonitor();
            StopSystemCalendarInterceptor();
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HINSTANCE instance_{};
    HWND window_{};
    HWND settingsWindow_{};
    HWND detailWindow_{};
    HFONT detailTitleFont_{};
    HFONT detailBodyFont_{};
    HFONT detailCaptionFont_{};
    UINT detailDpi_{96};
    bool detailCloseHot_{};
    Date detailDate_{};
    wincal::CalendarEvent detailEvent_{};
#ifdef WINCAL_SETTINGS_PREVIEW
    UINT settingsPreviewDpi_{96};
#endif
    struct SettingControlLayout { HWND window; int x; int y; int width; int height; int page; };
    std::vector<SettingControlLayout> settingsControls_;
    UINT settingsDpi_{96};
    int settingsPage_{};
    HWND settingsHotControl_{};
    int settingsScrollX_{};
    int settingsScrollY_{};
    bool settingsLayoutActive_{};
    HFONT settingsCaptionFont_{};
    HFONT settingsTitleFont_{};
    HFONT settingsBodyFont_{};
    HFONT settingsLabelFont_{};
    HBRUSH settingsInputBrush_{};
    HBRUSH settingsCardBrush_{};
    NOTIFYICONDATAW tray_{};
    HHOOK mouseHook_{};
    HWINEVENTHOOK calendarHook_{};
    std::vector<HWND> hiddenCalendarWindows_;
    std::mutex hiddenCalendarWindowsMutex_;
    ULONGLONG lastCalendarInterceptTick_{};
    ULONGLONG lastTrayMouseUpTick_{};
    bool trayPointerDown_{};
    bool trayClickWasVisible_{};
    UINT taskbarCreatedMessage_{};
    UINT dpi_{96};
    bool dark_{};
    bool settingsWindowDark_{};
    float fontScale_{1.0f};
    bool weekStartsMonday_{true};
    wincal::AppSettings settings_;
    wincal::AppSettings settingsDraft_;
    wincal::AppSettings settingsBeforeEdit_;
    bool settingsLivePreview_{};
    bool settingsSaveAccepted_{};
    int displayYear_{};
    int displayMonth_{};
    Date selected_{};
    int hoveredCell_{-1};
    int selectedEventScroll_{};
    float transition_{1.0f};
    wincal::CalendarData calendarData_;
    wincal::LunarCalendar lunarCalendar_;
    std::vector<std::jthread> calendarRefreshThreads_;
    std::jthread systemCalendarThread_;
    std::mutex systemCalendarRequestMutex_;
    std::optional<std::pair<int, int>> pendingSystemCalendarMonth_;
    bool systemCalendarWorkerRunning_{};

    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> bodyFormat_;
    ComPtr<IDWriteTextFormat> dayFormat_;
    ComPtr<IDWriteTextFormat> lunarFormat_;
    ComPtr<IDWriteTextFormat> badgeFormat_;

    inline static App* activeInstance_{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    const bool openSettings = commandLine && wcsstr(commandLine, L"--settings");
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, L"Local\\WinCal.Native.Singleton");
    if (!singleInstance)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND existing = FindWindowW(L"WinCalNativePopup", nullptr))
        {
            PostMessageW(existing, openSettings ? WM_APP + 6 : WM_APP + 2, 0, 0);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleInstance);
        return 0;
    }
    App app;
    const int result = app.Run(instance, showCommand, openSettings);
    CloseHandle(singleInstance);
    return result;
}
