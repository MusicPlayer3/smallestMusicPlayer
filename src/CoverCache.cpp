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
        // cv::imdecode 默认输出 BGR(A)
        cv::Mat rawData(1, static_cast<int>(blob.size()), CV_8UC1, const_cast<void *>(static_cast<const void *>(blob.data())));

        // 使用 IMREAD_UNCHANGED 以保留 alpha 通道(如果有)
        cv::Mat decodedMat = cv::imdecode(rawData, cv::IMREAD_UNCHANGED);

        if (decodedMat.empty())
            return nullptr;

        // 统一转为 RGBA 供 UI 使用
        cv::Mat rgbaMat;
        if (decodedMat.channels() == 4)
            cv::cvtColor(decodedMat, rgbaMat, cv::COLOR_BGRA2RGBA); // 修正：从 BGRA 转 RGBA
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

    // 1. RAM Hit (Read Lock)
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_map.find(albumKey);
        if (it != m_map.end())
        {
            return it->second.image;
        }
    }

    // 2. RAM Miss -> DB Query (No Lock)
    std::vector<uint8_t> blob = DatabaseService::instance().getCoverBlob(albumKey);
    if (blob.empty())
        return nullptr;

    auto image = decodeBlob(blob);
    if (!image || !image->isValid())
        return nullptr;

    // 3. Put into RAM (Write Lock)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_map.find(albumKey);
        if (it != m_map.end())
        {
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
            return it->second.image;
        }

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
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        if (m_map.find(albumKey) != m_map.end())
            return true;
    }
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
        // 输入数据 srcPixels 来自 FileScanner，格式为 RGBA
        int type = (channels == 4) ? CV_8UC4 : CV_8UC3;
        cv::Mat srcMat(srcH, srcW, type, const_cast<unsigned char *>(srcPixels));

        cv::Mat dstMat;
        cv::resize(srcMat, dstMat, cv::Size(256, 256), 0, 0, cv::INTER_AREA);

        // 确保 dstMat 也是 RGBA (供内存缓存使用)
        if (dstMat.channels() == 3)
            cv::cvtColor(dstMat, dstMat, cv::COLOR_RGB2RGBA);

        if (!dstMat.isContinuous())
            dstMat = dstMat.clone();

        // A. 构建内存对象 (RAM Cache) -> 需要 RGBA
        size_t dataSize = dstMat.total() * dstMat.elemSize();
        std::vector<uint8_t> resizedPixels(dataSize);
        std::memcpy(resizedPixels.data(), dstMat.data, dataSize);
        auto image = std::make_shared<CoverImage>(256, 256, 4, std::move(resizedPixels));

        // B. 构建数据库对象 (PNG File) -> 需要 BGRA (OpenCV 标准)
        // [Critical Fix] 必须转换通道顺序，否则保存的 PNG 颜色会反转 (蓝变橙)
        cv::Mat saveMat;
        if (dstMat.channels() == 4)
            cv::cvtColor(dstMat, saveMat, cv::COLOR_RGBA2BGRA);
        else
            cv::cvtColor(dstMat, saveMat, cv::COLOR_RGB2BGR);

        std::vector<uchar> pngBuf;
        std::vector<int> params = {cv::IMWRITE_PNG_COMPRESSION, 3};
        cv::imencode(".png", saveMat, pngBuf, params); // 使用转换后的 saveMat 编码

        // --- 2. 写入数据库 ---
        DatabaseService::instance().saveCoverBlob(albumKey, pngBuf);

        // --- 3. 更新内存 LRU (Write Lock) ---
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_map.find(albumKey);
        if (it != m_map.end())
        {
            it->second.image = image;
            m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIt);
        }
        else
        {
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
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_map.clear();
    m_lruList.clear();
}

std::shared_ptr<CoverImage> CoverCache::getRamOnly(const std::string &albumKey)
{
    if (albumKey.empty())
        return nullptr;

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_map.find(albumKey);
    if (it != m_map.end())
    {
        return it->second.image;
    }
    return nullptr;
}