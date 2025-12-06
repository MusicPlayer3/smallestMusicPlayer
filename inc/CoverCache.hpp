#ifndef COVER_HPP
#define COVER_HPP
#include "Precompiled.h"
#include "CoverImage.hpp"
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>
#include <array>

class CoverCache
{
public:
    static CoverCache &instance()
    {
        static CoverCache cache;
        return cache;
    }

    void putCompressedFromPixels(const std::string &album,
                                 const unsigned char *srcPixels,
                                 int srcW, int srcH, int channels);

    std::shared_ptr<CoverImage> get(const std::string &album);

    void clear();

private:
    CoverCache() = default;

    // 分段锁机制：使用 32 个分桶减少锁竞争
    static const size_t SHARD_COUNT = 32;
    struct CacheShard
    {
        std::mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<CoverImage>> map;
    };

    std::array<CacheShard, SHARD_COUNT> shards;

    // 辅助：根据 key 获取分桶索引
    size_t getShardIndex(const std::string &key) const
    {
        return std::hash<std::string>{}(key) % SHARD_COUNT;
    }

    friend void run_cover_test();
};
#endif