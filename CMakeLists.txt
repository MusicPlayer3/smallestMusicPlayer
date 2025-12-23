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

# --- OpenCV 查找配置 (通用) ---
# 我们只需要核心(core)、图像处理(imgproc)和编解码(imgcodecs)
# 这部分逻辑在 Windows(MSYS2) 和 Linux 下通常是通用的，
# 但为了配合你现有的结构，我将其放入下方的条件块中以便管理路径。
if(WIN32)
    message(STATUS "Configuring for Windows (Qt Official MinGW + MSYS2 Libs strategy)...")

    # --- 1. 路径定义 ---
    set(FFMPEG_ROOT "C:/ffmpeg" CACHE PATH "Path to FFmpeg install directory.")
    set(MSYS64_PREFIX "C:/msys64/mingw64" CACHE PATH "Path to MSYS2 MinGW64 prefix.")

    # 添加 MSYS2 前缀到查找路径，这对 find_package(OpenCV) 至关重要
    list(APPEND CMAKE_PREFIX_PATH "${MSYS64_PREFIX}")

    # --- 2. FFmpeg (Manual) ---
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBS avformat avcodec avutil swscale swresample avfilter)

    find_package_handle_standard_args(FFMPEG DEFAULT_MSG FFMPEG_INCLUDE_DIRS FFMPEG_LIBS)

    list(APPEND PROJECT_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIRS}")
    list(APPEND PROJECT_LIBRARY_DIRS "${FFMPEG_LIBRARY_DIRS}")
    list(APPEND PROJECT_LIBRARIES ${FFMPEG_LIBS})

    # --- 3. MSYS2 库 ---

    # [新增] OpenCV (通过 MSYS2 的 Config 模式查找)
    # 只要 CMAKE_PREFIX_PATH 包含了 mingw64 路径，这里就能自动找到
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
    message(STATUS "OpenCV Windows Found: ${OpenCV_VERSION}")

    list(APPEND PROJECT_INCLUDE_DIRS ${OpenCV_INCLUDE_DIRS})
    list(APPEND PROJECT_LIBRARIES ${OpenCV_LIBS})

    # TagLib
    find_path(TAGLIB_INCLUDE_DIRS NAMES tag.h PATH_SUFFIXES taglib PATHS "${MSYS64_PREFIX}/include")
    find_library(TAGLIB_LIBRARIES NAMES tag PATHS "${MSYS64_PREFIX}/lib")
    find_package_handle_standard_args(TAGLIB DEFAULT_MSG TAGLIB_INCLUDE_DIRS TAGLIB_LIBRARIES)

    list(APPEND PROJECT_INCLUDE_DIRS "${TAGLIB_INCLUDE_DIRS}")
    list(APPEND PROJECT_LIBRARIES ${TAGLIB_LIBRARIES})

    # Uchardet
    find_path(UCHARDET_INCLUDE_DIRS NAMES uchardet.h PATH_SUFFIXES uchardet PATHS "${MSYS64_PREFIX}/include")
    find_library(UCHARDET_LIBRARIES NAMES uchardet PATHS "${MSYS64_PREFIX}/lib")
    find_package_handle_standard_args(UCHARDET DEFAULT_MSG UCHARDET_INCLUDE_DIRS UCHARDET_LIBRARIES)

    list(APPEND PROJECT_INCLUDE_DIRS "${UCHARDET_INCLUDE_DIRS}")
    list(APPEND PROJECT_LIBRARIES ${UCHARDET_LIBRARIES})

    # spdlog
    find_package(spdlog CONFIG REQUIRED)
    list(APPEND PROJECT_LIBRARIES spdlog::spdlog)

    set(SDBUSCPP_FOUND FALSE)

    # 补充库
    list(APPEND PROJECT_LIBRARIES z)
    list(APPEND PROJECT_LIBRARY_DIRS "${MSYS64_PREFIX}/lib")

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

qt_add_executable(appMusicPlayer src/main.cpp)

qt_add_qml_module(appMusicPlayer
    URI MusicPlayer
    VERSION 1.0
    SOURCES
    ${RES}

    # Headers
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
target_compile_definitions(appMusicPlayer PRIVATE TAGLIB_STATIC)

# 4. Compile Options
if(MSVC)
    target_compile_options(appMusicPlayer PRIVATE
        /arch:AVX2
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2 /Ob2 /GL> # /GL 开启全程序优化
    )

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

    # 开启 LTO (Link Time Optimization) - 对 MinGW Release 构建同样有效
    # 注意：如果遇到 LTO 报错 (lto-wrapper failed)，请注释掉下面这行
    set_property(TARGET appMusicPlayer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# 5. Link Options & Libraries
if(WIN32)
    set_target_properties(appMusicPlayer PROPERTIES WIN32_EXECUTABLE TRUE)

    # 确保 MinGW 链接器能找到手动指定的库路径
    target_link_directories(appMusicPlayer PRIVATE ${PROJECT_LIBRARY_DIRS})

    if(NOT MSVC)
        # -mwindows: 隐藏控制台窗口
        # -s: Release 模式下剥离符号，减小体积
        target_link_options(appMusicPlayer PRIVATE -mwindows $<$<CONFIG:Release>:-s>)
    endif()
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