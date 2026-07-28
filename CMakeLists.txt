cmake_minimum_required(VERSION 3.20)
project(Latenci LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Drop the default /O2 /Ob2 so the size-oriented /O1 below is the only
# optimization level passed (previously /O1 won only by argument ordering).
set(CMAKE_CXX_FLAGS_RELEASE "/DNDEBUG")

add_executable(Latenci WIN32
    src/main.cpp
    src/TrayIcon.cpp
    src/PopupWindow.cpp
    src/Fonts.cpp
    src/NetworkMonitor.cpp
    src/SpeedTest.cpp
    src/DnsManager.cpp
    src/Autostart.cpp
    src/UpdateChecker.cpp
    resources/app.rc
)

target_include_directories(Latenci PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_compile_definitions(Latenci PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

# /GL enables whole-program optimization (pairs with /LTCG).
# /GR- drops RTTI: no dynamic_cast or typeid anywhere in the codebase.
# /guard:cf adds Control Flow Guard; this process runs elevated.
target_compile_options(Latenci PRIVATE
    /W4
    /permissive-
    # Sources are UTF-8 without a BOM; without this the em-dash placeholders in
    # Theme.h / NetworkMonitor.cpp / TrayIcon.cpp decode as the ANSI codepage.
    /utf-8
    /EHs-c-
    $<$<CONFIG:Release>:/O1 /Gy /Gw /GL /GR- /guard:cf>
)

target_link_libraries(Latenci PRIVATE
    dwmapi
    d2d1
    dwrite
    iphlpapi
    wlanapi
    ws2_32
    winhttp
    comctl32
    shell32
    ole32
    oleaut32
    gdi32
    user32
    advapi32
    taskschd
)

# Single embedded manifest (UAC + DPI + comctl v6). Suppress linker default UAC snippet.
if(MSVC)
    target_link_options(Latenci PRIVATE
        "/MANIFESTUAC:NO"
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO /LTCG /GUARD:CF /CETCOMPAT>
    )
endif()

# Optional Windows installer (Inno Setup 6 — free: https://jrsoftware.org/isinfo.php)
find_program(ISCC_EXECUTABLE ISCC
    HINTS
        "C:/Program Files (x86)/Inno Setup 6"
        "C:/Program Files/Inno Setup 6"
    PATH_SUFFIXES .
)
if(ISCC_EXECUTABLE)
    add_custom_target(installer
        COMMAND ${ISCC_EXECUTABLE}
            /DMyBuildDir=$<TARGET_FILE_DIR:Latenci>
            ${CMAKE_CURRENT_SOURCE_DIR}/installer/Latenci.iss
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS Latenci
        COMMENT "Building Latenci Windows installer..."
        VERBATIM
    )
    message(STATUS "Inno Setup found: run 'cmake --build build --config Release --target installer'")
else()
    message(STATUS "Inno Setup not found — install from https://jrsoftware.org/isinfo.php to build the installer")
endif()
