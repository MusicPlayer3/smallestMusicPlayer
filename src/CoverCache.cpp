#include "CoverCache.hpp"
#include <SDL2/SDL_log.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

void CoverCache::putCompressedFromPixels(const std::string &album,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
{
    if (album.empty() || !srcPixels || srcW <= 0 || srcH <= 0 || channels <= 0)
        return;

    if (covercache.find(album) != covercache.end())
        return;

    const int targetW = 200;
    const int targetH = 200;

    // ============================================================
    // 修改点 1: 在创建 CoverImage 对象前，先分配好内存
    // ============================================================
    // 创建一个临时的 vector 来存储缩放后的数据
    // 大小必须严格等于 宽 * 高 * 通道数
    std::vector<uint8_t> resizedPixels;
    try
    {
        resizedPixels.resize(targetW * targetH * channels);
    }
    catch (const std::bad_alloc &)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CoverCache: OOM allocating resized buffer");
        return;
    }

    int srcStride = srcW * channels;
    int dstStride = targetW * channels;

    // 布局选择逻辑保持不变
    stbir_pixel_layout layout;
    switch (channels)
    {
    case 1: layout = STBIR_1CHANNEL; break;
    case 2: layout = STBIR_2CHANNEL; break;
    case 3: layout = STBIR_RGB; break;
    case 4: layout = STBIR_RGBA; break;
    default:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unsupported channel count: %d", channels);
        return;
    }

    // ============================================================
    // 修改点 2: 写入到临时的 resizedPixels 中
    // ============================================================
    unsigned char *res = stbir_resize_uint8_srgb(
        srcPixels,
        srcW, srcH, srcStride,
        resizedPixels.data(), // 这里直接传入 vector 的指针
        targetW, targetH, dstStride,
        layout);

    if (!res)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CoverCache: resize_uint8_srgb failed for album %s",
                     album.c_str());
        return;
    }

    // ============================================================
    // 修改点 3: 构建 RAII 对象
    // ============================================================
    try
    {
        // 使用 std::move 将 resizedPixels 的所有权转移给 CoverImage
        // 这样不会发生内存拷贝，效率极高
        auto img = std::make_shared<CoverImage>(
            targetW,
            targetH,
            channels,
            std::move(resizedPixels));

        // 存入缓存
        covercache[album] = std::move(img);
    }
    catch (const std::exception &e)
    {
        // 捕获构造函数中可能抛出的 std::invalid_argument 等异常
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CoverCache: Failed to create CoverImage: %s", e.what());
    }
}

std::shared_ptr<CoverImage> CoverCache::get(const std::string &album) // 获取封面图
{
    auto it = covercache.find(album);
    return (it == covercache.end()) ? nullptr : it->second;
}

void CoverCache::clear()
{
    covercache.clear();
}