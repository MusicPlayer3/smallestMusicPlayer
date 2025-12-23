#include "CoverCache.hpp"
#include "DatabaseService.hpp"
#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>

std::shared_ptr<CoverImage> CoverCache::decodeBlob(const std::vector<uint8_t> &blob)
{
    if (blob.empty())
        return nullptr;
    try
    {
        // 从 Blob 解码
        cv::Mat rawData(1, static_cast<int>(blob.size()), CV_8UC1, const_cast<void *>(static_cast<const void *>(blob.data())));
        cv::Mat decodedMat = cv::imdecode(rawData, cv::IMREAD_UNCHANGED);

        if (decodedMat.empty())
            return nullptr;

        cv::Mat rgbaMat;
        if (decodedMat.channels() == 4)
            rgbaMat = decodedMat;
        else if (decodedMat.channels() == 3)
            cv::cvtColor(decodedMat, rgbaMat, cv::COLOR_BGR2RGBA);
        else if (decodedMat.channels() == 1)
            cv::cvtColor(decodedMat, rgbaMat, cv::COLOR_GRAY2RGBA);
        else
            return nullptr;

        // 双重保险 Resize
        if (rgbaMat.cols != 256 || rgbaMat.rows != 256)
            cv::resize(rgbaMat, rgbaMat, cv::Size(256, 256), 0, 0, cv::INTER_AREA);

        if (!rgbaMat.isContinuous())
            rgbaMat = rgbaMat.clone();

        size_t dataSize = rgbaMat.total() * rgbaMat.elemSize();
        std::vector<uint8_t> pixels(dataSize);
        std::memcpy(pixels.data(), rgbaMat.data, dataSize);

        return std::make_shared<CoverImage>(256, 256, 4, std::move(pixels));
    }
    catch (std::exception &e)
    {
        spdlog::error("decodeBlob error: {}", e.what());
        return nullptr;
    }
}

std::shared_ptr<CoverImage> CoverCache::get(const std::string &albumKey)
{
    if (albumKey.empty())
        return nullptr;

    // 1. RAM Hit (Critical Section)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(albumKey);
        if (it != m_map.end())
        {
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
            spdlog::debug("find image in RAM:{}", albumKey);
            return it->second.image;
        }
    }

    // 2. RAM Miss -> DB Query (Release Cache Lock to avoid deadlock with DB Lock)
    std::vector<uint8_t> blob = DatabaseService::instance().getCoverBlob(albumKey);
    if (blob.empty())
        return nullptr;

    auto image = decodeBlob(blob);
    if (!image || !image->isValid())
        return nullptr;
    spdlog::debug("find image in DataBase:{}", albumKey);

    // 3. Put into RAM (Critical Section)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Double check
        auto it = m_map.find(albumKey);
        if (it != m_map.end())
        {
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
            return it->second.image;
        }

        // Evict if needed
        if (m_map.size() >= MAX_CAPACITY)
        {
            std::string keyToRemove = m_lruList.back();
            m_lruList.pop_back();
            m_map.erase(keyToRemove);
        }

        m_lruList.push_front(albumKey);
        m_map[albumKey] = {image, m_lruList.begin()};
        return image;
    }
}

bool CoverCache::hasKey(const std::string &albumKey)
{
    if (albumKey.empty())
        return false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_map.find(albumKey) != m_map.end())
            return true;
    }
    // Fallback to DB check
    return !DatabaseService::instance().getCoverBlob(albumKey).empty();
}

void CoverCache::putCompressedFromPixels(const std::string &albumKey,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
{
    if (albumKey.empty() || !srcPixels || srcW <= 0 || srcH <= 0)
        return;

    try
    {
        // --- 1. 处理图片 (无锁) ---
        int type = (channels == 4) ? CV_8UC4 : CV_8UC3;
        cv::Mat srcMat(srcH, srcW, type, const_cast<unsigned char *>(srcPixels));
        cv::Mat dstMat;
        cv::resize(srcMat, dstMat, cv::Size(256, 256), 0, 0, cv::INTER_AREA);
        if (dstMat.channels() == 3)
            cv::cvtColor(dstMat, dstMat, cv::COLOR_RGB2RGBA);
        if (!dstMat.isContinuous())
            dstMat = dstMat.clone();

        // 内存对象 (RAM)
        size_t dataSize = dstMat.total() * dstMat.elemSize();
        std::vector<uint8_t> resizedPixels(dataSize);
        std::memcpy(resizedPixels.data(), dstMat.data, dataSize);
        auto image = std::make_shared<CoverImage>(256, 256, 4, std::move(resizedPixels));

        // 数据库对象 (PNG压缩)
        std::vector<uchar> pngBuf;
        std::vector<int> params = {cv::IMWRITE_PNG_COMPRESSION, 3};
        cv::imencode(".png", dstMat, pngBuf, params);

        // --- 2. 立即写入数据库 (避免数据丢失) ---
        // 关键点：即使稍后 Cache 满了被 Evict，DB 里已经安全了。
        // saveCoverBlob 内部有自己的锁，不持有 Cache 锁，安全。
        DatabaseService::instance().saveCoverBlob(albumKey, pngBuf);

        // --- 3. 更新内存 LRU (持有 Cache 锁) ---
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(albumKey);
        if (it != m_map.end())
        {
            // Update & Move to front
            it->second.image = image;
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
        }
        else
        {
            // Insert & Evict if needed
            if (m_map.size() >= MAX_CAPACITY)
            {
                m_map.erase(m_lruList.back());
                m_lruList.pop_back();
            }
            m_lruList.push_front(albumKey);
            m_map[albumKey] = {image, m_lruList.begin()};
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("[CoverCache] Put pixels failed: {}", e.what());
    }
}

void CoverCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_map.clear();
    m_lruList.clear();
}

std::shared_ptr<CoverImage> CoverCache::getRamOnly(const std::string &albumKey)
{
    if (albumKey.empty())
        return nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_map.find(albumKey);
    if (it != m_map.end())
    {
        // 命中内存：更新 LRU 并返回
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
        return it->second.image;
    }
    // 内存未命中：直接返回空，绝不调用 DatabaseService
    return nullptr;
}