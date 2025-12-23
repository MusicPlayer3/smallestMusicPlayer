#include "FileScanner.hpp"
#include "CoverCache.hpp"
#include "MetaData.hpp"
#include "PCH.h"
#include "SimpleThreadPool.hpp"

// [Modified] 引入 OpenCV 库，用于替代 stb_image 进行高性能解码
#include <opencv2/opencv.hpp>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex> // [New] C++17 读写锁，大幅提升缓存读取并发性

// ==========================================
// 0. 性能分析工具 (Profiler)
// ==========================================
#define ANALYSE
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

// FNV-1a 64-bit Hash 算法
// 用于快速计算图片数据的唯一指纹，实现 Content-Addressable Storage (CAS)
inline uint64_t fnv1a_hash(const void *data, size_t size)
{
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV_offset_basis
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= p[i];
        hash *= 0x100000001b3ULL; // FNV_prime
    }
    return hash;
}

// 将 Hash 值转为十六进制字符串 Key
std::string makeContentKey(const void *data, size_t size)
{
    if (!data || size == 0)
        return "";
    uint64_t h = fnv1a_hash(data, size);
    std::stringstream ss;
    ss << "img_" << std::hex << h; // 前缀 img_ 方便调试区分
    return ss.str();
}

} // namespace

// ==========================================
// 2. 字符编码处理 (Encoding Logic)
// ==========================================

namespace EncodingUtils
{

static std::string sanitizeUTF8(const std::string &data)
{
    std::string res;
    res.reserve(data.size());
    const unsigned char *bytes = (const unsigned char *)data.data();
    const unsigned char *end = bytes + data.size();
    while (bytes < end)
    {
        if ((*bytes & 0x80) == 0)
        {
            res += *bytes++;
            continue;
        }
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
                if ((bytes[i] & 0xC0) != 0x80)
                {
                    ok = false;
                    break;
                }
        }
        if (ok)
        {
            res.append((const char *)bytes, len);
            bytes += len;
        }
        else
        {
            res += "?";
            bytes++;
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

static std::string detectAndConvert(const std::string &rawData)
{
    if (rawData.empty())
        return "";
    std::string detectedEncoding = detectCharset(rawData);
    if (detectedEncoding.empty())
        detectedEncoding = "GB18030";
    else if (detectedEncoding == "ASCII")
        return sanitizeUTF8(rawData);
    std::string result = convertToUTF8(rawData, detectedEncoding.c_str());
    return sanitizeUTF8(result);
}

} // namespace EncodingUtils

// ==========================================
// 3. TagLib 提取工具 (TagLib Helpers)
// ==========================================

namespace TagLibHelpers
{
static TagLib::ByteVector extractID3v2Cover(TagLib::ID3v2::Tag *tag)
{
    if (!tag)
        return {};
    auto frames = tag->frameList("APIC");
    if (frames.isEmpty())
        return {};
    return static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front())->picture();
}
static TagLib::ByteVector extractAPECover(TagLib::APE::Tag *tag)
{
    if (!tag)
        return {};
    auto itemList = tag->itemListMap();
    static const std::vector<std::string> keys = {"Cover Art (Front)", "Cover Art (Back)", "Cover Art"};
    for (const auto &key : keys)
        if (itemList.contains(key))
            return itemList[key].binaryData();
    return {};
}
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
static TagLib::ByteVector extractXiphCover(TagLib::Ogg::XiphComment *tag)
{
    if (!tag)
        return {};
    auto picList = tag->pictureList();
    if (!picList.isEmpty())
        return picList.front()->data();
    return {};
}
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
    bool hasWide = false;
    for (size_t i = 0; i < tagStr.length(); ++i)
        if (tagStr[i] > 255)
        {
            hasWide = true;
            break;
        }
    if (hasWide)
        return EncodingUtils::sanitizeUTF8(tagStr.to8Bit(true));
    std::string raw = tagStr.to8Bit(false);
    return EncodingUtils::detectAndConvert(raw);
}
} // namespace TagLibHelpers

// ==========================================
// 4. 图片加载工具 (Image Helpers)
// ==========================================

namespace ImageHelpers
{
static std::string processImageDataAndGetHash(const void *data, size_t size)
{
    if (!data || size == 0)
        return "";
    std::string contentKey = makeContentKey(data, size);

    // 缓存命中则直接返回，不解码 (极快)
    if (CoverCache::instance().hasKey(contentKey))
        return contentKey;

    try
    {
#ifdef ANALYSE
        ScopedTimer timer(Profiler::t_cover_process);
#endif
        cv::Mat rawData(1, (int)size, CV_8UC1, const_cast<void *>(data));
        cv::Mat decoded = cv::imdecode(rawData, cv::IMREAD_COLOR | cv::IMREAD_REDUCED_COLOR_2 | cv::IMREAD_IGNORE_ORIENTATION);
        if (!decoded.empty())
        {
            cv::Mat rgba;
            cv::cvtColor(decoded, rgba, cv::COLOR_BGR2RGBA);
            CoverCache::instance().putCompressedFromPixels(contentKey, rgba.data, rgba.cols, rgba.rows, 4);
        }
    }
    catch (const cv::Exception &e)
    {
        spdlog::warn("OpenCV decode error: {}", e.what());
        return "";
    }
    catch (...)
    {
        return "";
    }
    return contentKey;
}
} // namespace ImageHelpers

// ==========================================
// 5. 音频技术参数获取
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
            else
                info.bitDepth = 16;
        }
    }
    return info;
}
} // namespace AudioInfoUtils

// ==========================================
// 6. CUE 解析逻辑
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
            tracks[i].duration = tracks[i + 1].startTime - tracks[i].startTime;
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

// [关键优化] 目录+专辑级别的哈希缓存 (Memoization)
// 作用：恢复原始版本的性能特性 —— 同一专辑只进行一次昂贵的 I/O 读取。
// 使用读写锁 (shared_mutex) 来保证高并发性能
static std::shared_mutex g_memoMutex;
static std::unordered_map<std::string, std::string> g_dirAlbumCache;

static void clearCache()
{
    std::unique_lock<std::shared_mutex> lock(g_memoMutex);
    g_dirAlbumCache.clear();
}

static void processNodeTask(std::shared_ptr<PlaylistNode> node)
{
    if (!node)
        return;
#ifdef ANALYSE
    Profiler::count_files++;
#endif

    // [Fix CUE Metadata Conflict]
    // 检查节点是否已经包含了由 CUE 解析器提供的元数据 (Title 不为空)
    MetaData existingMd = node->getMetaData();
    bool hasCueInfo = !existingMd.getTitle().empty();

    // 1. 读取 MetaData (I/O)
    // 即使是 CUE 分轨，我们也需要读取物理文件的 Tag 补充信息 (如 Album, Year)
    MetaData md = FileScanner::getMetaData(node->getPath());

    // [Fix] 合并元数据：CUE 的 Title/Artist 优先级高于文件的 Tag
    // 防止物理文件的元数据覆盖了 CUE 中定义的元数据
    if (hasCueInfo)
    {
        md.setTitle(existingMd.getTitle());
        if (!existingMd.getArtist().empty())
            md.setArtist(existingMd.getArtist());
        if (existingMd.getDuration() > 0)
            md.setDuration(existingMd.getDuration());
        if (existingMd.getOffset() > 0)
            md.setOffset(existingMd.getOffset());
    }

    std::string parentDir = md.getParentDir();
    if (parentDir.empty())
        parentDir = fs::path(node->getPath()).parent_path().string();
    std::string albumName = md.getAlbum();
    if (albumName.empty())
        albumName = md.getTitle();

    // 2. 构造缓存 Key (Dir + Album) - 这是恢复性能的关键
    // 同目录下的同名专辑，封面 99.9% 是一样的
    std::string memoKey = parentDir + "||" + albumName;
    std::string finalCoverKey = "";
    bool coverProcessed = false;

    // [优化] 检查内存缓存 (使用共享读锁)
    // 如果这个专辑已经被同目录下的其他歌曲算过 Hash，直接复用，完全跳过 IO！
    {
        std::shared_lock<std::shared_mutex> lock(g_memoMutex);
        auto it = g_dirAlbumCache.find(memoKey);
        if (it != g_dirAlbumCache.end())
        {
            finalCoverKey = it->second;
            coverProcessed = true;
        }
    }

    // 缓存未命中 -> 进行 I/O 读取和计算 (每个专辑仅一次)
    if (!coverProcessed)
    {
        // 策略 A: 尝试内嵌封面
        TagLib::ByteVector embData = TagLibHelpers::extractCoverDataGeneric(node->getPath());
        if (!embData.isEmpty())
        {
            finalCoverKey = ImageHelpers::processImageDataAndGetHash(embData.data(), embData.size());
            coverProcessed = true;
        }

        // 策略 B: 回退到目录封面
        if (!coverProcessed || finalCoverKey.empty())
        {
            auto parentNode = node->getParent();
            if (parentNode)
            {
                std::string dirCoverPath = parentNode->getCoverPath();
                if (!dirCoverPath.empty() && fs::exists(dirCoverPath))
                {
                    std::ifstream file(dirCoverPath, std::ios::binary | std::ios::ate);
                    if (file.good())
                    {
                        auto fileSize = file.tellg();
                        if (fileSize > 0)
                        {
                            std::vector<char> buffer(fileSize);
                            file.seekg(0, std::ios::beg);
                            file.read(buffer.data(), fileSize);
                            finalCoverKey = ImageHelpers::processImageDataAndGetHash(buffer.data(), buffer.size());
                        }
                    }
                }
            }
        }

        // [优化] 写入内存缓存 (使用独占写锁)
        {
            std::unique_lock<std::shared_mutex> lock(g_memoMutex);
            g_dirAlbumCache[memoKey] = finalCoverKey;
        }
    }

    // 3. 更新节点
    node->setMetaData(md);
    node->setCoverKey(finalCoverKey);
}

// CUE 辅助：解析并生成节点，返回被引用的音频文件路径列表
static std::vector<std::string> handleCueFile(const fs::path &cuePath, const std::shared_ptr<PlaylistNode> &parentNode)
{
    auto tracks = CueUtils::parseCueFile(cuePath);
    fs::path dirPath = cuePath.parent_path();
    auto &pool = SimpleThreadPool::instance().get_native_pool();
    std::vector<std::string> handledFiles;

    for (auto &track : tracks)
    {
        std::string realAudioPath = CueUtils::findRealAudioFile(dirPath, track.audioFile);
        if (!realAudioPath.empty() && isffmpeg(realAudioPath))
        {
            handledFiles.push_back(realAudioPath);
            auto trackNode = std::make_shared<PlaylistNode>(realAudioPath, false);
            parentNode->addChild(trackNode);

            // 预设 CUE 元数据
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

            (void)pool.submit_task([trackNode]()
                                   {
                // 异步处理：补充技术参数并处理封面 (会触发 Metadata 合并逻辑)
                auto tech = AudioInfoUtils::getAudioTechInfo(trackNode->getPath());
                auto currentMd = trackNode->getMetaData();
                currentMd.setSampleRate(tech.sampleRate);
                currentMd.setBitDepth(tech.bitDepth);
                currentMd.setFormatType(tech.formatType);
                currentMd.setParentDir(fs::path(trackNode->getPath()).parent_path().string());
                std::error_code ec;
                auto lwt = fs::last_write_time(trackNode->getPath(), ec);
                if (!ec) currentMd.setLastWriteTime(lwt);
                
                trackNode->setMetaData(currentMd);
                processNodeTask(trackNode); });
        }
    }
    return handledFiles;
}

static void scanAndDispatch(const fs::path &dirPath, const std::shared_ptr<PlaylistNode> &currentNode, std::stop_token stoken)
{
    if (stoken.stop_requested())
        return;
    if (fs::is_symlink(dirPath))
        return;

    std::vector<fs::path> subDirs;
    std::vector<std::shared_ptr<PlaylistNode>> audioNodesToSubmit;
    auto &pool = SimpleThreadPool::instance().get_native_pool();
    std::string detectedDirCover;

    // [Fix CUE Duplicate] 记录被 CUE 文件接管的音频文件
    std::unordered_set<std::string> cueHandledFiles;
    std::vector<fs::directory_entry> allEntries;

    try
    {
        for (const auto &entry : fs::directory_iterator(dirPath))
        {
            if (stoken.stop_requested())
                return;
            allEntries.push_back(entry);
        }
    }
    catch (std::exception &e)
    {
        spdlog::error("Scan error: {}", e.what());
        return;
    }

    // Pass 1: 优先处理 CUE 文件，填充黑名单
    for (const auto &entry : allEntries)
    {
        if (entry.is_regular_file() && hasExtension(entry.path().string(), ".cue"))
        {
            auto handled = handleCueFile(entry.path(), currentNode);
            cueHandledFiles.insert(handled.begin(), handled.end());
        }
    }

    // Pass 2: 处理剩余的音频文件、封面、子目录
    for (const auto &entry : allEntries)
    {
        if (stoken.stop_requested())
            return;
        if (entry.is_regular_file())
        {
            std::string pathStr = entry.path().string();
            std::string ext = getLowerExt(pathStr);

            if (hasExtension(pathStr, ".cue"))
                continue; // 已在 Pass 1 处理

            if (isffmpeg(pathStr))
            {
                // [Fix] 只有未被 CUE 接管的文件才创建节点
                if (cueHandledFiles.find(pathStr) == cueHandledFiles.end())
                {
                    auto fileNode = std::make_shared<PlaylistNode>(pathStr, false);
                    currentNode->addChild(fileNode);
                    audioNodesToSubmit.push_back(fileNode);
                }
            }
            else if (detectedDirCover.empty() && kImageExts.contains(ext))
            {
                std::string stem = entry.path().stem().string();
                if (isCoverFileName(stem))
                    detectedDirCover = pathStr;
            }
        }
        else if (entry.is_directory())
        {
            subDirs.push_back(entry.path());
        }
    }

    // 设置目录封面，供子节点回退使用
    if (!detectedDirCover.empty())
        currentNode->setCoverPath(detectedDirCover);

    // 批量提交任务
    if (!audioNodesToSubmit.empty())
    {
        size_t total = audioNodesToSubmit.size();
        for (size_t i = 0; i < total; i += K_BATCH_SIZE)
        {
            size_t end = std::min(i + K_BATCH_SIZE, total);
            std::vector<std::shared_ptr<PlaylistNode>> batch(audioNodesToSubmit.begin() + i, audioNodesToSubmit.begin() + end);
            (void)pool.submit_task([batch = std::move(batch)]()
                                   {
                for(const auto& node : batch) processNodeTask(node); });
        }
    }

    // 递归子目录
    for (const auto &sd : subDirs)
    {
        if (stoken.stop_requested())
            return;
        auto childDirNode = std::make_shared<PlaylistNode>(sd.string(), true);
        scanAndDispatch(sd, childDirNode, stoken);
        if (!childDirNode->getChildren().empty())
            currentNode->addChild(childDirNode);
    }
}

static std::string findDeepCoverRecursive(const std::shared_ptr<PlaylistNode> &node)
{
    for (const auto &c : node->getChildren())
    {
        if (c->isDir())
        {
            if (!c->getCoverPath().empty())
                return c->getCoverPath();
        }
    }
    for (const auto &c : node->getChildren())
    {
        if (c->isDir())
        {
            std::string res = findDeepCoverRecursive(c);
            if (!res.empty())
                return res;
        }
    }
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

    // 目录封面逻辑
    if (node->isDir())
    {
        std::string coverPath = node->getCoverPath();
        bool isAudioExtract = false;
        if (coverPath.empty())
        {
            coverPath = findDeepCoverRecursive(node);
            if (!coverPath.empty())
            {
                if (isffmpeg(coverPath))
                    isAudioExtract = true;
                node->setCoverPath(coverPath);
            }
        }
        if (!coverPath.empty())
        {
            (void)SimpleThreadPool::instance().get_native_pool().submit_task(
                [node, coverPath, isAudioExtract]()
                {
                    std::string key;
                    if (isAudioExtract)
                    {
                        TagLib::ByteVector data = TagLibHelpers::extractCoverDataGeneric(coverPath);
                        key = ImageHelpers::processImageDataAndGetHash(data.data(), data.size());
                    }
                    else
                    {
                        std::ifstream file(coverPath, std::ios::binary | std::ios::ate);
                        if (file.good())
                        {
                            auto sz = file.tellg();
                            if (sz > 0)
                            {
                                std::vector<char> buf(sz);
                                file.seekg(0);
                                file.read(buf.data(), sz);
                                key = ImageHelpers::processImageDataAndGetHash(buf.data(), sz);
                            }
                        }
                    }
                    if (!key.empty())
                        node->setCoverKey(key);
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
            if (av_find_input_format(ext.c_str())) g_supportedAudioExts.insert("." + ext);
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
#ifdef ANALYSE
    Profiler::reset();
    auto start_total = std::chrono::high_resolution_clock::now();
#endif
    // [优化] 扫描开始前清空 memo 缓存
    ScannerLogic::clearCache();

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

    if (fs::is_regular_file(rootPath))
    {
        rootNode = std::make_shared<PlaylistNode>(rootPath.string(), false);
        ScannerLogic::processNodeTask(rootNode);
        rootNode->setTotalSongs(1);
        rootNode->setTotalDuration(rootNode->getMetaData().getDuration() / 1000000);
        hasScanCpld = true;
        return;
    }

    rootNode = std::make_shared<PlaylistNode>(rootPath.string(), true);

    {
#ifdef ANALYSE
        ScopedTimer t1(Profiler::t_structure_scan);
#endif
        ScannerLogic::scanAndDispatch(rootPath, rootNode, stoken);
    }
    if (stoken.stop_requested())
        return;

    {
#ifdef ANALYSE
        ScopedTimer t2(Profiler::t_wait_audio);
#endif
        SimpleThreadPool::instance().get_native_pool().wait();
    }
    if (stoken.stop_requested())
        return;

    {
#ifdef ANALYSE
        ScopedTimer t3(Profiler::t_aggregation);
#endif
        ScannerLogic::postProcessAggregation(rootNode);
    }
    {
#ifdef ANALYSE
        ScopedTimer t4(Profiler::t_wait_cover);
#endif
        SimpleThreadPool::instance().get_native_pool().wait();
    }

    // [优化] 扫描结束后释放 memo 内存
    ScannerLogic::clearCache();
    hasScanCpld = true;
#ifdef ANALYSE
    auto end_total = std::chrono::high_resolution_clock::now();
    int64_t total_us = std::chrono::duration_cast<std::chrono::microseconds>(end_total - start_total).count();
    Profiler::printReport(total_us);
#endif
}

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path p(musicPath);
    MetaData musicData;
    if (!fs::exists(p))
        return musicData;
    {
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
    }
    if (musicData.getTitle().empty())
    {
        std::string filename = p.stem().string();
        musicData.setTitle(EncodingUtils::detectAndConvert(filename));
    }
    musicData.setFilePath(p.string());
    musicData.setParentDir(p.parent_path().string());
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

std::shared_ptr<PlaylistNode> FileScanner::scanFile(const std::string &path)
{
    if (!fs::exists(path) || !isffmpeg(path))
        return nullptr;
    auto node = std::make_shared<PlaylistNode>(path, false);
    ScannerLogic::processNodeTask(node);
    return node;
}

std::shared_ptr<PlaylistNode> FileScanner::scanDirectory(const std::string &path)
{
    fs::path p(path);
    if (!fs::exists(p) || !fs::is_directory(p))
        return nullptr;
    ScannerLogic::clearCache();
    auto dirNode = std::make_shared<PlaylistNode>(p.string(), true);
    ScannerLogic::scanAndDispatch(p, dirNode, std::stop_token{});
    ScannerLogic::postProcessAggregation(dirNode);
    SimpleThreadPool::instance().get_native_pool().wait();
    ScannerLogic::clearCache();
    return dirNode;
}

// 调试输出
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
        std::cout << std::format(" [DIR] Songs: {}, Dur: {}s, CoverKey: {}\n", node->getTotalSongs(), node->getTotalDuration(), node->getCoverKey());
    else
    {
        const auto &md = node->getMetaData();
        std::cout << std::format(" [FILE] Rate: {}Hz, CoverKey: {}\n", md.getSampleRate(), node->getCoverKey());
    }
    const auto &children = node->getChildren();
    for (size_t i = 0; i < children.size(); ++i)
        printNodeRecursive(children[i], prefix + (isLast ? "    " : "│   "), i == children.size() - 1);
}
void printPlaylistTree(const std::shared_ptr<PlaylistNode> &root)
{
    if (!root)
        return;
    std::cout << "\n========== Playlist Tree ==========\n";
    printNodeRecursive(root, "", true);
    std::cout << "===================================\n";
}