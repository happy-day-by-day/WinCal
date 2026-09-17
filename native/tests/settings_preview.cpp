#define WINCAL_SETTINGS_PREVIEW
#include "../src/main.cpp"

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    App app;
    if (argc == 9 && wcscmp(argv[1], L"--calendar") == 0)
    {
        return app.RenderCalendarPreview(GetModuleHandleW(nullptr), _wtoi(argv[2]),
            _wtoi(argv[3]) != 0, _wtoi(argv[4]), _wtoi(argv[5]), _wtoi(argv[6]),
            static_cast<float>(_wtof(argv[7])), argv[8]) ? 0 : 1;
    }
    if (argc != 6) return 2;
    return app.RenderSettingsPreview(GetModuleHandleW(nullptr), _wtoi(argv[1]),
        _wtoi(argv[2]), _wtoi(argv[3]) != 0, _wtoi(argv[4]), argv[5]) ? 0 : 1;
}
