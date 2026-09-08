cmake_minimum_required(VERSION 3.25)

project(WinCalNative VERSION 0.1.0 LANGUAGES CXX RC)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/WinCal.ico" WINCAL_ICON_PATH)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/WinCal.manifest" WINCAL_MANIFEST_PATH)
configure_file(resources/WinCal.rc.in "${CMAKE_CURRENT_BINARY_DIR}/WinCal.rc" @ONLY)

add_executable(WinCalNative WIN32
    src/app_settings.cpp
    src/calendar_data.cpp
    src/lunar_calendar.cpp
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/WinCal.rc"
)

add_executable(WinCalNativeSmoke
    src/app_settings.cpp
    src/lunar_calendar.cpp
    tests/smoke.cpp
)

set_target_properties(WinCalNative PROPERTIES
    OUTPUT_NAME "WinCal"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_features(WinCalNative PRIVATE cxx_std_20)
target_compile_features(WinCalNativeSmoke PRIVATE cxx_std_20)
target_include_directories(WinCalNativeSmoke PRIVATE src)
target_compile_definitions(WinCalNative PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_definitions(WinCalNativeSmoke PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(WinCalNative PRIVATE /utf-8 /W4 /permissive- /EHsc)
    target_compile_options(WinCalNativeSmoke PRIVATE /utf-8 /W4 /permissive- /EHsc)
    target_link_options(WinCalNative PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(WinCalNative PRIVATE
    advapi32
    bcrypt
    comctl32
    d2d1
    dwrite
    dwmapi
    gdi32
    imm32
    ole32
    shell32
    shlwapi
    user32
    winhttp
    windowsapp
)

target_link_libraries(WinCalNativeSmoke PRIVATE
    ole32
    shell32
    windowsapp
)

install(TARGETS WinCalNative RUNTIME DESTINATION .)

# On-demand visual check; excluded from normal builds and installation.
add_executable(WinCalSettingsPreview EXCLUDE_FROM_ALL
    tests/settings_preview.cpp
    src/app_settings.cpp
    src/calendar_data.cpp
    src/lunar_calendar.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/WinCal.rc"
)
target_compile_features(WinCalSettingsPreview PRIVATE cxx_std_20)
target_compile_definitions(WinCalSettingsPreview PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(WinCalSettingsPreview PRIVATE /utf-8 /W4 /permissive- /EHsc)
target_link_options(WinCalSettingsPreview PRIVATE /MANIFEST:NO)
target_link_libraries(WinCalSettingsPreview PRIVATE $<TARGET_PROPERTY:WinCalNative,LINK_LIBRARIES>)
