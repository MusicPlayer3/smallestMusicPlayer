# ==============================================================================
# 项目元数据与基础配置
# ==============================================================================
cmake_minimum_required(VERSION 3.16)

project(MusicPlayer VERSION 0.1 LANGUAGES CXX)

# C++ 标准设置 (C++23)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # 禁用编译器特定扩展 (-std=c++23 而非 -std=gnu++23)

# 选项配置
option(ENABLE_COMPILER_CACHE "Enable compiler cache (ccache/sccache)" ON)

# 启用编译器缓存 (ccache/sccache)
if(ENABLE_COMPILER_CACHE)
    find_program(COMPILER_CACHE NAMES ccache sccache)

    if(COMPILER_CACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER ${COMPILER_CACHE})
        message(STATUS "Compiler cache enabled: ${COMPILER_CACHE}")
    endif()
endif()

set(WIN_MANIFEST "${CMAKE_SOURCE_DIR}/app.manifest")

# ==============================================================================
# Qt 依赖查找
# ==============================================================================
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui Quick QuickControls2 Widgets
    Multimedia Concurrent LinguistTools
    Sql
)

qt_standard_project_setup(REQUIRES 6.8)
qt_policy(SET QTP0004 NEW)

# 引入辅助模块
include(CheckCXXCompilerFlag)
include(FindPackageHandleStandardArgs)
include(GNUInstallDirs)

# ==============================================================================
# 第三方库依赖查找 (平台适配核心逻辑)
# ==============================================================================
set(PROJECT_INCLUDE_DIRS "")
set(PROJECT_LIBRARY_DIRS "")
set(PROJECT_LIBRARIES "")

# --- 平台依赖分发 ---
if(WIN32)
    message(STATUS "Configuring for Windows (MSVC + Vcpkg strategy)...")

    # --------------------------------------------------------------------------
    # 1. OpenCV
    # Vcpkg 通常提供 OpenCVConfig.cmake，直接查找即可
    # --------------------------------------------------------------------------
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
    message(STATUS "OpenCV Windows Found: ${OpenCV_VERSION}")

    list(APPEND PROJECT_INCLUDE_DIRS ${OpenCV_INCLUDE_DIRS})
    list(APPEND PROJECT_LIBRARIES ${OpenCV_LIBS})

    # --------------------------------------------------------------------------
    # 2. FFmpeg
    # Vcpkg 安装的 FFmpeg 通常没有标准的 CMake Config，或者需要 pkg-config。
    # 为了保证 MSVC 下的稳健性，这里使用标准 find_library 查找 vcpkg 目录下的 .lib
    # --------------------------------------------------------------------------
    set(FFMPEG_COMPONENTS avformat avcodec avutil swscale swresample avfilter)
    set(FFMPEG_FOUND TRUE)

    # 查找头文件路径 (检查其中一个即可)
    find_path(FFMPEG_INCLUDE_DIR
        NAMES libavformat/avformat.h
        DOC "Path to FFmpeg headers"
    )

    if(FFMPEG_INCLUDE_DIR)
        list(APPEND PROJECT_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
    endif()

    # 查找各个库文件
    foreach(_comp ${FFMPEG_COMPONENTS})
        find_library(FFMPEG_${_comp}_LIBRARY
            NAMES ${_comp}
            DOC "FFmpeg ${_comp} library"
        )

        if(FFMPEG_${_comp}_LIBRARY)
            list(APPEND PROJECT_LIBRARIES ${FFMPEG_${_comp}_LIBRARY})
            message(STATUS "Found FFmpeg component: ${_comp}")
        else()
            set(FFMPEG_FOUND FALSE)
            message(WARNING "Could NOT find FFmpeg component: ${_comp}")
        endif()
    endforeach()

    if(NOT FFMPEG_FOUND)
        message(FATAL_ERROR "FFmpeg is required but some components were not found via Vcpkg.")
    endif()

    # --------------------------------------------------------------------------
    # 3. TagLib
    # Vcpkg 提供 TagLibConfig.cmake，导出目标通常为 TagLib::tag
    # --------------------------------------------------------------------------
    find_package(TagLib REQUIRED)

    if(TARGET TagLib::tag)
        list(APPEND PROJECT_LIBRARIES TagLib::tag)
    else()
        list(APPEND PROJECT_INCLUDE_DIRS ${TAGLIB_INCLUDE_DIRS})
        list(APPEND PROJECT_LIBRARIES ${TAGLIB_LIBRARIES})
    endif()

    # --------------------------------------------------------------------------
    # 4. Uchardet
    # Vcpkg 提供 uchardetConfig.cmake，导出目标 uchardet::libuchardet
    # --------------------------------------------------------------------------
    find_package(uchardet CONFIG REQUIRED)
    list(APPEND PROJECT_LIBRARIES uchardet::libuchardet)

    # --------------------------------------------------------------------------
    # 5. spdlog
    # Vcpkg 提供 spdlogConfig.cmake，导出目标 spdlog::spdlog
    # --------------------------------------------------------------------------
    find_package(spdlog CONFIG REQUIRED)
    list(APPEND PROJECT_LIBRARIES spdlog::spdlog)

    # --------------------------------------------------------------------------
    # 6. ZLib (vcpkg 默认包含)
    # --------------------------------------------------------------------------
    find_package(ZLIB REQUIRED)
    list(APPEND PROJECT_LIBRARIES ZLIB::ZLIB)

# 注意: sdbus-c++ (DBus) 是 Linux 特有机制，Windows 下通常不包含此库。
# 请确保代码中使用 #ifndef _WIN32 包裹了 DBus 相关逻辑。
else()
    # --- Linux / POSIX 模式 (PkgConfig) ---
    message(STATUS "Configuring for Linux/POSIX (PkgConfig strategy)...")
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale libswresample libavfilter)
    pkg_check_modules(TAGLIB REQUIRED taglib)
    pkg_check_modules(SDBUSCPP REQUIRED sdbus-c++)
    pkg_check_modules(UCHARDET REQUIRED uchardet)
    pkg_check_modules(SPDLOG REQUIRED spdlog)

    # [新增] OpenCV (Linux 下推荐优先使用 find_package 而不是 pkg-config)
    # 如果系统没有 CMake Config，可以回退使用 pkg_check_modules(OPENCV opencv4)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
    message(STATUS "OpenCV Linux Found: ${OpenCV_VERSION}")

    # 聚合 Include 路径
    list(APPEND PROJECT_INCLUDE_DIRS
        ${FFMPEG_INCLUDE_DIRS}
        ${TAGLIB_INCLUDE_DIRS}
        ${SDBUSCPP_INCLUDE_DIRS}
        ${UCHARDET_INCLUDE_DIRS}
        ${SPDLOG_INCLUDE_DIRS}
        ${OpenCV_INCLUDE_DIRS} # OpenCV
    )

    # 聚合 Libraries
    list(APPEND PROJECT_LIBRARIES
        ${FFMPEG_LIBRARIES}
        ${TAGLIB_LIBRARIES}
        ${SDBUSCPP_LIBRARIES}
        ${UCHARDET_LIBRARIES}
        ${SPDLOG_LIBRARIES}
        ${OpenCV_LIBS} # OpenCV
        z
    )
endif()

# ==============================================================================
# 子模块定义
# ==============================================================================
add_subdirectory(SingleheaderLibrary)

if(TARGET MyThirdPartyLibs)
    target_link_libraries(MyThirdPartyLibs PRIVATE Qt6::Core Qt6::Gui Qt6::Quick)
endif()

# ==============================================================================
# 定义主目标 (Executable & QML Module)
# ==============================================================================
qt_add_resources(RES qrc/img.qrc)

qt_add_executable(appMusicPlayer
    src/main.cpp
    ${WIN_MANIFEST}
)

qt_add_qml_module(appMusicPlayer
    URI MusicPlayer
    VERSION 1.0
    SOURCES
    ${RES}

    # Headers
    inc/AudioFilterChain.hpp
    inc/AudioPlayer.hpp
    inc/CoverCache.hpp
    inc/CoverImage.hpp
    inc/CoverImageProvider.hpp
    inc/FileScanner.hpp
    inc/MediaController.hpp
    inc/MetaData.hpp
    inc/musiclistmodel.h
    inc/PCH.h
    inc/SimpleThreadPool.hpp
    inc/SysMediaService.hpp
    inc/uicontroller.h
    inc/DatabaseService.hpp

    # Sources
    src/AudioFilterChain.cpp
    src/AudioPlayer.cpp
    src/CoverCache.cpp
    src/CoverImageProvider.cpp
    src/FileScanner.cpp
    src/MediaController.cpp
    src/musiclistmodel.cpp
    src/SysMediaService.cpp
    src/UIController.cpp
    src/DatabaseService.cpp

    QML_FILES
    qml/Background.qml
    qml/BubblePopup.qml
    qml/Main.qml
    qml/MarqueeText.qml
    qml/MusicListHeader.qml
    qml/MusicListItemDelegate.qml
    qml/MusicListView.qml
    qml/OutputSettingsWindow.qml
    qml/StyleButton.qml
    qml/WaveformProgressBar.qml
    qml/StartupPage.qml
    qml/DeleteDialog.qml
    qml/InfoDialog.qml
)

# ==============================================================================
# 目标配置
# ==============================================================================

# 为 Debug 模式定义 DEBUG 宏
target_compile_definitions(appMusicPlayer PRIVATE $<$<CONFIG:Debug>:DEBUG>)

# 为 Release 模式定义 RELEASE 宏
target_compile_definitions(appMusicPlayer PRIVATE $<$<CONFIG:Release>:RELEASE>)

# 1. Include Directories
target_include_directories(appMusicPlayer PRIVATE
    ${CMAKE_SOURCE_DIR}/inc
    ${PROJECT_INCLUDE_DIRS}
)

# 2. Precompiled Headers
target_precompile_headers(appMusicPlayer PRIVATE inc/PCH.h)

# 3. Compile Definitions
# TAGLIB_STATIC 宏在使用 vcpkg 的静态库时通常是需要的，如果是动态库也无害
# target_compile_definitions(appMusicPlayer PRIVATE TAGLIB_STATIC)

# 4. Compile Options
if(MSVC)
    target_compile_options(appMusicPlayer PRIVATE
        /arch:AVX2
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2 /Ob2 /GL> # /GL 开启全程序优化
    )
    target_compile_options(appMusicPlayer PRIVATE "/utf-8")

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_property(TARGET appMusicPlayer PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        target_link_options(appMusicPlayer PRIVATE /LTCG)
    endif()
else()
    # GCC / MinGW / Clang
    target_compile_options(appMusicPlayer PRIVATE
        -mavx2 -mfma
        $<$<CONFIG:Debug>:-Og -g>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )

    # 开启 LTO (Link Time Optimization)
    set_property(TARGET appMusicPlayer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# 5. Link Options & Libraries
if(WIN32)
    set_target_properties(appMusicPlayer PROPERTIES WIN32_EXECUTABLE TRUE)

    # MSVC / Vcpkg 不需要手动 target_link_directories，CMake 会自动处理
    # 也不需要 -mwindows 或 -s，因为 WIN32_EXECUTABLE 会处理子系统设置
endif()

target_link_libraries(appMusicPlayer
    PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
    $<$<PLATFORM_ID:Windows>:Qt6::EntryPointImplementation>
    MyThirdPartyLibs
    ${PROJECT_LIBRARIES}
)

# ==============================================================================
# 安装与部署
# ==============================================================================
install(TARGETS appMusicPlayer BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET appMusicPlayer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# ==============================================================================
# 调试信息汇总
# ==============================================================================
message(STATUS "============================================================")
message(STATUS "Build Configuration: ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenCV: ${OpenCV_VERSION}")
message(STATUS "Project Libraries: ${PROJECT_LIBRARIES}")
message(STATUS "============================================================")