# 3rdparty/CMakeLists.txt

# 创建一个静态库，包含我们刚才写的实现文件
add_library(MyThirdPartyLibs STATIC LibImpl.cpp)

# 设置头文件搜索路径
# PUBLIC 意味着：链接了这个库的目标，也能自动找到这些头文件
target_include_directories(MyThirdPartyLibs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR}BS_thread_pool
    ${CMAKE_CURRENT_SOURCE_DIR}/mpris_server
)

# 可以在这里屏蔽一些第三方库产生的烦人警告，保持你的主项目干净
if(MSVC)
    target_compile_options(MyThirdPartyLibs PRIVATE /W0)
else()
    target_compile_options(MyThirdPartyLibs PRIVATE -w)
endif()
