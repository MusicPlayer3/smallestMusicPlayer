#ifndef __COVER_CACHE_HPP__
#define __COVER_CACHE_HPP__

#include "PCH.h"
#include "CoverImage.hpp"
#include <list>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

class CoverCache
{
public:
    static CoverCache &instance()
    {
        static CoverCache cache;
        return cache;
    }

    std::shared_ptr<CoverImage> get(const std::string &albumKey);
    bool hasKey(const std::string &albumKey);

    /**
     * @brief [新增] 仅从内存缓存获取，绝不回调数据库
     * 用于打破 DatabaseService -> CoverCache -> DatabaseService 的死锁循环
     */
    std::shared_ptr<CoverImage> getRamOnly(const std::string &albumKey);

    void putCompressedFromPixels(const std::string &albumKey,
                                 const unsigned char *srcPixels,
                                 int srcW, int srcH, int channels);

    void clear();

private:
    CoverCache() = default;
    ~CoverCache() = default;
    CoverCache(const CoverCache &) = delete;
    CoverCache &operator=(const CoverCache &) = delete;

    const size_t MAX_CAPACITY = 200;

    std::mutex m_mutex;
    std::list<std::string> m_lruList;
    struct CacheEntry
    {
        std::shared_ptr<CoverImage> image;
        std::list<std::string>::iterator lruIt;
    };
    std::unordered_map<std::string, CacheEntry> m_map;

    std::shared_ptr<CoverImage> decodeBlob(const std::vector<uint8_t> &blob);
};

#endif