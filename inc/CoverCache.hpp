#ifndef COVER_HPP
#define COVER_HPP
#include "Precompiled.h"
#include "CoverImage.hpp"
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>

class CoverCache
{
public:
    // 全局单例
    static CoverCache &instance()
    {
        static CoverCache cache;
        return cache;
    }

    // 扫描阶段调用：用原图像素 + 原通道数，直接压缩并缓存
    void putCompressedFromPixels(const std::string &album,
                                 const unsigned char *srcPixels,
                                 int srcW, int srcH, int channels);

    // 按专辑名取缩略图
    std::shared_ptr<CoverImage> get(const std::string &album);

    void clear();

private:
    CoverCache() = default;

    std::unordered_map<std::string, std::shared_ptr<CoverImage>> covercache; // 专辑名 -> 缩略图
    std::mutex m_mutex;                                                      // 线程锁

    friend void run_cover_test(); // 测试用
};
#endif