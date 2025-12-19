#include "FileScanner.hpp"
#include "CoverCache.hpp"
#include "MetaData.hpp"
#include "PCH.h"
#include "SimpleThreadPool.hpp"

// ==========================================
// 0. 性能分析工具 (Profiler) - [NEW]
// ==========================================
// #define ANALYSE
struct Profiler
{
    // 累计耗时 (微秒) - 原子变量，线程安全
    static inline std::atomic<int64_t> t_structure_scan{0}; // Phase 1: 目录结构扫描与分发
    static inline std::atomic<int64_t> t_ffmpeg{0};         // Worker: FFmpeg 读取
    static inline std::atomic<int64_t> t_taglib{0};         // Worker: TagLib 解析
    static inline std::atomic<int64_t> t_cover_process{0};  // Worker: 封面提取与缩放 (含 Phase 3)
    static inline std::atomic<int64_t> t_wait_audio{0};     // Phase 2: 等待音频任务
    static inline std::atomic<int64_t> t_aggregation{0};    // Phase 3: 后处理聚合与封面任务分发
    static inline std::atomic<int64_t> t_wait_cover{0};     // Phase 4: 等待封面任务

    // 计数
    static inline std::atomic<int> count_files{0};

    static void reset()
    {
        t_structure_scan = 0;
        t_ffmpeg = 0;
        t_taglib = 0;
        t_cover_process = 0;
        t_wait_audio = 0;
        t_aggregation = 0;
        t_wait_cover = 0;
        count_files = 0;
    }

    static void printReport(int64_t total_us)
    {
        auto fmt = [](int64_t us)
        { return (double)us / 1000.0; };
        std::cout << "\n========== Performance Analysis Report ==========\n";
        std::cout << "Total Wall Time  : " << fmt(total_us) << " ms\n";
        std::cout << "Processed Files  : " << count_files << "\n";
        std::cout << "-----------------------------------------------\n";
        std::cout << "[Phase 1] Dir Scan (Main Thread): " << fmt(t_structure_scan) << " ms\n";
        std::cout << "[Phase 2] Wait Audio (Wall Time): " << fmt(t_wait_audio) << " ms\n";
        std::cout << "[Phase 3] Aggregation           : " << fmt(t_aggregation) << " ms\n";
        std::cout << "[Phase 4] Wait Covers (Wall Time):" << fmt(t_wait_cover) << " ms\n";
        std::cout << "-----------------------------------------------\n";
        std::cout << ">> Cumulative Worker CPU Time (Sum of all threads):\n";
        std::cout << "   - FFmpeg Probe   : " << fmt(t_ffmpeg) << " ms\n";
        std::cout << "   - TagLib Parse   : " << fmt(t_taglib) << " ms\n";
        std::cout << "   - Cover Process  : " << fmt(t_cover_process) << " ms\n";
        std::cout << "===============================================\n\n";
    }
};

// RAII 计时辅助类
struct ScopedTimer
{
    std::atomic<int64_t> &target;
    std::chrono::high_resolution_clock::time_point start;

    ScopedTimer(std::atomic<int64_t> &counter) : target(counter)
    {
        start = std::chrono::high_resolution_clock::now();
    }
    ~ScopedTimer()
    {
        auto end = std::chrono::high_resolution_clock::now();
        target += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    }
};

// ==========================================
// 1. 内部工具与常量 (Internal Helpers)
// ==========================================

namespace
{

constexpr size_t K_BATCH_SIZE = 64;

static const std::vector<std::string> kKnownAudioExtensions = {
    "mp3", "aac", "m4a", "ogg", "wma", "opus", "mpc", "mp+", "mpp",
    "flac", "ape", "wav", "aiff", "aif", "wv", "tta", "alac", "shn", "tak",
    "dsf", "dff", "dxd", "mka", "webm", "dts", "ac3", "truehd"};

static const std::unordered_set<std::string> kCoverFileNames = {
    "cover", "folder", "front", "album", "art"};

static const std::unordered_set<std::string> kImageExts = {
    ".jpg", ".jpeg", ".png", ".bmp"};

static std::unordered_set<std::string> g_supportedAudioExts;
static std::once_flag g_initFlag;

struct StbiDeleter
{
    void operator()(unsigned char *data) const
    {
        if (data)
            stbi_image_free(data);
    }
};
using StbiPtr = std::unique_ptr<unsigned char, StbiDeleter>;

struct AVFormatContextDeleter
{
    void operator()(AVFormatContext *ctx) const
    {
        if (ctx)
            avformat_close_input(&ctx);
    }
};
using AVContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct UchardetDeleter
{
    void operator()(uchardet_t ud) const
    {
        if (ud)
            uchardet_delete(ud);
    }
};
using UchardetPtr = std::unique_ptr<std::remove_pointer<uchardet_t>::type, UchardetDeleter>;

// 忽略大小写的后缀匹配
bool hasExtension(std::string_view filename, std::string_view ext)
{
    if (filename.length() < ext.length())
        return false;
    auto suffix = filename.substr(filename.length() - ext.length());
    return std::ranges::equal(suffix, ext, [](char a, char b)
                              { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

// 检查是否为支持的音频文件 (基于扩展名)
bool isffmpeg(const std::string &route)
{
    if (route.empty())
        return false;
    fs::path p(route);
    if (!p.has_extension())
        return false;
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), ::tolower);
    return g_supportedAudioExts.contains(ext);
}

// 辅助：获取小写扩展名
std::string getLowerExt(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    std::ranges::transform(ext, ext.begin(), ::tolower);
    return ext;
}

// 辅助：检查文件名是否是常见的封面名
bool isCoverFileName(const std::string &stem)
{
    std::string lowerStem = stem;
    std::ranges::transform(lowerStem, lowerStem.begin(), ::tolower);
    return kCoverFileNames.contains(lowerStem);
}

} // namespace

// ==========================================
// 2. 字符编码处理 (Encoding Logic)
// ==========================================

namespace EncodingUtils
{

// 仅用于输出清洗：确保结果是合法的 UTF-8，防止 sdbus 崩溃
static std::string sanitizeUTF8(const std::string &data)
{
    std::string res;
    res.reserve(data.size());
    const unsigned char *bytes = (const unsigned char *)data.data();
    const unsigned char *end = bytes + data.size();

    while (bytes < end)
    {
        // ASCII
        if ((*bytes & 0x80) == 0)
        {
            res += *bytes++;
            continue;
        }

        // Multibyte check
        int len = 0;
        if ((*bytes & 0xE0) == 0xC0)
            len = 2;
        else if ((*bytes & 0xF0) == 0xE0)
            len = 3;
        else if ((*bytes & 0xF8) == 0xF0)
            len = 4;

        bool ok = (len > 0) && (bytes + len <= end);
        if (ok)
        {
            for (int i = 1; i < len; ++i)
            {
                if ((bytes[i] & 0xC0) != 0x80)
                {
                    ok = false;
                    break;
                }
            }
        }

        if (ok)
        {
            res.append((const char *)bytes, len);
            bytes += len;
        }
        else
        {
            // 非法序列：用 '?' 替换，避免 DBus 崩溃
            res += "?";
            bytes++; // 跳过非法字节
        }
    }
    return res;
}

static std::string convertToUTF8(const std::string &data, const char *fromEncoding)
{
    if (data.empty())
        return "";
    std::string fromEncName = fromEncoding ? fromEncoding : "GB18030";

#ifdef _WIN32
    int codePage = CP_ACP;
    std::string encodingUpper = fromEncName;
    std::ranges::transform(encodingUpper, encodingUpper.begin(), ::toupper);

    if (encodingUpper == "GBK" || encodingUpper == "GB2312" || encodingUpper == "GB18030")
        codePage = 54936;
    else if (encodingUpper == "BIG5")
        codePage = 950;
    else if (encodingUpper == "SHIFT_JIS")
        codePage = 932;
    else if (encodingUpper == "WINDOWS-1252")
        codePage = 1252;
    else if (encodingUpper == "UTF-8" || encodingUpper == "ASCII")
        return sanitizeUTF8(data);

    int wLen = MultiByteToWideChar(codePage, 0, data.c_str(), (int)data.size(), nullptr, 0);
    if (wLen <= 0)
        return sanitizeUTF8(data);

    std::wstring wStr(wLen, 0);
    MultiByteToWideChar(codePage, 0, data.c_str(), (int)data.size(), &wStr[0], wLen);

    int uLen = WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), wLen, nullptr, 0, nullptr, nullptr);
    if (uLen <= 0)
        return sanitizeUTF8(data);

    std::string ret(uLen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), wLen, &ret[0], uLen, nullptr, nullptr);
    return ret;
#else
    iconv_t cd = iconv_open("UTF-8", fromEncName.c_str());
    if (cd == (iconv_t)-1)
    {
        if (fromEncName == "GB2312")
            return convertToUTF8(data, "GB18030");
        return sanitizeUTF8(data);
    }

    // Linux iconv
    size_t inLeft = data.size();
    char *inBuf = const_cast<char *>(data.c_str());

    size_t outLen = inLeft * 4 + 1;
    std::string outStr(outLen, '\0');
    char *outBuf = &outStr[0];
    size_t outLeft = outLen;

    size_t res = iconv(cd, &inBuf, &inLeft, &outBuf, &outLeft);
    iconv_close(cd);

    if (res == (size_t)-1)
    {
        // 尝试部分保留
        if (outLeft < outLen)
        {
            outStr.resize(outLen - outLeft);
            return sanitizeUTF8(outStr);
        }
        return sanitizeUTF8(data);
    }

    outStr.resize(outLen - outLeft);
    return sanitizeUTF8(outStr);
#endif
}

static std::string detectCharset(const std::string &data)
{
    if (data.empty())
        return "";
    UchardetPtr ud(uchardet_new());
    if (!ud)
        return "";
    uchardet_handle_data(ud.get(), data.c_str(), data.length());
    uchardet_data_end(ud.get());
    const char *charset = uchardet_get_charset(ud.get());
    return charset ? std::string(charset) : "";
}

// 核心逻辑：完全依赖 uchardet 探测，不预判 isValidUTF8
static std::string detectAndConvert(const std::string &rawData)
{
    if (rawData.empty())
        return "";

    // 1. 直接探测
    std::string detectedEncoding = detectCharset(rawData);

    // 2. 策略修正
    if (detectedEncoding.empty())
    {
        detectedEncoding = "GB18030"; // 默认回退
    }
    else if (detectedEncoding == "ASCII")
    {
        return sanitizeUTF8(rawData);
    }

    // 3. 转换
    std::string result = convertToUTF8(rawData, detectedEncoding.c_str());

    // 4. 清洗非法字节
    return sanitizeUTF8(result);
}

} // namespace EncodingUtils

// ==========================================
// 3. TagLib 提取工具 (TagLib Helpers)
// ==========================================

namespace TagLibHelpers
{

// ID3v2 封面
static TagLib::ByteVector extractID3v2Cover(TagLib::ID3v2::Tag *tag)
{
    if (!tag)
        return {};
    auto frames = tag->frameList("APIC");
    if (frames.isEmpty())
        return {};
    return static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front())->picture();
}

// APE 封面
static TagLib::ByteVector extractAPECover(TagLib::APE::Tag *tag)
{
    if (!tag)
        return {};
    auto itemList = tag->itemListMap();
    static const std::vector<std::string> keys = {"Cover Art (Front)", "Cover Art (Back)", "Cover Art"};
    for (const auto &key : keys)
    {
        if (itemList.contains(key))
            return itemList[key].binaryData();
    }
    return {};
}

// ASF/WMA 封面
static TagLib::ByteVector extractASFCover(TagLib::ASF::Tag *tag)
{
    if (!tag)
        return {};
    if (tag->attributeListMap().contains("WM/Picture"))
    {
        auto attrList = tag->attributeListMap()["WM/Picture"];
        if (!attrList.isEmpty())
        {
            auto pic = attrList.front().toPicture();
            if (pic.isValid())
                return pic.picture();
        }
    }
    return {};
}

// Ogg/FLAC/Vorbis 封面
static TagLib::ByteVector extractXiphCover(TagLib::Ogg::XiphComment *tag)
{
    if (!tag)
        return {};
    auto picList = tag->pictureList();
    if (!picList.isEmpty())
        return picList.front()->data();
    return {};
}

// 统一封面提取
static TagLib::ByteVector extractCoverDataGeneric(const std::string &musicPath)
{
    fs::path p(musicPath);
    std::string ext = getLowerExt(musicPath);
    TagLib::ByteVector data;

    if (ext == ".mp3")
    {
        TagLib::MPEG::File f(p.c_str(), false);
        if (f.isValid() && f.ID3v2Tag())
            return extractID3v2Cover(f.ID3v2Tag());
    }
    else if (ext == ".flac")
    {
        TagLib::FLAC::File f(p.c_str(), false);
        if (f.isValid() && !f.pictureList().isEmpty())
            return f.pictureList()[0]->data();
    }
    else if (ext == ".m4a" || ext == ".mp4" || ext == ".aac" || ext == ".alac")
    {
        TagLib::MP4::File f(p.c_str(), false);
        if (f.isValid() && f.tag() && f.tag()->itemMap().contains("covr"))
        {
            auto list = f.tag()->itemMap()["covr"].toCoverArtList();
            if (!list.isEmpty())
                return list.front().data();
        }
    }
    else if (ext == ".wma" || ext == ".asf")
    {
        TagLib::ASF::File f(p.c_str(), false);
        if (f.isValid() && f.tag())
            return extractASFCover(f.tag());
    }
    else if (ext == ".ape")
    {
        TagLib::APE::File f(p.c_str(), false);
        if (f.isValid() && f.APETag())
            return extractAPECover(f.APETag());
    }
    else if (ext == ".wav")
    {
        TagLib::RIFF::WAV::File f(p.c_str(), false);
        if (f.isValid() && f.ID3v2Tag())
            return extractID3v2Cover(f.ID3v2Tag());
    }
    else if (ext == ".aiff" || ext == ".aif")
    {
        TagLib::RIFF::AIFF::File f(p.c_str(), false);
        if (f.isValid() && f.tag())
            return extractID3v2Cover(f.tag());
    }
    else if (ext == ".wv")
    {
        TagLib::WavPack::File f(p.c_str(), false);
        if (f.isValid() && f.APETag())
            return extractAPECover(f.APETag());
    }
    else if (ext == ".mpc" || ext == ".mp+" || ext == ".mpp")
    {
        TagLib::MPC::File f(p.c_str(), false);
        if (f.isValid() && f.APETag())
            return extractAPECover(f.APETag());
    }
    else if (ext == ".tta")
    {
        TagLib::TrueAudio::File f(p.c_str(), false);
        if (f.isValid() && f.ID3v2Tag())
            return extractID3v2Cover(f.ID3v2Tag());
    }
    else if (ext == ".dsf")
    {
#ifdef TAGLIB_DSF_FILE_H
        TagLib::DSF::File f(p.c_str(), false);
        if (f.isValid() && f.tag())
            return extractID3v2Cover(f.tag());
#endif
    }
    else if (ext == ".ogg" || ext == ".opus")
    {
        TagLib::FileRef f(p.c_str(), false);
        if (!f.isNull() && f.file())
        {
            if (auto oggVorbis = dynamic_cast<TagLib::Vorbis::File *>(f.file()))
            {
                if (oggVorbis->tag())
                    return extractXiphCover(oggVorbis->tag());
            }
            else if (auto oggOpus = dynamic_cast<TagLib::Ogg::Opus::File *>(f.file()))
            {
                if (oggOpus->tag())
                    return extractXiphCover(oggOpus->tag());
            }
            else if (auto oggFlac = dynamic_cast<TagLib::FLAC::File *>(f.file()))
            {
                if (auto xiphTag = oggFlac->xiphComment())
                    return extractXiphCover(xiphTag);
            }
        }
    }

    // 兜底
    if (data.isEmpty())
    {
        TagLib::FileRef f(p.c_str(), false);
        if (!f.isNull() && f.file())
        {
            if (auto id3v2 = dynamic_cast<TagLib::ID3v2::Tag *>(f.tag()))
                data = extractID3v2Cover(id3v2);
            else if (auto ape = dynamic_cast<TagLib::APE::Tag *>(f.tag()))
                data = extractAPECover(ape);
        }
    }
    return data;
}

// 提取 ID3v2 原始帧 (辅助)
static std::string extractRawID3v2Frame(TagLib::ID3v2::Tag *tag, const TagLib::ByteVector &frameID)
{
    if (!tag)
        return "";
    auto frameList = tag->frameList(frameID);
    if (frameList.isEmpty())
        return "";
    TagLib::ID3v2::Frame *frame = frameList.front();
    if (!frame)
        return "";
    TagLib::ByteVector rawData = frame->render();
    unsigned int headerSize = frame->headerSize();
    if (rawData.size() <= headerSize + 1)
        return "";
    TagLib::ByteVector content = rawData.mid(headerSize + 1);
    return std::string(content.data(), content.size());
}

// 歌词提取
static std::string extractLyrics(TagLib::Tag *tag, const TagLib::FileRef &fileRef)
{
    if (!tag)
        return "";
    std::string lyrics;

    // 1. MP3 USLT
    if (auto *id3v2 = dynamic_cast<TagLib::ID3v2::Tag *>(tag))
    {
        auto frames = id3v2->frameList("USLT");
        if (!frames.isEmpty())
        {
            if (auto *frame = static_cast<TagLib::ID3v2::UnsynchronizedLyricsFrame *>(frames.front()))
            {
                lyrics = frame->text().to8Bit(true);
            }
        }
    }

    // 2. 通用属性 (FLAC/MP4/APE 等)
    if (lyrics.empty())
    {
        TagLib::PropertyMap properties = fileRef.file()->properties();
        static const std::vector<std::string> lyricKeys = {"LYRICS", "UNSYNCEDLYRICS", "TEXT", "C_LYRICS"};
        for (const auto &key : lyricKeys)
        {
            if (properties.contains(key))
            {
                lyrics = properties[key].front().to8Bit(true);
                break;
            }
        }
    }
    return EncodingUtils::sanitizeUTF8(lyrics);
}

// 标签解析逻辑
static std::string resolveSafeTag(TagLib::Tag *tag, const std::string &type)
{
    if (!tag)
        return "";

    TagLib::String tagStr;
    if (type == "Title")
        tagStr = tag->title();
    else if (type == "Artist")
        tagStr = tag->artist();
    else if (type == "Album")
        tagStr = tag->album();

    if (tagStr.isEmpty())
        return "";

    // 1. 如果 TagLib 已经明确这是宽字符 (Unicode)，信赖它
    bool hasWide = false;
    for (size_t i = 0; i < tagStr.length(); ++i)
    {
        if (tagStr[i] > 255)
        {
            hasWide = true;
            break;
        }
    }

    if (hasWide)
    {
        return EncodingUtils::sanitizeUTF8(tagStr.to8Bit(true));
    }

    // 2. 否则，它完全由单字节组成
    std::string raw = tagStr.to8Bit(false);
    return EncodingUtils::detectAndConvert(raw);
}

} // namespace TagLibHelpers

// ==========================================
// 4. 图片加载工具 (Image Helpers)
// ==========================================

namespace ImageHelpers
{

static StbiPtr loadBufferAsRGBA(const unsigned char *data, int size, int &w, int &h)
{
    if (!data || size <= 0)
        return nullptr;
    int c = 0;
    return StbiPtr(stbi_load_from_memory(data, size, &w, &h, &c, 4));
}

static StbiPtr loadFileAsRGBA(const std::string &path, int &w, int &h)
{
    if (path.empty())
        return nullptr;
    std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
    if (!file.good())
        return nullptr;
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> buffer(size);
    if (!file.read(reinterpret_cast<char *>(buffer.data()), size))
        return nullptr;
    return loadBufferAsRGBA(buffer.data(), (int)size, w, h);
}

static void processTrackCover(const std::string &musicPath, const std::string &albumKey)
{
    if (albumKey.empty() || CoverCache::instance().hasKey(albumKey))
        return;

    TagLib::ByteVector data = TagLibHelpers::extractCoverDataGeneric(musicPath);
    if (!data.isEmpty())
    {
        int w = 0, h = 0;
        if (auto img = loadBufferAsRGBA(reinterpret_cast<const unsigned char *>(data.data()), (int)data.size(), w, h))
        {
            CoverCache::instance().putCompressedFromPixels(albumKey, img.get(), w, h, 4);
        }
    }
}

} // namespace ImageHelpers

// ==========================================
// 5. 音频技术参数获取 (FFmpeg)
// ==========================================

namespace AudioInfoUtils
{

struct AudioTechInfo
{
    int64_t duration = 0;
    uint32_t sampleRate = 0;
    uint16_t bitDepth = 0;
    std::string formatType;
};

static AudioTechInfo getAudioTechInfo(const std::string &filePath)
{
#ifdef ANALYSE
    // [Profile] 统计 FFmpeg 耗时
    ScopedTimer timer(Profiler::t_ffmpeg);
#endif

    AudioTechInfo info;
    AVFormatContext *ctxRaw = nullptr;
    if (avformat_open_input(&ctxRaw, filePath.c_str(), nullptr, nullptr) != 0)
        return info;
    AVContextPtr ctx(ctxRaw);

    if (avformat_find_stream_info(ctx.get(), nullptr) < 0)
        return info;
    if (ctx->duration != AV_NOPTS_VALUE)
        info.duration = ctx->duration;

    int streamIdx = av_find_best_stream(ctx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIdx >= 0)
    {
        AVStream *stream = ctx->streams[streamIdx];
        AVCodecParameters *par = stream->codecpar;

        if (info.duration == 0 && stream->duration != AV_NOPTS_VALUE)
            info.duration = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);

        info.sampleRate = par->sample_rate;
        const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
        info.formatType = desc ? desc->name : "unknown";

        if (par->bits_per_raw_sample > 0)
            info.bitDepth = par->bits_per_raw_sample;
        else
        {
            bool isLossy = desc && (desc->props & AV_CODEC_PROP_LOSSY);
            if (!isLossy)
            {
                int bytes = av_get_bytes_per_sample(static_cast<AVSampleFormat>(par->format));
                if (bytes > 0)
                    info.bitDepth = bytes * 8;
            }
        }
    }
    return info;
}

} // namespace AudioInfoUtils

// ==========================================
// 6. CUE 解析逻辑 (CUE Parsing)
// ==========================================

namespace CueUtils
{

struct CueTrackInfo
{
    int trackNum = 0;
    std::string title;
    std::string performer;
    int64_t startTime = 0;
    int64_t duration = 0;
    std::string audioFile;
};

static int64_t parseCueTime(const std::string &timeStr)
{
    int m = 0, s = 0, f = 0;
    char d;
    std::stringstream ss(timeStr);
    ss >> m >> d >> s >> d >> f;
    double totalSeconds = m * 60.0 + s + (f / 75.0);
    return static_cast<int64_t>(totalSeconds * 1000000);
}

static std::string cleanString(const std::string &str)
{
    size_t first = str.find_first_not_of(" \t\r\n\"");
    if (std::string::npos == first)
        return "";
    size_t last = str.find_last_not_of(" \t\r\n\"");
    return str.substr(first, (last - first + 1));
}

static std::vector<CueTrackInfo> parseCueFile(const fs::path &cuePath)
{
    std::vector<CueTrackInfo> tracks;
    std::ifstream file(cuePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return tracks;

    auto fileSize = file.tellg();
    if (fileSize <= 0)
        return tracks;
    std::string rawBuffer(fileSize, '\0');
    file.seekg(0, std::ios::beg);
    if (!file.read(&rawBuffer[0], fileSize))
        return tracks;

    std::string utf8Content = EncodingUtils::detectAndConvert(rawBuffer);

    size_t bomOffset = 0;
    if (utf8Content.size() >= 3 && (uint8_t)utf8Content[0] == 0xEF && (uint8_t)utf8Content[1] == 0xBB && (uint8_t)utf8Content[2] == 0xBF)
        bomOffset = 3;

    std::stringstream fileSS(utf8Content.substr(bomOffset));
    std::string line, globalPerformer, globalTitle, currentFile;
    CueTrackInfo currentTrack;
    bool inTrack = false;

    while (std::getline(fileSS, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::stringstream ss(line);
        std::string token;
        ss >> token;
        std::string upperToken = token;
        std::ranges::transform(upperToken, upperToken.begin(), ::toupper);

        if (upperToken == "FILE")
        {
            size_t firstQuote = line.find('\"');
            size_t lastQuote = line.rfind('\"');
            if (firstQuote != std::string::npos && lastQuote > firstQuote)
                currentFile = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            else
            {
                ss >> currentFile;
                currentFile = cleanString(currentFile);
            }
        }
        else if (upperToken == "TRACK")
        {
            if (inTrack)
            {
                if (currentTrack.performer.empty())
                    currentTrack.performer = globalPerformer;
                tracks.push_back(currentTrack);
            }
            inTrack = true;
            currentTrack = CueTrackInfo();
            ss >> currentTrack.trackNum;
            currentTrack.audioFile = currentFile;
        }
        else if (upperToken == "TITLE")
        {
            std::string content;
            std::getline(ss, content);
            content = cleanString(content);
            if (inTrack)
                currentTrack.title = content;
            else
                globalTitle = content;
        }
        else if (upperToken == "PERFORMER")
        {
            std::string content;
            std::getline(ss, content);
            content = cleanString(content);
            if (inTrack)
                currentTrack.performer = content;
            else
                globalPerformer = content;
        }
        else if (upperToken == "INDEX")
        {
            std::string idxStr, timeStr;
            ss >> idxStr >> timeStr;
            if (idxStr == "01" && inTrack)
                currentTrack.startTime = parseCueTime(timeStr);
        }
    }
    if (inTrack)
    {
        if (currentTrack.performer.empty())
            currentTrack.performer = globalPerformer;
        tracks.push_back(currentTrack);
    }
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (i < tracks.size() - 1 && tracks[i].audioFile == tracks[i + 1].audioFile)
        {
            tracks[i].duration = tracks[i + 1].startTime - tracks[i].startTime;
        }
    }
    return tracks;
}

static std::string findRealAudioFile(const fs::path &dirPath, const std::string &cueFileName)
{
    fs::path target = dirPath / cueFileName;
    if (fs::exists(target))
        return target.string();

    static const std::vector<std::string> fallbackExts = {".flac", ".ape", ".wv", ".wav", ".m4a", ".mp3", ".tta"};
    fs::path stem = target.parent_path() / target.stem();

    for (const auto &ext : fallbackExts)
    {
        fs::path tryPath = stem;
        tryPath.replace_extension(ext);
        if (fs::exists(tryPath))
            return tryPath.string();
    }
    return "";
}

} // namespace CueUtils

// ==========================================
// 7. 扫描逻辑核心 (Scan Logic - Optimized)
// ==========================================

namespace ScannerLogic
{

// [Worker Task] 处理单个音频节点：读取元数据 + 提取封面
static void processNodeTask(std::shared_ptr<PlaylistNode> node)
{
    if (!node)
        return;
#ifdef ANALYSE
    // [Profile] 增加文件计数
    Profiler::count_files++;
#endif
    // 1. 读取基础 Metadata (内部 TagLib 计时器在 getMetaData 里)
    MetaData md = FileScanner::getMetaData(node->getPath());

    // 2. 封面 Key 策略
    std::string albumName = md.getAlbum();
    std::string titleName = md.getTitle();
    std::string albumKey = albumName.empty() ? titleName : albumName;

    node->setCoverKey(albumKey);
    node->setMetaData(md);

    // 3. 提取嵌入封面并缓存 (CPU 密集型解压 + Resize)
    {
        // [Profile] 统计 Phase 1/2 的封面处理
#ifdef ANALYSE
        ScopedTimer timer(Profiler::t_cover_process);
#endif
        ImageHelpers::processTrackCover(node->getPath(), albumKey);
    }
}

// [Main Thread] CUE 文件逻辑：主线程解析结构，线程池处理元数据
static void handleCueFile(
    const fs::path &cuePath,
    const std::shared_ptr<PlaylistNode> &parentNode,
    std::vector<std::shared_ptr<PlaylistNode>> &batchBuffer)
{
    auto tracks = CueUtils::parseCueFile(cuePath);
    fs::path dirPath = cuePath.parent_path();
    auto &pool = SimpleThreadPool::instance().get_native_pool();

    for (auto &track : tracks)
    {
        std::string realAudioPath = CueUtils::findRealAudioFile(dirPath, track.audioFile);
        if (!realAudioPath.empty() && isffmpeg(realAudioPath))
        {
            auto trackNode = std::make_shared<PlaylistNode>(realAudioPath, false);
            parentNode->addChild(trackNode);

            // 预设 CUE 中的信息
            MetaData md;
            if (!track.title.empty())
                md.setTitle(track.title);
            if (!track.performer.empty())
                md.setArtist(track.performer);
            md.setOffset(track.startTime);
            if (track.duration > 0)
                md.setDuration(track.duration);

            md.setFilePath(realAudioPath);
            trackNode->setMetaData(md);

            // 异步任务：补充技术参数 (SampleRate/BitDepth) 和封面
            (void)pool.submit_task([trackNode, track]()
                                   {
                // 注意：这里也涉及 FFmpeg 探测，会由 getAudioTechInfo 内部的 timer 统计
                auto tech = AudioInfoUtils::getAudioTechInfo(trackNode->getPath());
                
                auto currentMd = trackNode->getMetaData();
                currentMd.setSampleRate(tech.sampleRate);
                currentMd.setBitDepth(tech.bitDepth);
                currentMd.setFormatType(tech.formatType);
                currentMd.setParentDir(fs::path(trackNode->getPath()).parent_path().string());
                
                // 如果 CUE 没算出时长，用文件总时长-偏移
                if (track.duration <= 0) {
                    int64_t rem = tech.duration - track.startTime;
                    if (rem > 0) currentMd.setDuration(rem);
                }
                
                std::string albumKey = currentMd.getAlbum().empty() ? "Unknown" : currentMd.getAlbum();
                trackNode->setCoverKey(albumKey);
                trackNode->setMetaData(currentMd);

                {
                    // [Profile] 统计 CUE 任务中的封面处理
#ifdef ANALYSE
                    ScopedTimer timer(Profiler::t_cover_process);
#endif
                     ImageHelpers::processTrackCover(trackNode->getPath(), albumKey); 
                } });
        }
    }
}

// [Main Thread] 递归扫描并分发任务
// [Optimization] 单次遍历时顺便检测封面文件，避免二次 IO
// [Fix] 修复空目录问题：只有当子目录包含有效内容时才添加到树中
static void scanAndDispatch(
    const fs::path &dirPath,
    const std::shared_ptr<PlaylistNode> &currentNode,
    std::stop_token stoken,
    std::vector<std::shared_ptr<PlaylistNode>> &batchBuffer)
{
    if (stoken.stop_requested())
        return;
    if (fs::is_symlink(dirPath))
        return;

    std::vector<fs::path> subDirs;
    auto &pool = SimpleThreadPool::instance().get_native_pool();

    // 记录本目录检测到的封面文件
    std::string detectedDirCover;

    try
    {
        for (const auto &entry : fs::directory_iterator(dirPath))
        {
            if (stoken.stop_requested())
                return;

            if (entry.is_regular_file())
            {
                std::string pathStr = entry.path().string();
                std::string ext = getLowerExt(pathStr);

                // 1. 处理音频文件
                if (hasExtension(pathStr, ".cue"))
                {
                    handleCueFile(entry.path(), currentNode, batchBuffer);
                }
                else if (isffmpeg(pathStr))
                {
                    auto fileNode = std::make_shared<PlaylistNode>(pathStr, false);
                    currentNode->addChild(fileNode);
                    batchBuffer.push_back(fileNode);

                    // [Optimization] 批量提交 + 移动语义 (Zero-Copy)
                    if (batchBuffer.size() >= K_BATCH_SIZE)
                    {
                        (void)pool.submit_task([batch = std::move(batchBuffer)]()
                                               {
                            for(const auto& node : batch) {
                                processNodeTask(node);
                            } });
                        // 移动后 batchBuffer 需要重新初始化
                        batchBuffer = std::vector<std::shared_ptr<PlaylistNode>>();
                        batchBuffer.reserve(K_BATCH_SIZE);
                    }
                }
                // 2. [Optimization] 顺便检测封面图片
                else if (detectedDirCover.empty() && kImageExts.contains(ext))
                {
                    std::string stem = entry.path().stem().string();
                    if (isCoverFileName(stem))
                    {
                        detectedDirCover = pathStr;
                        // 找到一个即可，不强制扫描所有图片
                    }
                }
            }
            else if (entry.is_directory())
            {
                subDirs.push_back(entry.path());
            }
        }
    }
    catch (...)
    {
    }

    // 如果顺路发现了封面，直接存入节点
    if (!detectedDirCover.empty())
    {
        currentNode->setCoverPath(detectedDirCover);
    }

    // 递归子目录
    for (const auto &sd : subDirs)
    {
        if (stoken.stop_requested())
            return;

        fs::path p = sd;
        p.make_preferred();
        std::string folderName = p.filename().string();
        folderName = EncodingUtils::detectAndConvert(folderName);

        // 创建子目录节点，但暂时不挂载到 currentNode
        auto childDirNode = std::make_shared<PlaylistNode>(p.string(), true);
        childDirNode->setCoverKey(folderName);

        // 先递归进入子目录进行扫描填充
        scanAndDispatch(sd, childDirNode, stoken, batchBuffer);

        // [Fix] 只有当子目录节点确实包含有效内容（文件或非空子目录）时，才将其真正添加到树中
        if (!childDirNode->getChildren().empty())
        {
            currentNode->addChild(childDirNode);
        }
    }
}

// 递归查找深层封面 (Fallback Strategy)
static std::string findDeepCoverRecursive(const std::shared_ptr<PlaylistNode> &node)
{
    // 1. 检查下一级目录是否在阶段1已经发现了封面
    for (const auto &c : node->getChildren())
    {
        if (c->isDir())
        {
            if (!c->getCoverPath().empty())
                return c->getCoverPath();
        }
    }
    // 2. 递归查找子目录
    for (const auto &c : node->getChildren())
    {
        if (c->isDir())
        {
            std::string res = findDeepCoverRecursive(c);
            if (!res.empty())
                return res;
        }
    }
    // 3. 检查子文件是否有已缓存的内嵌封面
    for (const auto &c : node->getChildren())
    {
        if (!c->isDir())
        {
            if (CoverCache::instance().hasKey(c->getCoverKey()))
                return c->getPath();
        }
    }
    return "";
}

// [Post Process] 后处理：聚合统计数据、确定封面
// [Optimization] 将目录封面的加载与解码提交到线程池并行处理
static std::pair<uint64_t, uint64_t> postProcessAggregation(const std::shared_ptr<PlaylistNode> &node)
{
    uint64_t songs = 0;
    uint64_t duration = 0;

    for (const auto &child : node->getChildren())
    {
        if (child->isDir())
        {
            auto [s, d] = postProcessAggregation(child);
            songs += s;
            duration += d;
        }
        else
        {
            songs++;
            duration += (child->getMetaData().getDuration() / 1000000);
        }
    }

    node->setTotalSongs(songs);
    node->setTotalDuration(duration);
    node->sortChildren();

    // 目录封面逻辑 (Phase 3 中的并行优化)
    if (node->isDir())
    {
        std::string cover = node->getCoverPath(); // [Optimization] 优先使用阶段1检测到的封面
        bool isAudioExtract = false;

        // 如果阶段1没找到，尝试深层递归
        if (cover.empty())
        {
            cover = findDeepCoverRecursive(node);
            if (!cover.empty())
            {
                // 如果返回的是音频文件路径，说明是内嵌封面
                if (isffmpeg(cover))
                    isAudioExtract = true;
            }
        }

        if (!cover.empty())
        {
            std::string folderName = fs::path(node->getPath()).filename().string();
            folderName = EncodingUtils::detectAndConvert(folderName);

            // [Parallel Optimization] 将封面的 IO 解码和 Resize 提交给线程池
            // 这消除了 Phase 3 中的主线程阻塞，极大提高了大目录扫描速度
            (void)SimpleThreadPool::instance().get_native_pool().submit_task(
                [cover, folderName, isAudioExtract]()
                {
            // [Profile] 统计 Phase 3 的并行封面处理耗时
#ifdef ANALYSE
                    ScopedTimer timer(Profiler::t_cover_process);
#endif

                    int w = 0, h = 0;
                    StbiPtr img(nullptr);
                    if (isAudioExtract)
                    {
                        auto d = TagLibHelpers::extractCoverDataGeneric(cover);
                        if (!d.isEmpty())
                            img = ImageHelpers::loadBufferAsRGBA((const unsigned char *)d.data(), d.size(), w, h);
                    }
                    else
                    {
                        img = ImageHelpers::loadFileAsRGBA(cover, w, h);
                    }

                    if (img)
                    {
                        // CoverCache 内部已经做了分段锁优化，并行调用是安全的
                        CoverCache::instance().putCompressedFromPixels(folderName, img.get(), w, h, 4);
                    }
                });
        }
    }

    return {songs, duration};
}

} // namespace ScannerLogic

// ==========================================
// 8. FileScanner 类实现
// ==========================================

FileScanner::FileScanner(std::string rootDir) : rootDir(std::move(rootDir))
{
}

void FileScanner::setRootDir(const std::string &rootDir)
{
    this->rootDir = rootDir;
}
const std::string FileScanner::getRootDir() const
{
    return rootDir;
}
bool FileScanner::isScanCompleted() const
{
    return hasScanCpld.load();
}
std::shared_ptr<PlaylistNode> FileScanner::getPlaylistTree() const
{
    return rootNode;
}

void FileScanner::initSupportedExtensions()
{
    std::call_once(g_initFlag, []()
                   {
        for (const auto &ext : kKnownAudioExtensions) {
            if (av_find_input_format(ext.c_str())) {
                g_supportedAudioExts.insert("." + ext);
            }
        } });
}

void FileScanner::startScan()
{
    hasScanCpld = false;
    scanThread = std::jthread([this](std::stop_token stoken)
                              { this->scanDir(stoken); });
}

void FileScanner::stopScan()
{
    if (scanThread.joinable())
    {
        scanThread.request_stop();
        scanThread.join();
    }
}

void FileScanner::scanDir(std::stop_token stoken)
{
    // [Profile] 重置计数
#ifdef ANALYSE
    Profiler::reset();
    auto start_total = std::chrono::high_resolution_clock::now();
#endif

    // 0. 初始化
    initSupportedExtensions();
    fs::path rootPath(rootDir);
    rootPath.make_preferred();

    if (stoken.stop_requested())
        return;

    if (!fs::exists(rootPath))
    {
        hasScanCpld = true;
        return;
    }

    // 1. 如果根路径本身就是文件
    if (fs::is_regular_file(rootPath))
    {
        rootNode = std::make_shared<PlaylistNode>(rootPath.string(), false);
        ScannerLogic::processNodeTask(rootNode); // 同步处理
        rootNode->setTotalSongs(1);
        rootNode->setTotalDuration(rootNode->getMetaData().getDuration() / 1000000);
        hasScanCpld = true;
        return;
    }

    // 2. 初始化根节点
    rootNode = std::make_shared<PlaylistNode>(rootPath.string(), true);
    std::string folderName = rootPath.filename().string();
    if (folderName.empty())
        folderName = rootPath.string();
    rootNode->setCoverKey(EncodingUtils::detectAndConvert(folderName));

    // =========================================================
    // Phase 1: 流水线扫描 (Pipeline Scan & Dispatch)
    // =========================================================
    {
        // [Profile] Phase 1 耗时
#ifdef ANALYSE
        ScopedTimer t1(Profiler::t_structure_scan);
#endif
        std::vector<std::shared_ptr<PlaylistNode>> batchBuffer;
        batchBuffer.reserve(K_BATCH_SIZE);

        ScannerLogic::scanAndDispatch(rootPath, rootNode, stoken, batchBuffer);

        // 提交剩余的任务 (Flush buffer)
        if (!batchBuffer.empty())
        {
            auto &pool = SimpleThreadPool::instance().get_native_pool();
            (void)pool.submit_task([batch = std::move(batchBuffer)]()
                                   {
                for(const auto& node : batch) {
                    ScannerLogic::processNodeTask(node);
                } });
        }
    }

    if (stoken.stop_requested())
        return;

    // =========================================================
    // Phase 2: 等待所有音频分析任务完成 (Wait for Audio Tasks)
    // =========================================================
    {
        // [Profile] Phase 2 耗时
#ifdef ANALYSE
        ScopedTimer t2(Profiler::t_wait_audio);
#endif
        SimpleThreadPool::instance().get_native_pool().wait();
    }

    if (stoken.stop_requested())
        return;

    // =========================================================
    // Phase 3: 后处理聚合与封面提取 (Aggregation & Dispatch Cover Tasks)
    // =========================================================
    {
        // [Profile] Phase 3 耗时
#ifdef ANALYSE
        ScopedTimer t3(Profiler::t_aggregation);
#endif
        ScannerLogic::postProcessAggregation(rootNode);
    }

    // =========================================================
    // Phase 4: 等待所有封面提取任务完成 (Wait for Cover Tasks)
    // 注意：因为 Phase 3 向线程池提交了新的任务，必须再次等待
    // =========================================================
    {
        // [Profile] Phase 4 耗时
#ifdef ANALYSE
        ScopedTimer t4(Profiler::t_wait_cover);
#endif
        SimpleThreadPool::instance().get_native_pool().wait();
    }

    hasScanCpld = true;
#ifdef ANALYSE
    // [Profile] 打印报告
    auto end_total = std::chrono::high_resolution_clock::now();
    int64_t total_us = std::chrono::duration_cast<std::chrono::microseconds>(end_total - start_total).count();
    Profiler::printReport(total_us);
#endif
}

// ==========================================
// 9. 静态公共工具函数实现
// ==========================================

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path p(musicPath);
    MetaData musicData;
    if (!fs::exists(p))
        return musicData;

    {
        // [Profile] TagLib 耗时
#ifdef ANALYSE
        ScopedTimer timer(Profiler::t_taglib);
#endif
        TagLib::FileRef f(p.c_str());
        if (!f.isNull() && f.tag())
        {
            auto tag = f.tag();
            musicData.setTitle(TagLibHelpers::resolveSafeTag(tag, "Title"));
            musicData.setArtist(TagLibHelpers::resolveSafeTag(tag, "Artist"));
            musicData.setAlbum(TagLibHelpers::resolveSafeTag(tag, "Album"));
            if (tag->year() > 0)
                musicData.setYear(std::to_string(tag->year()));
        }
    } // TagLib timer destructed

    if (musicData.getTitle().empty())
    {
        std::string filename = p.stem().string();
        musicData.setTitle(EncodingUtils::detectAndConvert(filename));
    }

    musicData.setFilePath(p.string());
    musicData.setParentDir(p.parent_path().string());

    // FFmpeg 耗时已经在 getAudioTechInfo 内部统计了
    auto tech = AudioInfoUtils::getAudioTechInfo(p.string());
    musicData.setDuration(tech.duration);
    musicData.setSampleRate(tech.sampleRate);
    musicData.setBitDepth(tech.bitDepth);
    musicData.setFormatType(tech.formatType);

    std::error_code ec;
    auto lwt = fs::last_write_time(p, ec);
    if (!ec)
        musicData.setLastWriteTime(lwt);

    return musicData;
}

std::string FileScanner::extractCoverToTempFile(MetaData &metadata)
{
    if (!metadata.getCoverPath().empty())
        return metadata.getCoverPath();

    std::string musicPath = metadata.getFilePath();
    fs::path tmpDir = fs::temp_directory_path() / "SmallestMusicPlayer";

    try
    {
        if (!fs::exists(tmpDir))
            fs::create_directories(tmpDir);
    }
    catch (...)
    {
        return "";
    }

    TagLib::ByteVector coverData = TagLibHelpers::extractCoverDataGeneric(musicPath);

    if (!coverData.isEmpty())
    {
        auto sanitize = [](const std::string &name)
        {
            std::string safe;
            for (char c : name)
                safe.push_back((c == '/' || c == '\\' || c == ':') ? '_' : c);
            return safe.empty() ? "cover" : safe;
        };
        std::string safeName = sanitize(fs::path(musicPath).stem().string());

        std::string ext = ".jpg";
        if (coverData.size() >= 4 && coverData[0] == (char)0x89 && coverData[1] == 'P' && coverData[2] == 'N' && coverData[3] == 'G')
            ext = ".png";

        fs::path targetPath = tmpDir / (safeName + ext);
        if (fs::exists(targetPath) && fs::file_size(targetPath) > 0)
            return fs::absolute(targetPath).string();

        std::ofstream outFile(targetPath, std::ios::binary | std::ios::trunc);
        if (outFile)
        {
            outFile.write(coverData.data(), coverData.size());
            return fs::absolute(targetPath).string();
        }
    }

    fs::path musicDir = fs::path(musicPath).parent_path();
    // 简单的重新实现目录封面查找逻辑
    std::string coverPath;
    for (const auto &name : kCoverFileNames)
    {
        for (const auto &ext : kImageExts)
        {
            fs::path p = musicDir / (name + ext);
            if (fs::exists(p))
            {
                coverPath = p.string();
                break;
            }
        }
        if (!coverPath.empty())
            break;
    }

    if (!coverPath.empty())
    {
        metadata.setCoverPath(coverPath);
        return coverPath;
    }
    return "";
}

// 调试辅助
static void printNodeRecursive(const std::shared_ptr<PlaylistNode> &node, std::string prefix, bool isLast)
{
    if (!node)
        return;
    fs::path p(node->getPath());
    std::string name = p.filename().string();
    if (name.empty())
        name = node->getPath();

    std::cout << std::format("{}{}{}", prefix, (isLast ? "└── " : "├── "), name);

    if (node->isDir())
    {
        std::cout << std::format(" [DIR] Songs: {}, Dur: {}s\n", node->getTotalSongs(), node->getTotalDuration());
    }
    else
    {
        const auto &md = node->getMetaData();
        try
        {
            auto st = std::chrono::clock_cast<std::chrono::system_clock>(md.getLastWriteTime());
            std::cout << std::format(" [FILE] Rate: {}Hz, Depth: {}bit, Fmt: {}, Date: {:%F}\n",
                                     md.getSampleRate(), md.getBitDepth(), md.getFormatType(), st);
        }
        catch (...)
        {
            std::cout << " [FILE] (Time Error)\n";
        }
    }

    const auto &children = node->getChildren();
    for (size_t i = 0; i < children.size(); ++i)
    {
        printNodeRecursive(children[i], prefix + (isLast ? "    " : "│   "), i == children.size() - 1);
    }
}

void printPlaylistTree(const std::shared_ptr<PlaylistNode> &root)
{
    if (!root)
    {
        std::cerr << "Root node is null.\n";
        return;
    }
    std::cout << "\n========== Playlist Tree ==========\n";
    printNodeRecursive(root, "", true);
    std::cout << "===================================\n";
}