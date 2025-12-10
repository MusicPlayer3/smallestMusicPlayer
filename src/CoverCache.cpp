#include "CoverCache.hpp"
#include <QDebug>
#include "CoverImage.hpp"

namespace fs = std::filesystem;

void CoverCache::putCompressedFromPixels(const std::string &album,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
{
    if (album.empty() || !srcPixels || srcW <= 0 || srcH <= 0 || channels != 4)
    {
        if (channels != 4 && channels > 0)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "CoverCache: Rejecting non-4-channel image for album %s (got %d)",
                         album.c_str(), channels);
        }
        return;
    }

    // 1. 获取对应分桶
    size_t idx = getShardIndex(album);
    CacheShard &shard = shards[idx];

    // 2. 先检查是否存在（加锁）
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (shard.map.find(album) != shard.map.end())
            return;
    }

    // 3. 执行耗时的 Resize 操作（无需持有锁！）
    //    这是多线程优化的关键，多个线程可以同时在这里做 resize，互不阻塞
    const int targetW = 256;
    const int targetH = 256;
    std::vector<uint8_t> resizedPixels;

    try
    {
        resizedPixels.resize(targetW * targetH * 4);
    }
    catch (const std::bad_alloc &)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CoverCache: Out of memory resizing image for album %s", album.c_str());
        return;
    }

    int srcStride = srcW * 4;
    int dstStride = targetW * 4;

    auto res = stbir_resize_uint8_srgb(
        srcPixels,
        srcW, srcH, srcStride,
        resizedPixels.data(),
        targetW, targetH, dstStride,
        STBIR_RGBA);

    if (!res)
        return;

    // 4. 再次加锁写入结果
    try
    {
        auto img = std::make_shared<CoverImage>(
            targetW, targetH, 4, std::move(resizedPixels));

        std::lock_guard<std::mutex> lock(shard.mutex);
        // 双重检查，防止resize期间别的线程已经写进去了
        if (shard.map.find(album) == shard.map.end())
        {
            shard.map[album] = std::move(img);
        }
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CoverCache: Failed to create CoverImage for album %s: %s", album.c_str(), e.what());
    }
}

std::shared_ptr<CoverImage> CoverCache::get(const std::string &album)
{
    size_t idx = getShardIndex(album);
    CacheShard &shard = shards[idx];

    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(album);
    return (it == shard.map.end()) ? nullptr : it->second;
}

void CoverCache::clear()
{
    for (auto &shard : shards)
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.map.clear();
    }
}

// ... (辅助函数 sanitizeFilename 等保持不变) ...

void run_cover_test()
{
    CoverCache &cache = CoverCache::instance();
    // 简单遍历所有分桶进行统计
    int totalKeys = 0;
    for (const auto &shard : cache.shards)
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(shard.mutex));
        totalKeys += shard.map.size();
    }

    std::cout << "========================================================";
    std::cout << "--- Start CoverCache Debug Output (Total Keys:" << totalKeys << ") ---";
    std::cout << "========================================================";

    int count = 0;
    for (const auto &shard : cache.shards)
    {
        // 注意：这里为了 debug 锁住了 shard，会暂时阻塞写入
        std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(shard.mutex));
        for (const auto &[key, imgPtr] : shard.map)
        {
            QString status = "Invalid or Null";
            if (imgPtr && imgPtr->isValid())
            {
                status = QString("%1x%2 (%3 channels)")
                             .arg(imgPtr->width())
                             .arg(imgPtr->height())
                             .arg(imgPtr->channels());
            }

            std::cout << QString("[%1] KEY: \"%2\" | SIZE: %3")
                             .arg(++count, 2, 10, QChar('0'))
                             .arg(QString::fromStdString(key))
                             .arg(status)
                             .toStdString();
        }
    }
    // ...
}