#define WINCAL_SETTINGS_PREVIEW
#include "../src/main.cpp"

int wmain(int argc, wchar_t** argv)
{
    if (argc != 6) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    App app;
    return app.RenderSettingsPreview(GetModuleHandleW(nullptr), _wtoi(argv[1]),
        _wtoi(argv[2]), _wtoi(argv[3]) != 0, _wtoi(argv[4]), argv[5]) ? 0 : 1;
}
