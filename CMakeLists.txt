cmake_minimum_required(VERSION 3.10)
project(smallestMusicPlayer LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets LinguistTools Multimedia)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_SOURCE_DIR}/ui)

# ------------------------------
# 跨平台依赖查找
# ------------------------------
include(CheckCXXCompilerFlag)
include(FindPackageHandleStandardArgs) # 用于自定义 find_package 宏

# ------------------------------
# FFMPEG、SDL2 和 TagLib 查找 (Windows vs Linux)
# ------------------------------
if(WIN32)
    # --- Windows 平台配置 ---
    # 假设 FFMPEG、SDL2 和 TagLib 库文件结构如下:
    # C:\ffmpeg\include
    # C:\ffmpeg\lib
    # C:\sdl2\SDL2_2.32.2\include
    # C:\sdl2\SDL2_2.32.2\lib
    # C:\taglib\taglib_2.1.1\include
    # C:\taglib\taglib_2.1.1\lib

    # FFMPEG 路径配置
    set(FFMPEG_ROOT "C:/ffmpeg" CACHE PATH "Path to FFmpeg install directory on Windows.")
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
    set(FFMPEG_LIBRARY_DIRS ${FFMPEG_ROOT}/lib)

    # 手动指定 FFmpeg 库名称
    set(FFMPEG_LIBRARIES
        avformat
        avcodec
        avutil
        swscale
        swresample
        avfilter
    )

    # SDL2 路径配置
    set(SDL2_ROOT "C:/sdl2/SDL2_2.32.2" CACHE PATH "Path to SDL2 install directory on Windows.")
    set(SDL2_INCLUDE_DIRS ${SDL2_ROOT}/include)
    set(SDL2_LIBRARY_DIRS ${SDL2_ROOT}/lib)
    
    # 手动指定 SDL2 库名称
    # 在 Windows 上，通常是 SDL2.lib
    set(SDL2_LIBRARIES SDL2)

    # TagLib 路径配置
    set(TAGLIB_ROOT "C:/Program Files (x86)/taglib" CACHE PATH "Path to TagLib install directory on Windows.")
    set(TAGLIB_INCLUDE_DIRS ${TAGLIB_ROOT}/include)
    set(TAGLIB_LIBRARY_DIRS ${TAGLIB_ROOT}/lib)
    
    # 手动指定 TagLib 库名称
    # 在 Windows 上，通常是 tag.lib
    set(TAGLIB_LIBRARIES tag)

    # 因为 sdbus-c++ 是 Linux/DBus 特有的，我们在 Windows 上禁用它
    set(SDBUSCPP_FOUND FALSE)

    # 检查 FFMPEG、SDL2 和 TagLib 路径是否有效 (可选，但推荐)
    find_package_handle_standard_args(FFMPEG DEFAULT_MSG
        FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES
    )
    find_package_handle_standard_args(SDL2 DEFAULT_MSG
        SDL2_INCLUDE_DIRS SDL2_LIBRARIES
    )
    find_package_handle_standard_args(TAGLIB DEFAULT_MSG
        TAGLIB_INCLUDE_DIRS TAGLIB_LIBRARIES
    )

    if(NOT FFMPEG_FOUND OR NOT SDL2_FOUND OR NOT TAGLIB_FOUND)
        message(FATAL_ERROR "FFmpeg, SDL2 or TagLib not found. Please check paths and library names.")
    endif()

    message(STATUS "Windows mode: FFMPEG, SDL2 and TagLib configured with hardcoded paths.")

else()
    # --- Linux / 其他 POSIX 平台配置 ---
    message(STATUS "Linux/POSIX mode: Using PkgConfig to find dependencies.")
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(FFMPEG REQUIRED
        libavformat
        libavcodec
        libavutil
        libswscale
        libswresample
        libavfilter
    )

    pkg_check_modules(TAGLIB REQUIRED taglib)

    # sdbus-c++ 仅在 Linux 上查找 (用于 MPRIS 等)
    pkg_check_modules(SDBUSCPP REQUIRED sdbus-c++)
endif()

# ------------------------------
# 硬件指令集优化 (AVX2)
# ------------------------------
CHECK_CXX_COMPILER_FLAG("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    # 注意：MSVC 编译器使用不同的 flag，这里仅针对 GCC/Clang/MinGW
    # MSVC 默认支持或使用 /arch:AVX2 选项，但通常在配置 Release 模式优化时可以省略。
    if(NOT MSVC)
        add_compile_options(-mavx2 -mfma)
    endif()
endif()

# ------------------------------
# 通用配置
# ------------------------------

# 仅当库被找到时才包含其头文件
include_directories(
    ${CMAKE_SOURCE_DIR}/inc
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
if(SDBUSCPP_FOUND)
    include_directories(
        ${SDBUSCPP_INCLUDE_DIRS}
    )
endif()

qt_add_executable(smallestMusicPlayer
    WIN32 MACOSX_BUNDLE

    ui/mainwindow.ui

    inc/AudioPlayer.hpp
    inc/FileScanner.hpp
    inc/mainwindow.h
    inc/MetaData.hpp
    inc/mpris_server.hpp
    inc/Playlist.hpp
    inc/Precompiled.h
    inc/stb_image_write.h
    inc/stb_image.h
    inc/MediaController.hpp
    
    src/AudioPlayer.cpp
    src/FileScanner.cpp
    src/main.cpp
    src/mainwindow.cpp
    src/Playlist.cpp
    src/MediaController.cpp
)

# -----------------------------------------------------------------------------
# [原有逻辑] 针对 Debug 和 Release 的差异化编译选项
# -----------------------------------------------------------------------------
if(MSVC)
    # Visual Studio (MSVC) 编译器选项
    # Debug:   /Od (禁用优化), /Zi (生成调试信息)
    # Release: /O2 (最大化速度), /Ob2 (内联扩展)
    target_compile_options(smallestMusicPlayer PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2 /Ob2>
    )
    # [新增] 针对 MSVC 添加 FFMPEG/SDL2/TagLib 库目录
    target_link_directories(smallestMusicPlayer PRIVATE 
        ${FFMPEG_LIBRARY_DIRS} 
        ${SDL2_LIBRARY_DIRS}
        ${TAGLIB_LIBRARY_DIRS}
    )

else()
    # GCC / Clang / MinGW 编译器选项
    # Debug:   -Og (优化调试体验), -g (生成调试符号)
    # Release: -O3 (激进优化), -DNDEBUG (禁用assert)
    target_compile_options(smallestMusicPlayer PRIVATE
        $<$<CONFIG:Debug>:-Og -g>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
endif()

if(WIN32)
    # 注意：这里我们使用 target_link_directories 来指定库目录
    # target_link_directories 在 MinGW 下相当于给 g++ 传递 -L<dir> 选项
    target_link_directories(smallestMusicPlayer PRIVATE 
        ${FFMPEG_LIBRARY_DIRS} 
        ${SDL2_LIBRARY_DIRS}
        ${TAGLIB_LIBRARY_DIRS}
    )
endif()

# [建议] Release 模式下开启 IPO/LTO (链接时优化)
# 这允许编译器在链接阶段跨文件优化（如跨模块内联），对音视频项目性能提升明显
set_property(TARGET smallestMusicPlayer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# -----------------------------------------------------------------------------

# 修复：正确使用 qt_add_translations
# 注意：在 Qt6 中，语法应该是 qt_add_translations(target TS_FILES ...)
qt_add_translations(smallestMusicPlayer
    TS_FILES smallestMusicPlayer_zh_CN.ts
)

target_link_libraries(smallestMusicPlayer
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt6::Multimedia
        ${FFMPEG_LIBRARIES}
        ${SDL2_LIBRARIES}
        ${TAGLIB_LIBRARIES}
)

# 仅在 SDBUSCPP 被找到时才链接它
if(SDBUSCPP_FOUND)
    target_link_libraries(smallestMusicPlayer PRIVATE ${SDBUSCPP_LIBRARIES})
endif()

include(GNUInstallDirs)

install(TARGETS smallestMusicPlayer
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET smallestMusicPlayer
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# 输出调试信息
message(STATUS "FFmpeg include dirs: ${FFMPEG_INCLUDE_DIRS}")
message(STATUS "FFmpeg library dirs: ${FFMPEG_LIBRARY_DIRS}")
message(STATUS "FFmpeg libraries: ${FFMPEG_LIBRARIES}")

message(STATUS "SDL2 include dirs: ${SDL2_INCLUDE_DIRS}")
message(STATUS "SDL2 library dirs: ${SDL2_LIBRARY_DIRS}")
message(STATUS "SDL2 libraries: ${SDL2_LIBRARIES}")

message(STATUS "TagLib include dirs: ${TAGLIB_INCLUDE_DIRS}")
message(STATUS "TagLib library dirs: ${TAGLIB_LIBRARY_DIRS}")
message(STATUS "TagLib libraries: ${TAGLIB_LIBRARIES}")

if(SDBUSCPP_FOUND)
    message(STATUS "SDBUSCPP found: YES")
else()
    message(STATUS "SDBUSCPP found: NO (Excluded from build)")
endif()