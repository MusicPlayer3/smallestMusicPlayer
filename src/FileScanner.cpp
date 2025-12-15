#include "FileScanner.hpp"
#include "CoverCache.hpp"
#include "MetaData.hpp"
#include "Precompiled.h"
#include <print>
#include <chrono>
#include <filesystem>
#include <format>

// ================= TagLib Headers =================
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tfile.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/asffile.h>
#include <taglib/asftag.h>
#include <taglib/asfpicture.h>
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
#include <taglib/dsffile.h>

#include <queue>
#include <condition_variable>
#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <sstream> // 确保引入 stringstream
#include "SimpleThreadPool.hpp"

// 引入 uchardet
#include <uchardet.h>

// 引入 SDL 用于字符编码转换
#include <SDL.h>

namespace fs = std::filesystem;

// ==========================================
// RAII Helpers
// ==========================================

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

// ==========================================
// 编码转换与探测工具
// ==========================================

class EncodingDetector
{
public:
    static std::string detectAndConvert(const std::string &rawData)
    {
        if (rawData.empty())
            return "";

        // 1. 快速检查：如果是合法的 UTF-8，直接返回
        if (isValidUTF8(rawData))
        {
            return rawData;
        }

        // 2. 使用 uchardet 进行探测
        std::string detectedEncoding = detectCharset(rawData);

        // 3. 兜底策略
        if (detectedEncoding.empty())
        {
            // 如果探测失败，对于 CUE 文件，国内环境最可能是 GB18030
            detectedEncoding = "GB18030";
        }

        // 4. 执行转换
        if (detectedEncoding == "UTF-8" || detectedEncoding == "ASCII")
        {
            return rawData;
        }

        return convertToUTF8(rawData, detectedEncoding.c_str());
    }

private:
    static std::string detectCharset(const std::string &data)
    {
        UchardetPtr ud(uchardet_new());
        if (!ud)
            return "";

        int retval = uchardet_handle_data(ud.get(), data.c_str(), data.length());
        if (retval != 0)
            return "";

        uchardet_data_end(ud.get());

        const char *charset = uchardet_get_charset(ud.get());
        return charset ? std::string(charset) : "";
    }

    static bool isValidUTF8(const std::string &data)
    {
        const unsigned char *bytes = (const unsigned char *)data.c_str();
        while (*bytes)
        {
            if ((*bytes & 0x80) == 0)
                bytes++;
            else if ((*bytes & 0xE0) == 0xC0)
            {
                if ((bytes[1] & 0xC0) != 0x80)
                    return false;
                bytes += 2;
            }
            else if ((*bytes & 0xF0) == 0xE0)
            {
                if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80)
                    return false;
                bytes += 3;
            }
            else if ((*bytes & 0xF8) == 0xF0)
            {
                if ((bytes[1] & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80)
                    return false;
                bytes += 4;
            }
            else
                return false;
        }
        return true;
    }

    static std::string convertToUTF8(const std::string &data, const char *fromEncoding)
    {
        if (data.empty())
            return "";

        SDL_iconv_t cd = SDL_iconv_open("UTF-8", fromEncoding);
        if (cd == (SDL_iconv_t)-1)
        {
            if (std::string(fromEncoding) == "GB2312")
                return convertToUTF8(data, "GB18030");
            return data;
        }

        size_t inLeft = data.size();
        const char *inBuf = data.c_str();

        size_t outLen = inLeft * 4 + 1;
        std::vector<char> outBuf(outLen);
        char *outPtr = outBuf.data();
        size_t outLeft = outLen;

        size_t res = SDL_iconv(cd, &inBuf, &inLeft, &outPtr, &outLeft);
        SDL_iconv_close(cd);

        if (res == (size_t)-1)
        {
            return data;
        }

        return std::string(outBuf.data(), outLen - outLeft);
    }
};

// ==========================================
// 1. 后缀名支持 (保持不变)
// ==========================================

static std::unordered_set<std::string> g_supportedAudioExts;
static std::once_flag g_initFlag;

static const std::vector<std::string> kKnownAudioExtensions = {
    "mp3", "aac", "m4a", "ogg", "wma", "opus", "mpc", "mp+", "mpp",
    "flac", "ape", "wav", "aiff", "aif", "wv", "tta", "alac", "shn", "tak",
    "dsf", "dff", "dxd", "mka", "webm",
    "mod", "it", "s3m", "xm", "mtm", "vgm", "vgz", "spc", "psf", "psf2",
    "adx", "hca", "brstm", "bcstm", "bfstm",
    "au", "snd", "voc", "ra", "rm", "amr", "awb", "dts", "ac3", "truehd"};

void FileScanner::initSupportedExtensions()
{
    std::call_once(g_initFlag, []()
                   {
        for (const auto &ext : kKnownAudioExtensions)
        {
            if (av_find_input_format(ext.c_str()))
            {
                g_supportedAudioExts.insert("." + ext);
            }
        } });
}

static std::string getLowerExtension(const std::string &pathStr)
{
    fs::path p(pathStr);
    if (!p.has_extension())
        return "";
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

inline bool isffmpeg(const std::string &route)
{
    std::string ext = getLowerExtension(route);
    if (g_supportedAudioExts.find(ext) == g_supportedAudioExts.end())
        return false;
    return true;
}

inline bool hasExtension(const std::string &filename, const std::string &ext)
{
    if (filename.length() < ext.length())
        return false;
    auto itFn = filename.rbegin();
    auto itExt = ext.rbegin();
    while (itExt != ext.rend())
    {
        if (std::tolower(*itFn) != std::tolower(*itExt))
            return false;
        ++itFn;
        ++itExt;
    }
    return true;
}

// ==========================================
// 2. CUE 解析 (关键修改：整体探测)
// ==========================================

static int64_t parseCueTime(const std::string &timeStr)
{
    int m = 0, s = 0, f = 0;
    char d;
    std::stringstream ss(timeStr);
    ss >> m >> d >> s >> d >> f;
    double totalSeconds = m * 60.0 + s + (f / 75.0);
    return static_cast<int64_t>(totalSeconds * 1000000);
}

struct CueTrackInfo
{
    int trackNum = 0;
    std::string title;
    std::string performer;
    int64_t startTime = 0;
    int64_t duration = 0;
    std::string audioFile;
};

static std::string cleanString(const std::string &str)
{
    size_t first = str.find_first_not_of(" \t\r\n\"");
    if (std::string::npos == first)
        return "";
    size_t last = str.find_last_not_of(" \t\r\n\"");
    return str.substr(first, (last - first + 1));
}

// [重要修改]：parseCueFile 改为先读取全部内容，统一探测编码，再解析
static std::vector<CueTrackInfo> parseCueFile(const fs::path &cuePath)
{
    std::vector<CueTrackInfo> tracks;

    // 1. 读取整个 CUE 文件到 buffer
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

    // 2. 对整个文件内容进行编码探测和转换
    //    这样 uchardet 拥有最大上下文，能准确区分 GB18030 和 Latin-1
    std::string utf8Content = EncodingDetector::detectAndConvert(rawBuffer);

    // 3. 处理 UTF-8 BOM (如果存在)
    //    虽然 detectAndConvert 内的 isValidUTF8 可能已经处理了 BOM 后的有效性，
    //    但 stringstream 不会跳过 BOM，可能导致第一行解析失败。
    size_t bomOffset = 0;
    if (utf8Content.size() >= 3 && (unsigned char)utf8Content[0] == 0xEF && (unsigned char)utf8Content[1] == 0xBB && (unsigned char)utf8Content[2] == 0xBF)
    {
        bomOffset = 3;
    }

    // 4. 使用 stringstream 解析转换后的 UTF-8 文本
    std::stringstream fileSS(utf8Content.substr(bomOffset));

    std::string line, globalPerformer, globalTitle, currentFile;
    CueTrackInfo currentTrack;
    bool inTrack = false;

    // 接下来的逻辑是标准的 ASCII/UTF-8 解析，不需要再做 detectAndConvert
    while (std::getline(fileSS, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::string token;
        std::stringstream ss(line);
        ss >> token;
        std::transform(token.begin(), token.end(), token.begin(), ::toupper);

        if (token == "FILE")
        {
            size_t firstQuote = line.find('\"');
            size_t lastQuote = line.rfind('\"');
            if (firstQuote != std::string::npos && lastQuote > firstQuote)
            {
                currentFile = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            }
            else
            {
                ss >> currentFile;
                currentFile = cleanString(currentFile);
            }
        }
        else if (token == "TRACK")
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
        else if (token == "TITLE")
        {
            std::string content;
            std::getline(ss, content);
            content = cleanString(content); // content 已经是 UTF-8

            if (inTrack)
                currentTrack.title = content;
            else
                globalTitle = content;
        }
        else if (token == "PERFORMER")
        {
            std::string content;
            std::getline(ss, content);
            content = cleanString(content); // content 已经是 UTF-8

            if (inTrack)
                currentTrack.performer = content;
            else
                globalPerformer = content;
        }
        else if (token == "INDEX")
        {
            std::string idxStr, timeStr;
            ss >> idxStr >> timeStr;
            if (idxStr == "01" && inTrack)
            {
                currentTrack.startTime = parseCueTime(timeStr);
            }
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

    static const std::vector<std::string> fallbackExts = {
        ".flac", ".ape", ".wv", ".wav", ".m4a", ".tak", ".tta", ".mp3"};

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

// ==========================================
// 3. 封面提取基础 (TagLib)
// ==========================================

static TagLib::ByteVector extractID3v2Cover(TagLib::ID3v2::Tag *tag)
{
    if (!tag)
        return TagLib::ByteVector();
    auto frames = tag->frameList("APIC");
    if (frames.isEmpty())
        return TagLib::ByteVector();
    return static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front())->picture();
}

static TagLib::ByteVector extractCoverData(const std::string &musicPath)
{
    fs::path p(musicPath);
    p.make_preferred();
    std::string ext = getLowerExtension(musicPath);

    if (ext == ".mp3" || ext == ".wav" || ext == ".aiff" || ext == ".aif")
    {
        TagLib::FileRef f(p.c_str(), false);
        if (!f.isNull() && f.file())
        {
            if (auto mp3 = dynamic_cast<TagLib::MPEG::File *>(f.file()))
            {
                if (mp3->ID3v2Tag())
                    return extractID3v2Cover(mp3->ID3v2Tag());
            }
            else if (auto wav = dynamic_cast<TagLib::RIFF::WAV::File *>(f.file()))
            {
                if (wav->ID3v2Tag())
                    return extractID3v2Cover(wav->ID3v2Tag());
            }
        }
    }
    else if (ext == ".flac")
    {
        TagLib::FLAC::File file(p.c_str(), false);
        if (file.isValid() && !file.pictureList().isEmpty())
            return file.pictureList()[0]->data();
    }
    else if (ext == ".m4a" || ext == ".mp4" || ext == ".aac")
    {
        TagLib::MP4::File file(p.c_str(), false);
        if (file.isValid() && file.tag() && file.tag()->itemMap().contains("covr"))
        {
            auto list = file.tag()->itemMap()["covr"].toCoverArtList();
            if (!list.isEmpty())
                return list.front().data();
        }
    }
    else if (ext == ".wma" || ext == ".asf")
    {
        TagLib::ASF::File file(p.c_str(), false);
        if (file.isValid() && file.tag() && file.tag()->attributeListMap().contains("WM/Picture"))
        {
            auto list = file.tag()->attributeListMap()["WM/Picture"];
            if (!list.isEmpty())
                return list.front().toPicture().picture();
        }
    }
    return TagLib::ByteVector();
}

static std::string findDirectoryCover(const fs::path &dirPath)
{
    static const std::vector<std::string> coverNames = {"cover", "folder", "front", "album", "art", dirPath.filename().string()};
    static const std::unordered_set<std::string> exts = {".jpg", ".jpeg", ".png", ".bmp"};

    try
    {
        for (const auto &entry : fs::directory_iterator(dirPath))
        {
            if (!entry.is_regular_file())
                continue;

            std::string stem = entry.path().stem().string();
            std::string ext = entry.path().extension().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (exts.count(ext))
            {
                for (const auto &name : coverNames)
                {
                    std::string lowerName = name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                    if (stem == lowerName)
                        return entry.path().string();
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to scan directory cover: %s", e.what());
    }
    return "";
}

// ==========================================
// 4. 图片加载
// ==========================================

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
    fs::path p(path);
    p.make_preferred();
    std::ifstream file(p, std::ios::binary | std::ios::ate);
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
    if (albumKey.empty())
        return;
    TagLib::ByteVector data = extractCoverData(musicPath);
    if (!data.isEmpty())
    {
        int w = 0, h = 0;
        StbiPtr imgPixels = loadBufferAsRGBA(reinterpret_cast<const unsigned char *>(data.data()),
                                             static_cast<int>(data.size()), w, h);
        if (imgPixels)
        {
            CoverCache::instance().putCompressedFromPixels(albumKey, imgPixels.get(), w, h, 4);
        }
    }
}

// ==========================================
// 5. 元数据获取
// ==========================================

struct AudioTechInfo
{
    int64_t duration = 0; // 微秒
    uint32_t sampleRate = 0;
    uint16_t bitDepth = 0;
    std::string formatType;
};

static AudioTechInfo getAudioTechInfo(const std::string &filePath)
{
    AudioTechInfo info;
    // 初始化 info.bitDepth 为 0，这是一个良好的默认值
    info.bitDepth = 0;
    info.duration = 0;

    AVFormatContext *ctxRaw = nullptr;
    if (avformat_open_input(&ctxRaw, filePath.c_str(), nullptr, nullptr) != 0)
        return info;

    AVContextPtr ctx(ctxRaw); // 假设你有自定义的智能指针封装
    if (avformat_find_stream_info(ctx.get(), nullptr) < 0)
        return info;

    // 获取时长
    if (ctx->duration != AV_NOPTS_VALUE)
    {
        info.duration = ctx->duration;
    }

    // 查找最佳音频流
    int streamIdx = av_find_best_stream(ctx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIdx >= 0)
    {
        AVStream *stream = ctx->streams[streamIdx];
        AVCodecParameters *par = stream->codecpar;

        // 1. 时长回退逻辑
        if (info.duration == 0 && stream->duration != AV_NOPTS_VALUE)
        {
            info.duration = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
        }

        // 2. 采样率
        info.sampleRate = par->sample_rate;

        // 获取编码描述符 (提前获取，后面位深度判断也要用)
        const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);

        // 3. 格式类型
        if (desc && desc->name)
        {
            info.formatType = desc->name;
        }
        else
        {
            info.formatType = "unknown";
        }

        // 4.位深度 (Bit Depth)
        if (par->bits_per_raw_sample > 0)
        {
            // 如果容器显式声明了位深 (如 FLAC 24bit, WAV 16bit)，直接使用
            info.bitDepth = par->bits_per_raw_sample;
        }
        else
        {
            // 如果 bits_per_raw_sample 为 0 (常见于 MP3, AAC, Vorbis, 浮点 WAV)
            // 检查是否为“有损压缩”格式
            // 如果是 Lossy 格式，位深度概念不适用，保持为 0，或者你可以强制设为 16
            bool isLossy = desc && (desc->props & AV_CODEC_PROP_LOSSY);

            if (!isLossy)
            {
                // 只有在非有损压缩 (即 PCM / Lossless) 且没有 bits_per_raw_sample 时，
                // 我们才根据解码后的 sample_fmt 推算位深。
                int bytes = av_get_bytes_per_sample(static_cast<AVSampleFormat>(par->format));
                if (bytes > 0)
                {
                    info.bitDepth = bytes * 8;
                }
            }
            
        }
    }

    return info;
}

// 辅助：从 TagLib::String 提取并探测编码
static std::string resolveTag(const TagLib::String &tagStr)
{
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
        return tagStr.toCString(true); // 转为 UTF-8

    // 2. 否则取原始字节流，交给 uchardet
    std::string raw = tagStr.to8Bit(false);
    return EncodingDetector::detectAndConvert(raw);
}

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path p(musicPath);
    p.make_preferred();
    MetaData musicData;

    if (fs::exists(p))
    {
        // 1. TagLib 获取常规标签
        TagLib::FileRef f(p.c_str());
        if (!f.isNull() && f.tag())
        {
            TagLib::Tag *tag = f.tag();
            musicData.setTitle(resolveTag(tag->title()));
            musicData.setArtist(resolveTag(tag->artist()));
            musicData.setAlbum(resolveTag(tag->album()));
            musicData.setYear(tag->year() > 0 ? std::to_string(tag->year()) : "");
        }
        musicData.setFilePath(p.string());
        musicData.setParentDir(p.parent_path().string());

        // 2. FFmpeg 获取技术参数 (时长、采样率、位深、格式)
        AudioTechInfo techInfo = getAudioTechInfo(p.string());
        musicData.setDuration(techInfo.duration);
        musicData.setSampleRate(techInfo.sampleRate);
        musicData.setBitDepth(techInfo.bitDepth);
        musicData.setFormatType(techInfo.formatType);

        // 3. 获取文件最后修改时间
        std::error_code ec;
        auto lastWriteTime = fs::last_write_time(p, ec);
        if (!ec)
        {
            musicData.setLastWriteTime(lastWriteTime);
        }
    }
    return musicData;
}

// ==========================================
// 6. 目录扫描与构建
// ==========================================

static std::vector<std::shared_ptr<PlaylistNode>> processCueAndGetNodes(
    const fs::path &cuePath)
{
    std::vector<std::shared_ptr<PlaylistNode>> resultNodes;
    try
    {
        fs::path dirPath = cuePath.parent_path();
        auto tracks = parseCueFile(cuePath);

        for (auto &track : tracks)
        {
            std::string realAudioPath = findRealAudioFile(dirPath, track.audioFile);
            if (!realAudioPath.empty() && isffmpeg(realAudioPath))
            {
                fs::path pAudio(realAudioPath);
                pAudio.make_preferred();

                auto trackNode = std::make_shared<PlaylistNode>(pAudio.string(), false);
                MetaData md = FileScanner::getMetaData(pAudio.string()); // 内部已处理编码

                // CUE 解析后的 track 信息已经是 UTF-8，直接覆盖
                if (!track.title.empty())
                    md.setTitle(track.title);
                if (!track.performer.empty())
                    md.setArtist(track.performer);

                md.setOffset(track.startTime);
                if (track.duration > 0)
                    md.setDuration(track.duration);
                else
                {
                    int64_t rem = md.getDuration() - track.startTime;
                    if (rem > 0)
                        md.setDuration(rem);
                }

                std::string albumKey = md.getAlbum().empty() ? "Unknown" : md.getAlbum();
                trackNode->setCoverKey(albumKey);
                processTrackCover(pAudio.string(), albumKey);
                trackNode->setMetaData(md);
                resultNodes.push_back(trackNode);
            }
        }
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[FileScanner:607]:Error processing cue file %s: %s", cuePath.c_str(), e.what());
    }
    return resultNodes;
}

static std::string findDeepCoverImage(const std::shared_ptr<PlaylistNode> &node)
{
    for (const auto &child : node->getChildren())
    {
        if (child->isDir())
        {
            std::string cover = findDirectoryCover(child->getPath());
            if (!cover.empty())
                return cover;
        }
    }
    for (const auto &child : node->getChildren())
    {
        if (child->isDir())
        {
            std::string cover = findDeepCoverImage(child);
            if (!cover.empty())
                return cover;
        }
    }
    return "";
}

static std::string findDeepAudio(const std::shared_ptr<PlaylistNode> &node)
{
    for (const auto &child : node->getChildren())
    {
        if (!child->isDir())
            return child->getPath();
        else
        {
            std::string res = findDeepAudio(child);
            if (!res.empty())
                return res;
        }
    }
    return "";
}

static std::shared_ptr<PlaylistNode> processSingleFile(const std::string &filePath)
{
    try
    {
        auto fileNode = std::make_shared<PlaylistNode>(filePath, false);
        MetaData md = FileScanner::getMetaData(filePath);

        std::string albumKey = md.getAlbum().empty() ? md.getTitle() : md.getAlbum();
        fileNode->setCoverKey(albumKey);
        processTrackCover(filePath, albumKey);
        fileNode->setMetaData(md);
        return fileNode;
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[FileScanner:672]:Error processing file %s: %s", filePath.c_str(), e.what());
        return nullptr;
    }
}

static std::shared_ptr<PlaylistNode> buildNodeFromDir(const fs::path &dirPath)
{
    if (fs::is_symlink(dirPath))
    {
        return nullptr;
    }

    fs::path preferredPath = dirPath;
    preferredPath.make_preferred();
    std::string dirPathStr = preferredPath.string();
    if (dirPathStr.length() > 1 && (dirPathStr.back() == '/' || dirPathStr.back() == '\\'))
    {
        dirPathStr.pop_back();
        preferredPath = fs::path(dirPathStr);
    }

    std::string folderName = preferredPath.filename().string();
    if (folderName.empty())
    {
        folderName = "Unknown_Folder";
    }

    folderName = EncodingDetector::detectAndConvert(folderName);

    auto node = std::make_shared<PlaylistNode>(preferredPath.string(), true);
    node->setCoverKey(folderName);

    std::set<std::string> processedFiles;
    std::vector<std::string> audioFilesToProcess;
    std::vector<fs::path> subDirsToProcess;

    try
    {
        for (const auto &entry : fs::directory_iterator(preferredPath))
        {
            if (entry.is_regular_file())
            {
                std::string pathStr = entry.path().string();
                if (hasExtension(pathStr, ".cue"))
                {
                    auto nodes = processCueAndGetNodes(entry.path());
                    for (auto &n : nodes)
                    {
                        node->addChild(n);
                        processedFiles.insert(n->getPath());
                    }
                }
                else if (isffmpeg(pathStr))
                {
                    audioFilesToProcess.push_back(pathStr);
                }
            }
            else if (entry.is_directory())
            {
                subDirsToProcess.push_back(entry.path());
            }
        }
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[FileScanner:737] directory_iterator failed: %s", e.what());
    }

    std::vector<std::future<std::shared_ptr<PlaylistNode>>> fileFutures;
    fileFutures.reserve(audioFilesToProcess.size());

    for (const auto &fpath : audioFilesToProcess)
    {
        fs::path p(fpath);
        std::string canon = fpath;
        try
        {
            canon = fs::canonical(p).string();
        }
        catch (...)
        {
        }

        if (processedFiles.count(fpath) || processedFiles.count(canon))
            continue;
        fileFutures.push_back(SimpleThreadPool::instance().enqueue(processSingleFile, fpath));
    }

    for (const auto &subDir : subDirsToProcess)
    {
        auto child = buildNodeFromDir(subDir);
        if (child)
            node->addChild(child);
    }

    for (auto &fut : fileFutures)
    {
        auto childNode = fut.get();
        if (childNode)
            node->addChild(childNode);
    }

    if (node->getChildren().empty())
        return nullptr;
    node->sortChildren();

    std::string finalCoverPath = "";
    bool isAudioExtract = false;

    std::string localCover = findDirectoryCover(preferredPath);
    if (!localCover.empty())
        finalCoverPath = localCover;
    else
    {
        std::string subDirCover = findDeepCoverImage(node);
        if (!subDirCover.empty())
            finalCoverPath = subDirCover;
        else
        {
            std::string firstAudio = findDeepAudio(node);
            if (!firstAudio.empty())
            {
                finalCoverPath = firstAudio;
                isAudioExtract = true;
            }
        }
    }

    if (!finalCoverPath.empty())
    {
        int w = 0, h = 0;
        StbiPtr imgPixels(nullptr);
        if (isAudioExtract)
        {
            TagLib::ByteVector data = extractCoverData(finalCoverPath);
            if (!data.isEmpty())
                imgPixels = loadBufferAsRGBA(reinterpret_cast<const unsigned char *>(data.data()), data.size(), w, h);
        }
        else
        {
            imgPixels = loadFileAsRGBA(finalCoverPath, w, h);
        }

        if (imgPixels)
        {
            CoverCache::instance().putCompressedFromPixels(folderName, imgPixels.get(), w, h, 4);
        }
    }

    // 统计当前文件夹下的总歌曲数和总时长
    uint64_t totalSongs = 0;
    uint64_t totalDurationSec = 0;

    for (const auto &child : node->getChildren())
    {
        if (child->isDir())
        {
            // 如果是子文件夹，累加子文件夹的统计数据
            totalSongs += child->getTotalSongs();
            totalDurationSec += child->getTotalDuration();
        }
        else
        {
            // 如果是文件（包括 CUE 分轨后的节点，通常也是叶子节点）
            totalSongs++;
            // MetaData 中的 Duration 是微秒，PlaylistNode 需要秒
            totalDurationSec += (child->getMetaData().getDuration() / 1000000);
        }
    }

    node->setTotalSongs(totalSongs);
    node->setTotalDuration(totalDurationSec);
    return node;
}

void FileScanner::scanDir()
{
    initSupportedExtensions();
    fs::path rootPath(rootDir);
    rootPath.make_preferred();

    if (!fs::exists(rootPath))
    {
        hasScanCpld = true;
        return;
    }

    if (fs::is_regular_file(rootPath))
    {
        if (hasExtension(rootDir, ".cue"))
        {
            auto root = std::make_shared<PlaylistNode>(rootPath.parent_path().string(), true);
            root->setCoverKey(rootPath.parent_path().filename().string());
            auto nodes = processCueAndGetNodes(rootPath);

            uint64_t tSongs = 0;
            uint64_t tDuration = 0;

            for (auto &n : nodes)
            {
                root->addChild(n);
                tSongs++;
                tDuration += (n->getMetaData().getDuration() / 1000000);
            }

            root->setTotalSongs(tSongs);
            root->setTotalDuration(tDuration);

            if (!root->getChildren().empty())
            {
                root->sortChildren();
                rootNode = root;
            }
        }
        else if (isffmpeg(rootDir))
        {
            rootNode = processSingleFile(rootPath.string());
            // 单个文件节点本身不算作“列表”，但如果是根节点，也可以视作包含1首歌
            if (rootNode)
            {
                rootNode->setTotalSongs(1);
                rootNode->setTotalDuration(rootNode->getMetaData().getDuration() / 1000000);
            }
        }
        hasScanCpld = true;
        return;
    }

    // 目录扫描情况，buildNodeFromDir 内部已经计算了统计信息
    auto root = buildNodeFromDir(rootPath);
    if (root)
        rootNode = root;
    else
    {
        rootNode = std::make_shared<PlaylistNode>(rootPath.string(), true);
        rootNode->setCoverKey(rootPath.filename().string());
        rootNode->setTotalSongs(0);
        rootNode->setTotalDuration(0);
    }
    hasScanCpld = true;
}

// ==========================================
// 辅助工具 (保持不变)
// ==========================================

static bool isIllegalChar(char c)
{
#ifdef _WIN32
    static const char *illegal = "<>:\"/\\|?*";
    return (c >= 0 && c < 32) || std::strchr(illegal, c) != nullptr;
#else
    return (c == '/' || c == '\0');
#endif
}

static std::string sanitizeFilename(const std::string &name)
{
    std::string safeName;
    safeName.reserve(name.size());
    for (char c : name)
        safeName.push_back(isIllegalChar(c) ? '_' : c);
    auto notSpace = [](unsigned char c)
    { return !std::isspace(c); };
    if (std::none_of(safeName.begin(), safeName.end(), notSpace))
        return "Unknown_Album";
    return safeName;
}

static std::string detectImageExtension(const TagLib::ByteVector &data)
{
    if (data.size() >= 3 && data[0] == '\xFF' && data[1] == '\xD8')
        return ".jpg";
    if (data.size() >= 8 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return ".png";
    return ".jpg";
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
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "[FileScanner:964]:Failed to create temp directory: %s", e.what());
        return "";
    }

    TagLib::ByteVector coverData = extractCoverData(musicPath);
    if (!coverData.isEmpty())
    {
        std::string safeName = sanitizeFilename(fs::path(musicPath).stem().string());
        std::string ext = detectImageExtension(coverData);
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
    auto coverPath = findDirectoryCover(musicDir);
    if (!coverPath.empty())
    {
        metadata.setCoverPath(coverPath);
        return coverPath;
    }
    return "";
}

// 辅助函数：将 file_time_type 转换为本地时间字符串
// 利用 C++23 的 zoned_time 自动处理时区
std::string formatTime(std::filesystem::file_time_type ftime)
{
    if (ftime == std::filesystem::file_time_type::min())
        return "Unknown Time";

    try
    {
        // 1. 将文件时间转换为系统时间 (clock_cast 是 C++20 引入)
        auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);

        // 2. 转换为当前时区的本地时间 (zoned_time 是 C++20 引入)
        // 注意：这通常需要编译器链接了时区数据库 (ICU 或 OS API)
        auto localTime = std::chrono::zoned_time(std::chrono::current_zone(), sysTime);

        // 3. 格式化输出，{:%F %T} 等同于 %Y-%m-%d %H:%M:%S
        return std::format("{:%F %T}", localTime);
    }
    catch (...)
    {
        // 如果时区转换失败（极少数环境），回退到系统时间显示
        auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        return std::format("{:%F %T} (UTC)", sysTime);
    }
}

// 内部递归打印函数
void printNodeRecursive(const std::shared_ptr<PlaylistNode> &node, std::string prefix, bool isLast)
{
    if (!node)
        return;

    // 获取显示名称
    std::filesystem::path p(node->getPath());
    std::string name = p.filename().string();
    if (name.empty())
        name = node->getPath(); // 处理根目录可能是 "C:" 或 "/" 的情况

    // 1. 打印树状前缀和名称 (C++23 std::print)
    std::print("{}{}{}", prefix, (isLast ? "└── " : "├── "), name);

    // 2. 打印属性信息
    if (node->isDir())
    {
        // 文件夹：totalSongs, totalDuration
        std::println(" [DIR] Songs: {}, Duration: {}s",
                     node->getTotalSongs(),
                     node->getTotalDuration());
    }
    else
    {
        // 音频文件：lastWriteTime, sampleRate, bitDepth, formatType
        const auto &md = node->getMetaData();
        std::println(" [FILE] LastMod: {}, Rate: {}Hz, Depth: {}bit, Fmt: {}",
                     formatTime(md.getLastWriteTime()),
                     md.getSampleRate(),
                     md.getBitDepth(),
                     md.getFormatType());
    }

    // 3. 递归处理子节点
    const auto &children = node->getChildren();
    for (size_t i = 0; i < children.size(); ++i)
    {
        bool lastChild = (i == children.size() - 1);
        std::string nextPrefix = prefix + (isLast ? "    " : "│   ");
        printNodeRecursive(children[i], nextPrefix, lastChild);
    }
}

// 外部调用接口
void printPlaylistTree(const std::shared_ptr<PlaylistNode> &root)
{
    if (!root)
    {
        std::println(stderr, "Root node is null.");
        return;
    }

    std::println("\n========== Playlist Tree (C++23) ==========");
    printNodeRecursive(root, "", true);
    std::println("===========================================\n");
}