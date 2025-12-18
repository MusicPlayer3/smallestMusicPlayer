#include "CoverCache.hpp"
#include "CoverImage.hpp"

void CoverCache::putCompressedFromPixels(const std::string &album,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
{
    // 参数基本校验
    if (album.empty() || !srcPixels || srcW <= 0 || srcH <= 0 || channels != 4)
    {
        if (channels != 4 && channels > 0)
        {
            spdlog::error("[CoverCache] Rejecting non-4-channel image for album '{}' (got {})", album, channels);
        }
        return;
    }

    // 1. 获取对应的分桶索引
    size_t idx = getShardIndex(album);
    CacheShard &shard = shards[idx];

    // 2. 快速检查：如果已存在，直接返回 (持有锁)
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (shard.map.find(album) != shard.map.end())
            return;
    }

    // 3. 执行耗时的 Resize 操作 (无锁状态！)
    // 这是多线程优化的关键，多个线程可以并发执行 stbir_resize，互不阻塞
    const int targetW = 256;
    const int targetH = 256;
    std::vector<uint8_t> resizedPixels;

    try
    {
        resizedPixels.resize(targetW * targetH * 4);
    }
    catch (const std::bad_alloc &)
    {
        spdlog::error("[CoverCache] Out of memory resizing image for album '{}'", album);
        return;
    }

    int srcStride = srcW * 4;
    int dstStride = targetW * 4;

    // 使用 stb_image_resize 进行缩放
    auto res = stbir_resize_uint8_srgb(
        srcPixels,
        srcW, srcH, srcStride,
        resizedPixels.data(),
        targetW, targetH, dstStride,
        STBIR_RGBA);

    if (!res)
        return;

    // 4. 再次加锁，将结果写入缓存
    try
    {
        auto img = std::make_shared<CoverImage>(
            targetW, targetH, 4, std::move(resizedPixels));

        std::lock_guard<std::mutex> lock(shard.mutex);
        // 双重检查，防止在 resize 期间别的线程已经写进去了
        if (shard.map.find(album) == shard.map.end())
        {
            shard.map[album] = std::move(img);
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("[CoverCache] Failed to create CoverImage for album '{}': {}", album, e.what());
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

// 调试函数
void run_cover_test()
{
    CoverCache &cache = CoverCache::instance();
    // 统计总数
    int totalKeys = 0;
    for (const auto &shard : cache.shards)
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(shard.mutex));
        totalKeys += shard.map.size();
    }

    std::cout << "========================================================\n";
    std::cout << "--- CoverCache Debug (Total Keys: " << totalKeys << ") ---\n";
    std::cout << "========================================================\n";

    int count = 0;
    for (const auto &shard : cache.shards)
    {
        // 注意：这里为了 debug 锁住了 shard，会暂时阻塞写入
        std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(shard.mutex));
        for (const auto &[key, imgPtr] : shard.map)
        {
            std::string status = "Invalid or Null";
            if (imgPtr && imgPtr->isValid())
            {
                status = std::format("{}x{} ({} channels)",
                                     imgPtr->width(), imgPtr->height(), imgPtr->channels());
            }

            // 格式化输出
            std::cout << std::format("[{:02}] KEY: \"{}\" | SIZE: {}\n",
                                     ++count, key, status);
        }
    }
}