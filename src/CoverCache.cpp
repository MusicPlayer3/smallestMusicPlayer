#include "CoverCache.hpp"

// resize 库实现 (保持原样)

// [新增] write 库实现，用于导出图片

namespace fs = std::filesystem;

void CoverCache::putCompressedFromPixels(const std::string &album,
                                         const unsigned char *srcPixels,
                                         int srcW, int srcH, int channels)
{
    if (album.empty() || !srcPixels || srcW <= 0 || srcH <= 0 || channels <= 0)
        return;

    if (covercache.find(album) != covercache.end())
        return;

    const int targetW = 256;
    const int targetH = 256;

    // ============================================================
    // 修改点 1: 在创建 CoverImage 对象前，先分配好内存
    // ============================================================
    // 创建一个临时的 vector 来存储缩放后的数据
    // 大小必须严格等于 宽 * 高 * 通道数
    std::vector<uint8_t> resizedPixels;
    try
    {
        resizedPixels.resize(targetW * targetH * channels);
    }
    catch (const std::bad_alloc &)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CoverCache: OOM allocating resized buffer");
        return;
    }

    int srcStride = srcW * channels;
    int dstStride = targetW * channels;

    // 布局选择逻辑保持不变
    stbir_pixel_layout layout;
    switch (channels)
    {
    case 1: layout = STBIR_1CHANNEL; break;
    case 2: layout = STBIR_2CHANNEL; break;
    case 3: layout = STBIR_RGB; break;
    case 4: layout = STBIR_RGBA; break;
    default:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unsupported channel count: %d", channels);
        return;
    }

    // ============================================================
    // 修改点 2: 写入到临时的 resizedPixels 中
    // ============================================================
    auto res = stbir_resize_uint8_srgb(
        srcPixels,
        srcW, srcH, srcStride,
        resizedPixels.data(),
        targetW, targetH, dstStride,
        layout);

    if (!res)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CoverCache: resize_uint8_srgb failed for album %s",
                     album.c_str());
        return;
    }

    // ============================================================
    // 修改点 3: 构建 RAII 对象
    // ============================================================
    try
    {
        // 使用 std::move 将 resizedPixels 的所有权转移给 CoverImage
        // 这样不会发生内存拷贝，效率极高
        auto img = std::make_shared<CoverImage>(
            targetW,
            targetH,
            channels,
            std::move(resizedPixels));

        // 存入缓存
        covercache[album] = std::move(img);
    }
    catch (const std::exception &e)
    {
        // 捕获构造函数中可能抛出的 std::invalid_argument 等异常
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CoverCache: Failed to create CoverImage: %s", e.what());
    }
}

std::shared_ptr<CoverImage> CoverCache::get(const std::string &album) // 获取封面图
{
    auto it = covercache.find(album);
    return (it == covercache.end()) ? nullptr : it->second;
}

void CoverCache::clear()
{
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
    SDL_Log("=== Starting Cover Cache Export Test ===");

    // 1. 创建 ./ttemp 目录
    fs::path exportDir = "/tmp/ttemp";
    try
    {
        if (!fs::exists(exportDir))
        {
            fs::create_directories(exportDir);
        }
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Test: Failed to create dir /tmp/ttemp: %s", e.what());
        return;
    }

    // 2. 获取单例引用
    CoverCache &cache = CoverCache::instance();

    // 3. 遍历私有 map
    if (cache.covercache.empty())
    {
        SDL_Log("Test: CoverCache is empty. Nothing to export.");
        return;
    }

    int count = 0;
    for (const auto &[albumName, imgPtr] : cache.covercache)
    {
        if (!imgPtr || !imgPtr->isValid())
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Test: Invalid image for album: %s", albumName.c_str());
            continue;
        }

        // 清洗文件名
        std::string safeName = sanitizeFilename(albumName);
        if (safeName.empty())
            safeName = "Unknown_Album_" + std::to_string(count);

        // 构造输出路径 (保存为 PNG)
        fs::path outPath = exportDir / (safeName + ".png");

        // 4. 写入文件
        // stbi_write_png 参数: filename, w, h, comp(channels), data, stride_in_bytes
        int stride = imgPtr->width() * imgPtr->channels();

        int result = stbi_write_png(
            outPath.string().c_str(),
            imgPtr->width(),
            imgPtr->height(),
            imgPtr->channels(),
            imgPtr->data(),
            stride);

        if (result)
        {
            // SDL_Log("Test: Exported [%s]", outPath.string().c_str());
        }
        else
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Test: Failed to write [%s]", outPath.string().c_str());
        }
        count++;
    }

    SDL_Log("=== Cover Cache Export Finished. Total: %d files ===", count);
}