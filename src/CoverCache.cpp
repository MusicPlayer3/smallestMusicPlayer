#include "CoverCache.hpp"
#include <QDebug>
#include "CoverImage.hpp"

namespace fs = std::filesystem;

void CoverCache::putCompressedFromPixels(const std::string &album,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
{
    // [修改] 严格检查通道数必须为 4
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

    std::lock_guard<std::mutex> lock(m_mutex);

    if (covercache.find(album) != covercache.end())
        return;

    const int targetW = 256;
    const int targetH = 256;

    std::vector<uint8_t> resizedPixels;
    try
    {
        // 4 channels * 256 * 256
        resizedPixels.resize(targetW * targetH * 4);
    }
    catch (const std::bad_alloc &)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CoverCache: OOM allocating resized buffer");
        return;
    }

    int srcStride = srcW * 4;
    int dstStride = targetW * 4;

    // [修改] 强制使用 STBIR_RGBA，因为我们已经确保输入是 4 通道
    auto res = stbir_resize_uint8_srgb(
        srcPixels,
        srcW, srcH, srcStride,
        resizedPixels.data(),
        targetW, targetH, dstStride,
        STBIR_RGBA);

    if (!res)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CoverCache: resize_uint8_srgb failed for album %s",
                     album.c_str());
        return;
    }

    try
    {
        auto img = std::make_shared<CoverImage>(
            targetW,
            targetH,
            4, // 确认是4
            std::move(resizedPixels));

        covercache[album] = std::move(img);
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CoverCache: Failed to create CoverImage: %s", e.what());
    }
}

std::shared_ptr<CoverImage> CoverCache::get(const std::string &album)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = covercache.find(album);
    return (it == covercache.end()) ? nullptr : it->second;
}

void CoverCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    covercache.clear();
}

static bool isIllegalChar(char c)
{
#ifdef _WIN32
    constexpr const char *illegalChars = "<>:\"/\\|?*";
    if (c >= 0 && c < 32)
        return true;
    return std::strchr(illegalChars, c) != nullptr;
#else
    return (c == '/' || c == '\0');
#endif
}

static std::string sanitizeFilename(const std::string &name)
{
    std::string safeName;
    safeName.reserve(name.size());

    for (unsigned char c : name)
    {
        if (isIllegalChar(c))
            safeName.push_back('_');
        else
            safeName.push_back(c);
    }

    if (safeName.empty() || std::all_of(safeName.begin(), safeName.end(), [](unsigned char c)
                                        { return std::isspace(c); }))
    {
        return "Unknown_Album";
    }

#ifdef _WIN32
    static const char *reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

    std::string upper = safeName;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    for (auto &r : reserved)
    {
        if (upper == r)
            return safeName + "_";
    }
#endif

    return safeName;
}

void run_cover_test()
{
    CoverCache &cache = CoverCache::instance();
    std::lock_guard<std::mutex> lock(cache.m_mutex);
    const auto &map = cache.covercache;

    qDebug() << "========================================================";
    qDebug() << "--- Start CoverCache Debug Output (Total Keys:" << map.size() << ") ---";
    qDebug() << "========================================================";

    if (map.empty())
    {
        qDebug() << "CoverCache is currently EMPTY. No keys found.";
    }
    else
    {
        int count = 0;
        for (const auto &[key, imgPtr] : map)
        {
            QString status = "Invalid or Null";
            if (imgPtr && imgPtr->isValid())
            {
                status = QString("%1x%2 (%3 channels)")
                             .arg(imgPtr->width())
                             .arg(imgPtr->height())
                             .arg(imgPtr->channels());
            }

            qDebug() << QString("[%1] KEY: \"%2\" | SIZE: %3")
                            .arg(++count, 2, 10, QChar('0'))
                            .arg(QString::fromStdString(key))
                            .arg(status);
        }
    }

    qDebug() << "========================================================";
    qDebug() << "--- End CoverCache Debug Output ---";
    qDebug() << "========================================================";
}