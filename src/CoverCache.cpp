#include "CoverCache.hpp"
#include "CoverImage.hpp"

// [Modified] 引入 OpenCV 核心与图像处理模块
// 移除 opencv2/core/ocl.hpp，不再使用 GPU，避免小图传输的性能惩罚
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

void CoverCache::putCompressedFromPixels(const std::string &album,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
{
    // 参数基本校验
    if (album.empty() || !srcPixels || srcW <= 0 || srcH <= 0)
    {
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

    // 3. 执行 Resize 操作 (无锁状态)
    // [Modified] 纯 CPU 处理。
    // OpenCV 的 resize 底层会自动使用 AVX2/SSE 指令集优化，
    // 对于 256x256 这种小图，CPU 处理仅需微秒级，远快于 GPU 的内存往返传输。
    const int targetW = 256;
    const int targetH = 256;

    std::vector<uint8_t> resizedPixels;

    try
    {
        // 构建 OpenCV Mat 包装原始数据 (不进行拷贝，只是引用指针)
        // 这里的 srcPixels 通常来自 FileScanner 的解码结果，已经是 RGBA 格式
        int type = (channels == 4) ? CV_8UC4 : CV_8UC3;
        cv::Mat srcMat(srcH, srcW, type, const_cast<unsigned char *>(srcPixels));
        cv::Mat dstMat;

        // [Modified] 使用 INTER_AREA (区域重采样)
        // 这是缩小图片时效果最好的插值算法，能有效避免摩尔纹，虽然计算量稍大，但在 CPU 上处理小图依然极快。
        cv::resize(srcMat, dstMat, cv::Size(targetW, targetH), 0, 0, cv::INTER_AREA);

        // 如果输入是 3 通道，转为 4 通道 (RGBA) 以便统一存储
        // 注意：FileScanner 现在通过 OpenCV 解码并转为 RGBA 传入，所以这里通常已经是 4 通道
        if (dstMat.channels() == 3)
        {
            cv::cvtColor(dstMat, dstMat, cv::COLOR_RGB2RGBA);
        }

        // 导出数据
        if (dstMat.isContinuous())
        {
            size_t dataSize = dstMat.total() * dstMat.elemSize();
            resizedPixels.resize(dataSize);
            std::memcpy(resizedPixels.data(), dstMat.data, dataSize);
        }
        else
        {
            // 防御性代码：如果不连续，克隆一份连续的
            cv::Mat cont = dstMat.clone();
            size_t dataSize = cont.total() * cont.elemSize();
            resizedPixels.resize(dataSize);
            std::memcpy(resizedPixels.data(), cont.data, dataSize);
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("[CoverCache] Error resizing album '{}': {}", album, e.what());
        return;
    }

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