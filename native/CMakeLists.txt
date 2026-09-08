cmake_minimum_required(VERSION 3.25)

project(WinCalNative VERSION 0.1.0 LANGUAGES CXX RC)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/WinCal.ico" WINCAL_ICON_PATH)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/WinCal.manifest" WINCAL_MANIFEST_PATH)
configure_file(resources/WinCal.rc.in "${CMAKE_CURRENT_BINARY_DIR}/WinCal.rc" @ONLY)

add_executable(WinCalNative WIN32
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/WinCal.rc"
)

set_target_properties(WinCalNative PROPERTIES
    OUTPUT_NAME "WinCal"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_features(WinCalNative PRIVATE cxx_std_20)
target_compile_definitions(WinCalNative PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(WinCalNative PRIVATE /utf-8 /W4 /permissive- /EHsc)
    target_link_options(WinCalNative PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(WinCalNative PRIVATE
    advapi32
    d2d1
    dwrite
    dwmapi
    gdi32
    imm32
    ole32
    shell32
    shlwapi
    user32
)

install(TARGETS WinCalNative RUNTIME DESTINATION .)
