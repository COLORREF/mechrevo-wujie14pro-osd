cmake_minimum_required(VERSION 3.20)

project(YaoOsd
    VERSION 1.1.0
    DESCRIPTION "Fn hotkey OSD for MECHREVO WUJIE14 Pro"
    LANGUAGES CXX)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install"
        CACHE PATH "安装前缀（默认在项目内的 install/）" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "$<IF:$<CONFIG:Release>,MultiThreaded,MultiThreaded$<$<CONFIG:Debug>:Debug>DLL>")
add_link_options("$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/icon_svg_data.h")
    message(FATAL_ERROR
        "缺少 src/icon_svg_data.h —— 请先运行：\n"
        "    python tools/gen_icon_svg_header.py")
endif()

add_executable(osd_hotkey WIN32 src/osd_hotkey.cpp)
target_compile_definitions(osd_hotkey PRIVATE UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(osd_hotkey PRIVATE /utf-8)
target_link_options(osd_hotkey PRIVATE "/DELAYLOAD:d2d1.dll")
target_link_libraries(osd_hotkey PRIVATE
    user32 gdi32 shell32 ole32 oleaut32 advapi32 wbemuuid gdiplus wtsapi32
    d2d1 delayimp)

set(OSD_SETUP_MSI "" CACHE FILEPATH "内嵌到 EXE 安装器中的 MSI 路径")
if(OSD_SETUP_MSI)
    if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        message(FATAL_ERROR "发行包要求使用 x64 MSVC 工具链")
    endif()
    enable_language(RC)

    add_library(osd_setup_check MODULE EXCLUDE_FROM_ALL packaging/check_folder.cpp)
    target_link_libraries(osd_setup_check PRIVATE msi)

    file(TO_CMAKE_PATH "${OSD_SETUP_MSI}" OSD_SETUP_MSI)
    configure_file(packaging/setup_payload.rc.in setup_payload.rc.in @ONLY)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc.in"
                "${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc"
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc.in" "${OSD_SETUP_MSI}"
        VERBATIM)
    add_executable(osd_setup WIN32 EXCLUDE_FROM_ALL
        packaging/setup.cpp "${CMAKE_CURRENT_BINARY_DIR}/setup_payload.rc")
    target_compile_definitions(osd_setup PRIVATE UNICODE _UNICODE)
    target_link_libraries(osd_setup PRIVATE msi ole32 user32)
    target_compile_options(osd_setup PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

install(TARGETS osd_hotkey RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION .)
install(FILES LICENSE DESTINATION .)
