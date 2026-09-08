#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <imm.h>
#include <wrl/client.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../resources/resource.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kWindowClass[] = L"WinCalNativePopup";
constexpr wchar_t kWindowTitle[] = L"WinCal";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowPopupMessage = WM_APP + 2;
constexpr UINT kScheduleOutsideHideMessage = WM_APP + 3;
constexpr UINT kAnimationTimer = 1;
constexpr UINT kOutsideClickTimer = 2;
constexpr DWORD kEventObjectUncloak = 0x8018;
constexpr UINT kMenuSettings = 1001;
constexpr UINT kMenuExit = 1002;
constexpr int kLogicalWidth = 430;
constexpr int kLogicalHeight = 560;
constexpr int kHeaderTop = 22;
constexpr int kWeekTop = 96;
constexpr int kGridTop = 124;
constexpr int kCellWidth = 58;
constexpr int kCellHeight = 49;
constexpr int kGridLeft = 12;

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
            D2D1::ColorF(0x2A3039), D2D1::ColorF(0x31363E)};
    }

    return {
        D2D1::ColorF(0xF8F9FB), D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0x20242A),
        D2D1::ColorF(0x6C737F), D2D1::ColorF(0xA1A7B0), D2D1::ColorF(0x3478F6),
        D2D1::ColorF(0xEDF3FF), D2D1::ColorF(0xE7E9ED)};
}

class App
{
public:
    int Run(HINSTANCE instance, int)
    {
        instance_ = instance;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (!RegisterWindowClass() || !CreateMainWindow())
            return 1;

        AddTrayIcon();
        StartSystemCalendarInterceptor();
        StartGlobalMouseMonitor();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        StopGlobalMouseMonitor();
        StopSystemCalendarInterceptor();
        RemoveTrayIcon();
        DiscardDeviceResources();
        CoUninitialize();
        return static_cast<int>(message.wParam);
    }

private:
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
        ApplyDwmAppearance();
        const auto today = Today();
        displayYear_ = today.year;
        displayMonth_ = today.month;
        selected_ = today;
        return true;
    }

    void ApplyDwmAppearance()
    {
        dark_ = IsDarkMode();
        const BOOL darkValue = dark_ ? TRUE : FALSE;
        DwmSetWindowAttribute(window_, 20, &darkValue, sizeof(darkValue));
        const DWORD roundPreference = 2;
        DwmSetWindowAttribute(window_, 33, &roundPreference, sizeof(roundPreference));
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
        hoveredCell_ = -1;
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
        if (message != WM_LBUTTONDOWN && message != WM_RBUTTONDOWN && message != WM_MBUTTONDOWN)
            return;
        if (!IsWindowVisible(window_))
            return;

        RECT popupRect{};
        if (GetWindowRect(window_, &popupRect) && PtInRect(&popupRect, mouse.pt))
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
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置（迁移中）");
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
            MessageBoxW(window_, L"原生设置页将在数据层迁移阶段接入。", L"WinCal", MB_OK | MB_ICONINFORMATION);
        }
        else if (command == kMenuExit)
        {
            DestroyWindow(window_);
        }
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
        BeginTransition();
    }

    void ChangeYear(int delta)
    {
        displayYear_ += delta;
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

    Date DateForCell(int index) const
    {
        const long long first = DaysFromCivil(displayYear_, static_cast<unsigned>(displayMonth_), 1);
        const int mondayBasedWeekday = static_cast<int>((first + 3) % 7 + 7) % 7;
        return CivilFromDays(first - mondayBasedWeekday + index);
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
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, Scale(20), L"zh-CN",
                titleFormat_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) result = writeFactory_->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, Scale(12), L"zh-CN",
                bodyFormat_.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) result = writeFactory_->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, Scale(13), L"zh-CN",
                dayFormat_.ReleaseAndGetAddressOf());
            if (FAILED(result))
            {
                ReportRenderFailure(L"text format", result);
                return false;
            }
            bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dayFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dayFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
        wchar_t todayText[64]{};
        swprintf_s(todayText, L"今天  %d月%d日", today.month, today.day);
        auto bodyLeft = bodyFormat_->GetTextAlignment();
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawText(todayText, bodyFormat_.Get(), D2D1::RectF(Scale(250), Scale(26), Scale(405), Scale(58)), theme.secondary);
        bodyFormat_->SetTextAlignment(bodyLeft);

        const auto separator = Brush(theme.separator);
        renderTarget_->DrawLine(
            D2D1::Point2F(Scale(18), Scale(78)),
            D2D1::Point2F(Scale(412), Scale(78)),
            separator.Get(),
            Scale(1));

        static constexpr std::array<const wchar_t*, 7> weekdays{
            L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
        for (int column = 0; column < 7; ++column)
        {
            const float left = Scale(static_cast<float>(kGridLeft + column * kCellWidth));
            DrawText(
                weekdays[column],
                bodyFormat_.Get(),
                D2D1::RectF(left, Scale(kWeekTop), left + Scale(kCellWidth), Scale(kGridTop)),
                column >= 5 ? theme.accent : theme.secondary);
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
                rect,
                selected ? D2D1::ColorF(0xFFFFFF) : (inMonth ? theme.primary : theme.muted),
                contentOpacity);
        }

        const float cardTop = Scale(432);
        const auto cardBrush = Brush(theme.card);
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(Scale(18), cardTop, Scale(412), Scale(536)), Scale(12), Scale(12)),
            cardBrush.Get());
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DrawText(L"近期日程", bodyFormat_.Get(), D2D1::RectF(Scale(34), Scale(446), Scale(200), Scale(474)), theme.primary);
        DrawText(
            L"系统日历与 ICS 数据层将在下一阶段接入",
            bodyFormat_.Get(),
            D2D1::RectF(Scale(34), Scale(482), Scale(390), Scale(518)),
            theme.secondary);
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
            ApplyDwmAppearance();
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
            const int hit = HitTestCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (hit >= 0)
            {
                selected_ = DateForCell(hit);
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
            ChangeMonth(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            return 0;
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
            SetTimer(window_, kOutsideClickTimer, 120, nullptr);
            return 0;
        case WM_DESTROY:
            RemovePropW(window_, L"WinCal.LastHideReason");
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
    int displayYear_{};
    int displayMonth_{};
    Date selected_{};
    int hoveredCell_{-1};
    float transition_{1.0f};

    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> bodyFormat_;
    ComPtr<IDWriteTextFormat> dayFormat_;

    inline static App* activeInstance_{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    App app;
    return app.Run(instance, showCommand);
}
