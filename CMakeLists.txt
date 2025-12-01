cmake_minimum_required(VERSION 3.16)

project(MusicPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------
# Qt 依赖查找
# ------------------------------
# QML 项目需要 Quick，保留 Multimedia 和 LinguistTools (用于翻译)

find_package(Qt6 REQUIRED COMPONENTS Quick Multimedia LinguistTools QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)
qt_policy(SET QTP0004 NEW) # 优化 QML 模块的隐式导入行为

# ------------------------------
# 跨平台依赖查找
# ------------------------------
include(CheckCXXCompilerFlag)
include(FindPackageHandleStandardArgs)

# ------------------------------
# FFMPEG、SDL2 和 TagLib 查找 (逻辑保持不变)
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

    set(TAGLIB_ROOT "C:/Program Files (x86)/taglib" CACHE PATH "Path to TagLib install directory on Windows.")
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
endif()

# ------------------------------
# 硬件指令集优化 (AVX2)
# ------------------------------
CHECK_CXX_COMPILER_FLAG("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    if(NOT MSVC)
        add_compile_options(-mavx2 -mfma)
    endif()
endif()

# ------------------------------
# 通用 Include 配置
# ------------------------------
include_directories(
    ${CMAKE_SOURCE_DIR}/inc
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
if(SDBUSCPP_FOUND)
    include_directories(${SDBUSCPP_INCLUDE_DIRS})
endif()

qt_add_resources(RES qrc/img.qrc)

# ------------------------------
# 定义目标 (Executable)
# ------------------------------
qt_add_executable(appMusicPlayer
    # C++ Sources
    inc/AudioPlayer.hpp
    inc/CoverCache.hpp
    inc/CoverImage.hpp
    inc/FileScanner.hpp
    inc/MediaController.hpp
    inc/MetaData.hpp
    inc/mpris_server.hpp
    inc/musiclistmodel.h
    inc/Precompiled.h
    inc/stb_image_resize2.h
    inc/stb_image_write.h
    inc/stb_image.h
    inc/SysMediaService.hpp

    src/AudioPlayer.cpp
    src/CoverCache.cpp
    src/FileScanner.cpp
    src/main.cpp
    src/MediaController.cpp
    src/musiclistmodel.cpp
    src/StbImpl.cpp
    src/SysMediaService.cpp

)

# ------------------------------
# 定义 QML 模块
# ------------------------------
qt_add_qml_module(appMusicPlayer
    URI MusicPlayer
    VERSION 1.0
    SOURCES ${RES}
    QML_FILES
        qml/Main.qml
        qml/ListView.qml
        qml/StyleButton.qml
        qml/MusicListHeader.qml
        qml/MusicListItemDelegate.qml
        qml/MusicListView.qml
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
        WIN32_EXECUTABLE TRUE
        # 开启 IPO/LTO (链接时优化) - 这一步对 C++23 音视频项目很重要
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
    )
endif()

target_compile_definitions(appMusicPlayer PRIVATE TAGLIB_STATIC)

# ------------------------------
# 编译选项优化 (移植自原项目)
# ------------------------------
if(MSVC)
    # Debug: /Od, /Zi; Release: /O2, /Ob2
    target_compile_options(appMusicPlayer PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2 /Ob2>
    )
    # [关键] 必须指定库目录，否则 linker 找不到 .lib 文件
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
    # MinGW 也需要指定目录
    if(WIN32)
        target_link_directories(appMusicPlayer PRIVATE 
            ${FFMPEG_LIBRARY_DIRS} 
            ${SDL2_LIBRARY_DIRS}
            ${TAGLIB_LIBRARY_DIRS}
        )
    endif()
endif()

# ------------------------------
# 链接库 (Linking)
# ------------------------------
target_link_libraries(appMusicPlayer
    PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
        ${FFMPEG_LIBRARIES}
        ${SDL2_LIBRARIES}
        ${TAGLIB_LIBRARIES}
        z
)

# 仅在 SDBUSCPP 被找到时才链接它 (Linux)
if(SDBUSCPP_FOUND)
    target_link_libraries(appMusicPlayer PRIVATE ${SDBUSCPP_LIBRARIES})
endif()

# ------------------------------
# 翻译文件 (移植自原项目)
# ------------------------------
# 确保你的 .ts 文件名正确
qt_add_translations(appMusicPlayer
    TS_FILES smallestMusicPlayer_zh_CN.ts
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

# 生成部署脚本 (windeployqt / macdeployqt)
# 这对于 QML 项目尤为重要，因为需要拷贝大量 QML 插件
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