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

# ------------------------------
# 跨平台依赖查找
# ------------------------------
include(CheckCXXCompilerFlag)
include(FindPackageHandleStandardArgs)

# ------------------------------
# FFMPEG、SDL2 和 TagLib 查找
# ------------------------------
if(WIN32)
    # --- Windows 平台配置 ---
    set(FFMPEG_ROOT "C:/ffmpeg" CACHE PATH "Path to FFmpeg install directory on Windows.")
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
    set(FFMPEG_LIBRARY_DIRS ${FFMPEG_ROOT}/lib)
    set(FFMPEG_LIBRARIES avformat avcodec avutil swscale swresample avfilter)

    set(SDL2_ROOT "C:/sdl2/SDL2_2.32.2" CACHE PATH "Path to SDL2 install directory on Windows.")
    set(SDL2_INCLUDE_DIRS ${SDL2_ROOT}/include)
    set(SDL2_LIBRARY_DIRS ${SDL2_ROOT}/lib)
    set(SDL2_LIBRARIES SDL2)

    set(TAGLIB_ROOT "C:/msys64/mingw64" CACHE PATH "Path to TagLib install directory on Windows.")
    set(TAGLIB_INCLUDE_DIRS ${TAGLIB_ROOT}/include)
    set(TAGLIB_LIBRARY_DIRS ${TAGLIB_ROOT}/lib)
    set(TAGLIB_LIBRARIES tag)

    set(SDBUSCPP_FOUND FALSE)

    find_package_handle_standard_args(FFMPEG DEFAULT_MSG FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES)
    find_package_handle_standard_args(SDL2 DEFAULT_MSG SDL2_INCLUDE_DIRS SDL2_LIBRARIES)
    find_package_handle_standard_args(TAGLIB DEFAULT_MSG TAGLIB_INCLUDE_DIRS TAGLIB_LIBRARIES)

    if(NOT FFMPEG_FOUND OR NOT SDL2_FOUND OR NOT TAGLIB_FOUND)
        message(FATAL_ERROR "FFmpeg, SDL2 or TagLib not found. Please check paths and library names.")
    endif()

    message(STATUS "Windows mode: FFMPEG, SDL2 and TagLib configured with hardcoded paths.")

else()
    # --- Linux / POSIX ---
    message(STATUS "Linux/POSIX mode: Using PkgConfig to find dependencies.")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale libswresample libavfilter)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(TAGLIB REQUIRED taglib)
    pkg_check_modules(SDBUSCPP REQUIRED sdbus-c++)
    pkg_check_modules(UCHARDET REQUIRED uchardet)
endif()

# ------------------------------
# 通用 Include 配置
# ------------------------------
include_directories(
    ${CMAKE_SOURCE_DIR}/inc
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
    ${UCHARDET_INCLUDE_DIRS}
)

if(SDBUSCPP_FOUND)
    include_directories(${SDBUSCPP_INCLUDE_DIRS})
endif()

add_subdirectory(SingleheaderLibrary)

# 处理资源文件
qt_add_resources(RES qrc/img.qrc)

# ------------------------------
# 定义目标 (Executable)
# ------------------------------
# 优化点：这里只包含入口 main.cpp。
# 其他逻辑代码移交给 qt_add_qml_module 管理，防止重复编译定义。
qt_add_executable(appMusicPlayer
    src/main.cpp
)

target_compile_definitions(appMusicPlayer
    PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:RELEASE>
)

# ------------------------------
# 定义 QML 模块
# ------------------------------
# 优化点：将所有 C++ 核心逻辑源文件放在这里。
# Qt6 会将它们添加到 appMusicPlayer 目标中，并自动处理类型注册。
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
# 目标属性设置
# ------------------------------
if(WIN32)
    set_target_properties(appMusicPlayer PROPERTIES
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        MACOSX_BUNDLE TRUE
        WIN32_EXECUTABLE TRUE
    )
else()
    set_target_properties(appMusicPlayer PROPERTIES
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        MACOSX_BUNDLE TRUE

        # 开启 IPO/LTO (链接时优化) - 对 C++23 音视频项目很重要
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
    )
endif()

target_compile_definitions(appMusicPlayer PRIVATE TAGLIB_STATIC)

# ------------------------------
# 编译选项优化
# ------------------------------
if(MSVC)
    # Debug: /Od, /Zi; Release: /O2, /Ob2
    target_compile_options(appMusicPlayer PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2 /Ob2>
    )

    target_link_directories(appMusicPlayer PRIVATE
        ${FFMPEG_LIBRARY_DIRS}
        ${SDL2_LIBRARY_DIRS}
        ${TAGLIB_LIBRARY_DIRS}
    )
else()
    # GCC/Clang
    target_compile_options(appMusicPlayer PRIVATE
        $<$<CONFIG:Debug>:-Og -g>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )

    if(WIN32)
        target_link_directories(appMusicPlayer PRIVATE
            ${FFMPEG_LIBRARY_DIRS}
            ${SDL2_LIBRARY_DIRS}
            ${TAGLIB_LIBRARY_DIRS}
        )
    endif()
endif()

# -----------------------------------------------------------
# 开启 AVX2 和 FMA 指令集优化
# -----------------------------------------------------------
if(MSVC)
    target_compile_options(appMusicPlayer PRIVATE /arch:AVX2)
else()
    target_compile_options(appMusicPlayer PRIVATE -mavx2 -mfma)
endif()

# ------------------------------
# 链接库 (Linking)
# ------------------------------
target_link_libraries(appMusicPlayer
    PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    ${FFMPEG_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    z
    ${UCHARDET_LIBRARIES}
    MyThirdPartyLibs
)

if(SDBUSCPP_FOUND)
    target_link_libraries(appMusicPlayer PRIVATE ${SDBUSCPP_LIBRARIES})
endif()

# 预编译头配置
# Clangd 将通过 .clangd 配置文件忽略这里的 -include 设置，而读取原始 PCH.h
target_precompile_headers(appMusicPlayer
    PRIVATE
    inc/PCH.h
)

# ------------------------------
# 安装与部署
# ------------------------------
include(GNUInstallDirs)
install(TARGETS appMusicPlayer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
    TARGET appMusicPlayer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# ------------------------------
# 输出调试信息
# ------------------------------
message(STATUS "------------------------------------------------")
message(STATUS "Build Config Summary:")
message(STATUS "FFmpeg libraries: ${FFMPEG_LIBRARIES}")
message(STATUS "SDL2 libraries: ${SDL2_LIBRARIES}")
message(STATUS "TagLib libraries: ${TAGLIB_LIBRARIES}")

if(SDBUSCPP_FOUND)
    message(STATUS "SDBUSCPP: Included")
else()
    message(STATUS "SDBUSCPP: Excluded (Windows or not found)")
endif()

message(STATUS "------------------------------------------------")

# 欺骗 Clangd：给第三方库挂载 Qt 路径
# 这样即使 Clangd 错误地用 LibImpl.cpp 来解析 PCH.h，它也能找到 QImage 等头文件。
# 这不会影响实际编译产物，因为 LibImpl.cpp 代码里并没有 #include <QImage>
find_package(Qt6 COMPONENTS Core Gui Quick) # 确保变量存在
target_link_libraries(MyThirdPartyLibs PRIVATE Qt6::Core Qt6::Gui Qt6::Quick)
