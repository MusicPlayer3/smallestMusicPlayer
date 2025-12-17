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
# 注意：Windows 下需要确保 CMAKE_PREFIX_PATH 包含 Qt 官方套件路径
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui Quick QuickControls2 Widgets
    Multimedia Concurrent LinguistTools
)

qt_standard_project_setup(REQUIRES 6.8)
qt_policy(SET QTP0004 NEW) # 使得 qt_add_qml_module 能够处理资源路径

# 引入辅助模块
include(CheckCXXCompilerFlag)
include(FindPackageHandleStandardArgs)
include(GNUInstallDirs)

# ==============================================================================
# 第三方库依赖查找 (平台适配核心逻辑)
# ==============================================================================
# 定义统一的列表变量，用于后续统一链接
set(PROJECT_INCLUDE_DIRS "")
set(PROJECT_LIBRARY_DIRS "")
set(PROJECT_LIBRARIES "")

if(WIN32)
    message(STATUS "Configuring for Windows (Qt Official MinGW + MSYS2 Libs strategy)...")

    # --- 1. 路径定义 (Cache 变量，方便在命令行覆盖) ---
    set(FFMPEG_ROOT "C:/ffmpeg" CACHE PATH "Path to FFmpeg install directory.")
    set(MSYS64_PREFIX "C:/msys64/mingw64" CACHE PATH "Path to MSYS2 MinGW64 prefix.")

    # 添加 MSYS2 前缀到查找路径
    list(APPEND CMAKE_PREFIX_PATH "${MSYS64_PREFIX}")

    # --- 2. FFmpeg (Manual) ---
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBS avformat avcodec avutil swscale swresample avfilter)

    # 验证 FFmpeg
    find_package_handle_standard_args(FFMPEG DEFAULT_MSG FFMPEG_INCLUDE_DIRS FFMPEG_LIBS)

    list(APPEND PROJECT_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIRS}")
    list(APPEND PROJECT_LIBRARY_DIRS "${FFMPEG_LIBRARY_DIRS}")
    list(APPEND PROJECT_LIBRARIES ${FFMPEG_LIBS})

    # --- 3. MSYS2 库 (TagLib, Uchardet) ---
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

    # spdlog (Windows 下通常有 Config 文件)
    find_package(spdlog CONFIG REQUIRED)
    list(APPEND PROJECT_LIBRARIES spdlog::spdlog)

    # Windows 下通常不需要 sdbus-c++，或者很难配置
    set(SDBUSCPP_FOUND FALSE)

    # 补充库：zlib (TagLib/FFmpeg 静态链接可能依赖)
    list(APPEND PROJECT_LIBRARIES z)

    # 补充路径：MSYS2 lib 路径，防止链接器找不到依赖的依赖
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

    # 聚合 Include 路径
    list(APPEND PROJECT_INCLUDE_DIRS
        ${FFMPEG_INCLUDE_DIRS}
        ${TAGLIB_INCLUDE_DIRS}
        ${SDBUSCPP_INCLUDE_DIRS}
        ${UCHARDET_INCLUDE_DIRS}
        ${SPDLOG_INCLUDE_DIRS}
    )

    # 聚合 Libraries
    list(APPEND PROJECT_LIBRARIES
        ${FFMPEG_LIBRARIES}
        ${TAGLIB_LIBRARIES}
        ${SDBUSCPP_LIBRARIES}
        ${UCHARDET_LIBRARIES}
        ${SPDLOG_LIBRARIES}
        z
    )
endif()

# ==============================================================================
# 子模块定义
# ==============================================================================
add_subdirectory(SingleheaderLibrary)

# 处理 MyThirdPartyLibs 的依赖 (原文件末尾的代码移至此处)
# 假设 SingleheaderLibrary 目标名为 MyThirdPartyLibs
if(TARGET MyThirdPartyLibs)
    target_link_libraries(MyThirdPartyLibs PRIVATE Qt6::Core Qt6::Gui Qt6::Quick)
endif()

# ==============================================================================
# 定义主目标 (Executable & QML Module)
# ==============================================================================
# QRC 资源
qt_add_resources(RES qrc/img.qrc)

qt_add_executable(appMusicPlayer src/main.cpp)

qt_add_qml_module(appMusicPlayer
    URI MusicPlayer
    VERSION 1.0
    SOURCES
    ${RES}

    # --- C++ Headers ---
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

    # --- C++ Sources ---
    src/AudioPlayer.cpp
    src/CoverCache.cpp
    src/CoverImageProvider.cpp
    src/FileScanner.cpp
    src/MediaController.cpp
    src/musiclistmodel.cpp
    src/SysMediaService.cpp
    src/UIController.cpp
    QML_FILES
    qml/Background.qml
    qml/BubblePopup.qml
    qml/ListView.qml
    qml/Main.qml
    qml/MarqueeText.qml
    qml/MusicListHeader.qml
    qml/MusicListItemDelegate.qml
    qml/MusicListView.qml
    qml/OutputSettingsWindow.qml
    qml/StyleButton.qml
    qml/WaveformProgressBar.qml
)

# ==============================================================================
# 目标配置 (Include, Compile, Link)
# ==============================================================================

# 1. Include Directories
target_include_directories(appMusicPlayer PRIVATE
    ${CMAKE_SOURCE_DIR}/inc
    ${PROJECT_INCLUDE_DIRS}
)

# 2. Precompiled Headers
target_precompile_headers(appMusicPlayer PRIVATE inc/PCH.h)

# 3. Compile Definitions
target_compile_definitions(appMusicPlayer PRIVATE TAGLIB_STATIC)

# 4. Compile Options (Optimization & AVX)
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

    # Windows 下通常需要 EntryPoint 来处理 WinMain
    $<$<PLATFORM_ID:Windows>:Qt6::EntryPointImplementation>
    MyThirdPartyLibs # 你的内部库
    ${PROJECT_LIBRARIES} # 之前统一收集的第三方库
)

# ==============================================================================
# 安装与部署 (Deployment)
# ==============================================================================
install(TARGETS appMusicPlayer BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# 生成部署脚本 (windeployqt / macdeployqt)
# 注意：在混合环境(MinGW Qt + MSYS2 Libs)下，deploy 脚本可能无法自动打包 MSYS2 的 DLL
# 你可能需要手动拷贝 MSYS2 的 DLL (如 libtag.dll, avcodec-*.dll) 到构建目录
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
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Project Include Dirs: ${PROJECT_INCLUDE_DIRS}")
message(STATUS "Project Libraries: ${PROJECT_LIBRARIES}")
message(STATUS "============================================================")