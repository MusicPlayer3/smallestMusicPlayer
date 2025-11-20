cmake_minimum_required(VERSION 3.10)
project(smallestMusicPlayer LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets LinguistTools Multimedia)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_SOURCE_DIR}/ui)

# ------------------------------
# 使用 pkg-config 查找 FFMPEG 和 SDL2
# ------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
    libavfilter
)

pkg_check_modules(SDBUSCPP REQUIRED sdbus-c++)

include(CheckCXXCompilerFlag)

# ------------------------------
# 硬件指令集优化 (AVX2)
# ------------------------------
CHECK_CXX_COMPILER_FLAG("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    add_compile_options(-mavx2 -mfma)
endif()

pkg_check_modules(SDL2 REQUIRED sdl2)

include_directories(
    ${CMAKE_SOURCE_DIR}/inc
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${SDBUSCPP_INCLUDE_DIRS}
)

qt_add_executable(smallestMusicPlayer
    WIN32 MACOSX_BUNDLE

    ui/mainwindow.ui

    inc/mainwindow.h
    inc/AudioPlayer.hpp
    inc/Precompiled.h
    inc/MetaData.hpp
    inc/mpris_server.hpp
    inc/MetaDataSharer.hpp

    src/main.cpp
    src/mainwindow.cpp
    src/AudioPlayer.cpp
    src/MetaDataSharer.cpp
)

# -----------------------------------------------------------------------------
# [新增/修改] 针对 Debug 和 Release 的差异化编译选项
# -----------------------------------------------------------------------------
if(MSVC)
    # Visual Studio (MSVC) 编译器选项
    # Debug:   /Od (禁用优化), /Zi (生成调试信息)
    # Release: /O2 (最大化速度), /Ob2 (内联扩展)
    target_compile_options(smallestMusicPlayer PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2 /Ob2>
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

# [建议] Release 模式下开启 IPO/LTO (链接时优化)
# 这允许编译器在链接阶段跨文件优化（如跨模块内联），对音视频项目性能提升明显
set_property(TARGET smallestMusicPlayer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# -----------------------------------------------------------------------------

qt_add_translations(
    TARGETS smallestMusicPlayer
    TS_FILES smallestMusicPlayer_zh_CN.ts
)

target_link_libraries(smallestMusicPlayer
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt6::Multimedia
        ${FFMPEG_LIBRARIES}
        ${SDL2_LIBRARIES}
        ${SDBUSCPP_LIBRARIES}
)

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
