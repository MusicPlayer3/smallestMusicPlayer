cmake_minimum_required(VERSION 3.16)

project(MusicPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(ENABLE_COMPILER_CACHE "Enable compiler cache (ccache/sccache)" ON)

if(ENABLE_COMPILER_CACHE)
    find_program(COMPILER_CACHE NAMES ccache sccache)
    if(COMPILER_CACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER ${COMPILER_CACHE})
        message(STATUS "Compiler cache enabled: ${COMPILER_CACHE}")
    endif()
endif()

# ------------------------------
# Qt 依赖查找
# ------------------------------
find_package(Qt6 REQUIRED COMPONENTS Quick Multimedia LinguistTools QuickControls2 Widgets Concurrent)
qt_standard_project_setup(REQUIRES 6.8)
qt_policy(SET QTP0004 NEW)

include(CheckCXXCompilerFlag)
include(FindPackageHandleStandardArgs)

# ------------------------------
# 依赖库查找 (针对外部编译器适配 MSYS2)
# ------------------------------
if(WIN32)
    # 1. FFMPEG: 手动路径 
    set(FFMPEG_ROOT "C:/ffmpeg" CACHE PATH "Path to FFmpeg install directory on Windows.")
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBRARIES avformat avcodec avutil swscale swresample avfilter)
    find_package_handle_standard_args(FFMPEG DEFAULT_MSG FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES)

    # 2. MSYS2 库适配 
    set(MSYS64_PREFIX "C:/msys64/mingw64")
    list(APPEND CMAKE_PREFIX_PATH "${MSYS64_PREFIX}")

    # --- TagLib ---
    # 显式查找包含目录，确保能找到版本宏定义头文件 
    find_path(TAGLIB_INCLUDE_DIRS NAMES tag.h PATH_SUFFIXES taglib PATHS "${MSYS64_PREFIX}/include")
    find_library(TAGLIB_LIBRARIES NAMES tag PATHS "${MSYS64_PREFIX}/lib")
    find_package_handle_standard_args(TAGLIB DEFAULT_MSG TAGLIB_INCLUDE_DIRS TAGLIB_LIBRARIES)

    # --- uchardet ---
    find_path(UCHARDET_INCLUDE_DIRS NAMES uchardet.h PATH_SUFFIXES uchardet PATHS "${MSYS64_PREFIX}/include")
    find_library(UCHARDET_LIBRARIES NAMES uchardet PATHS "${MSYS64_PREFIX}/lib")
    find_package_handle_standard_args(UCHARDET DEFAULT_MSG UCHARDET_INCLUDE_DIRS UCHARDET_LIBRARIES)

    # --- spdlog ---
    find_package(spdlog CONFIG REQUIRED)

    set(SDBUSCPP_FOUND FALSE)
else()
    # --- Linux / POSIX --- 
    message(STATUS "Linux/POSIX mode: Using PkgConfig to find dependencies.")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale libswresample libavfilter)
    pkg_check_modules(TAGLIB REQUIRED taglib)
    pkg_check_modules(SDBUSCPP REQUIRED sdbus-c++)
    pkg_check_modules(UCHARDET REQUIRED uchardet)
    pkg_check_modules(SPDLOG REQUIRED spdlog)
endif()

# ------------------------------
# 定义目标
# ------------------------------
add_subdirectory(SingleheaderLibrary)
qt_add_resources(RES qrc/img.qrc)

qt_add_executable(appMusicPlayer src/main.cpp)

# ------------------------------
# 配置 Include 路径 
# ------------------------------
target_include_directories(appMusicPlayer PRIVATE
    ${CMAKE_SOURCE_DIR}/inc
    ${FFMPEG_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
    ${UCHARDET_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS}
)

# ------------------------------
# 定义 QML 模块 [cite: 5, 6]
# ------------------------------
qt_add_qml_module(appMusicPlayer
    URI MusicPlayer
    VERSION 1.0
    SOURCES
    ${RES}

    # C++ Headers
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

    # C++ Sources
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

# ------------------------------
# 预编译头 (PCH) 
# ------------------------------
target_precompile_headers(appMusicPlayer PRIVATE inc/PCH.h)

# ------------------------------
# 链接与优化
# ------------------------------
target_compile_definitions(appMusicPlayer PRIVATE TAGLIB_STATIC)

if(MSVC)
    target_compile_options(appMusicPlayer PRIVATE /arch:AVX2 $<$<CONFIG:Debug>:/Od /Zi> $<$<CONFIG:Release>:/O2 /Ob2>)
else()
    target_compile_options(appMusicPlayer PRIVATE -mavx2 -mfma $<$<CONFIG:Debug>:-Og -g> $<$<CONFIG:Release>:-O3 -DNDEBUG>)
endif()

if(WIN32)
    set_target_properties(appMusicPlayer PROPERTIES
        WIN32_EXECUTABLE TRUE  # 重新开启，不显示控制台
    )
    
    # 强制 MinGW 编译器使用 Windows GUI 模式并指定入口
    # -mwindows 会告诉链接器这是一个 GUI 程序（不带控制台）
    # -u WinMain 确保链接器强制寻找 WinMain
    target_link_options(appMusicPlayer PRIVATE -mwindows)
endif()

target_link_libraries(appMusicPlayer
    PRIVATE
    Qt6::Quick
    Qt6::QuickControls2 
    Qt6::Widgets
    $<$<PLATFORM_ID:Windows>:Qt6::EntryPointImplementation>
    ${FFMPEG_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ${UCHARDET_LIBRARIES}
    spdlog::spdlog
    MyThirdPartyLibs
    z
)

if(WIN32 AND NOT MSVC)
    # 显式链接动态库目录 
    target_link_directories(appMusicPlayer PRIVATE "${MSYS64_PREFIX}/lib" ${FFMPEG_LIBRARY_DIRS})
endif()

# ------------------------------
# 安装与部署 [cite: 11]
# ------------------------------
include(GNUInstallDirs)
install(TARGETS appMusicPlayer BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_app_script(TARGET appMusicPlayer OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})

# 调试信息
message(STATUS "------------------------------------------------")
message(STATUS "TagLib Include: ${TAGLIB_INCLUDE_DIRS}")
message(STATUS "Uchardet Include: ${UCHARDET_INCLUDE_DIRS}")
message(STATUS "------------------------------------------------")

find_package(Qt6 COMPONENTS Core Gui Quick)
target_link_libraries(MyThirdPartyLibs PRIVATE Qt6::Core Qt6::Gui Qt6::Quick)