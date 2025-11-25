#include "Cover.hpp"
#include <SDL2/SDL_log.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

void CoverCache::putCompressedFromPixels(const std::string &album,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
    //压缩音频专辑封面并放入缓存
{
    if (album.empty() || !srcPixels || srcW <= 0 || srcH <= 0 || channels <= 0)
        return;

    if (covercache.find(album) != covercache.end())
        return;

    auto img = std::make_shared<CoverImage>();

    const int targetW = 200;
    const int targetH = 200;

    img->width = targetW;
    img->height = targetH;
    img->channels = channels;
    img->pixels.resize(targetW * targetH * channels);

    // 1. stride：每行字节数（紧密存储）
    int srcStride = srcW * channels;
    int dstStride = targetW * channels;

    // 2. 根据 channels 选 layout —— 名字以你头文件实际定义为准
    stbir_pixel_layout layout;
    switch (channels)
    {
    case 1:
        layout = STBIR_1CHANNEL;
        break;
    case 2:
        layout = STBIR_2CHANNEL; // 或 STBIR_RA
        break;
    case 3:
        layout = STBIR_RGB; // 封面图绝大多数都是 RGB
        break;
    case 4:
        layout = STBIR_RGBA; // RGBA 最安全
        break;
    default:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unsupported channel count: %d", channels);
        return;
    }
    // 3. 调用 stbir_resize_uint8_srgb
    unsigned char *res = stbir_resize_uint8_srgb(
        srcPixels,
        srcW, srcH, srcW * channels,
        img->pixels.data(),
        targetW, targetH, targetW * channels,
        layout);

    if (!res)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CoverCache: resize_uint8_srgb failed for album %s",
                     album.c_str());
        return;
    }

    covercache[album] = img;
}

std::shared_ptr<CoverImage> CoverCache::get(const std::string &album)//获取封面图
{
    auto it = covercache.find(album);
    return (it == covercache.end()) ? nullptr : it->second;
}

void CoverCache::clear()
{
    covercache.clear();
}