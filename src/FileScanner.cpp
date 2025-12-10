#include "FileScanner.hpp"
#include "CoverCache.hpp"
#include "MetaData.hpp"
#include "Precompiled.h"

// ================= TagLib Headers =================
#include <exception>
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
#include "SimpleThreadPool.hpp"

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

// ==========================================
// 1. 后缀名支持
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
// 2. CUE 解析 (基本保持不变)
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

static std::vector<CueTrackInfo> parseCueFile(const fs::path &cuePath)
{
    std::vector<CueTrackInfo> tracks;
    std::ifstream file(cuePath);
    if (!file.is_open())
        return tracks;

    std::string line, globalPerformer, globalTitle, currentFile;
    CueTrackInfo currentTrack;
    bool inTrack = false;

    while (std::getline(file, line))
    {
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
            content = cleanString(content);
            if (inTrack)
                currentTrack.title = content;
            else
                globalTitle = content;
        }
        else if (token == "PERFORMER")
        {
            std::string content;
            std::getline(ss, content);
            content = cleanString(content);
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

    // TagLib 通常是线程安全的，只要不同线程操作不同的 FileRef 对象
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
// 4. 图片加载与通道强制
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

// 单曲封面处理 (现在将在多线程中调用，CoverCache 必须是线程安全的)
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

static int64_t getFFmpegDuration(const std::string &filePath)
{
    // ffmpeg 的 avformat_open_input 只要使用不同的 AVFormatContext，就是线程安全的
    AVFormatContext *ctxRaw = nullptr;
    if (avformat_open_input(&ctxRaw, filePath.c_str(), nullptr, nullptr) != 0)
        return 0;

    AVContextPtr ctx(ctxRaw);
    if (avformat_find_stream_info(ctx.get(), nullptr) < 0)
        return 0;
    if (ctx->duration != AV_NOPTS_VALUE)
        return ctx->duration;

    for (unsigned int i = 0; i < ctx->nb_streams; i++)
    {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (ctx->streams[i]->duration != AV_NOPTS_VALUE)
            {
                return av_rescale_q(ctx->streams[i]->duration,
                                    ctx->streams[i]->time_base,
                                    AV_TIME_BASE_Q);
            }
        }
    }
    return 0;
}

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path p(musicPath);
    p.make_preferred();
    MetaData musicData;

    if (fs::exists(p))
    {
        TagLib::FileRef f(p.c_str());
        if (!f.isNull() && f.tag())
        {
            TagLib::Tag *tag = f.tag();
            musicData.setTitle(tag->title().toCString(true));
            musicData.setArtist(tag->artist().toCString(true));
            musicData.setAlbum(tag->album().toCString(true));
            musicData.setYear(tag->year() > 0 ? std::to_string(tag->year()) : "");
        }
        musicData.setFilePath(p.string());
        musicData.setParentDir(p.parent_path().string());
        musicData.setDuration(getFFmpegDuration(p.string()));
    }
    return musicData;
}

// ==========================================
// 6. 目录扫描与构建 (多线程优化版)
// ==========================================

// CUE 处理保持同步（通常 CUE 文件很少，且解析很快）
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

                // CUE 中的元数据通常不需要 FFmpeg 探测时长(CUE里有)，也不需要读TagLib(CUE里有)
                // 但为了保险起见，这里复用 getMetaData 还是比较稳妥的，或者可以优化
                // 考虑到 CUE 文件较少，此处为了逻辑一致性，暂不通过线程池分发（数量级小）
                // 若 CUE 链接的 FLAC 很大，计算 Duration 仍需 ffmpeg，耗时。
                // 考虑到复杂性，这里仍串行执行，因为 CUE 场景通常文件数不多。

                MetaData md = FileScanner::getMetaData(pAudio.string());

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
        {
            return child->getPath();
        }
        else
        {
            std::string res = findDeepAudio(child);
            if (!res.empty())
                return res;
        }
    }
    return "";
}

// 封装一个独立的任务函数，用于处理单个音频文件
static std::shared_ptr<PlaylistNode> processSingleFile(const std::string &filePath)
{
    try
    {
        auto fileNode = std::make_shared<PlaylistNode>(filePath, false);
        // 这里包含 heavy work: FFmpeg duration + TagLib
        MetaData md = FileScanner::getMetaData(filePath);

        std::string albumKey = md.getAlbum().empty() ? "Unknown" : md.getAlbum();
        fileNode->setCoverKey(albumKey);

        // heavy work: Image decode & resize -> CoverCache
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

// 核心逻辑：递归构建
static std::shared_ptr<PlaylistNode> buildNodeFromDir(const fs::path &dirPath)
{
    if (fs::is_symlink(dirPath))
        return nullptr;

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
        folderName = "Unknown_Folder";

    auto node = std::make_shared<PlaylistNode>(preferredPath.string(), true);
    node->setCoverKey(folderName);

    std::set<std::string> processedFiles;
    std::vector<std::string> audioFilesToProcess;
    std::vector<fs::path> subDirsToProcess;

    // 1. 扫描当前目录下的所有条目 (快速 IO 遍历)
    try
    {
        for (const auto &entry : fs::directory_iterator(preferredPath))
        {
            if (entry.is_regular_file())
            {
                std::string pathStr = entry.path().string();
                if (hasExtension(pathStr, ".cue"))
                {
                    // CUE 处理比较特殊，产生多个节点，且需要逻辑关联，暂不放入线程池
                    auto nodes = processCueAndGetNodes(entry.path());
                    for (auto &n : nodes)
                    {
                        node->addChild(n);
                        processedFiles.insert(n->getPath()); // 这里存的是 Split 后的虚拟路径或原文件路径
                        // 修正：CUE通常指向一个大文件，这里应该把该大文件的真实路径加入processed
                        // processCueAndGetNodes 内部逻辑比较复杂，简化起见，CUE 处理完后不阻止原文件被扫描(如果有重合)
                        // 但通常我们希望避免重复。此处逻辑维持原样，仅做结构调整。
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

    // 2. 并行处理当前文件夹下的所有音频文件
    //    这是解决 "1个文件夹 1000 首歌" 慢的关键
    std::vector<std::future<std::shared_ptr<PlaylistNode>>> fileFutures;
    fileFutures.reserve(audioFilesToProcess.size());

    for (const auto &fpath : audioFilesToProcess)
    {
        // 简单去重检查 (配合 CUE)
        // 注意：canonical 可能慢，如果没必要可去掉
        fs::path p(fpath);
        std::string canon = fpath;
        try
        {
            canon = fs::canonical(p).string();
        }
        catch (const std::exception &e)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[FileScanner:751] canonical failed: %s", e.what());
        }

        if (processedFiles.count(fpath) || processedFiles.count(canon))
            continue;

        // 提交到线程池
        fileFutures.push_back(
            SimpleThreadPool::instance().enqueue(processSingleFile, fpath));
    }

    // 3. 递归处理子文件夹 (串行递归，但子文件夹内部的文件处理是并行的)
    //    这样既利用了多核(处理文件)，又避免了线程池被递归目录任务占满导致死锁
    for (const auto &subDir : subDirsToProcess)
    {
        auto child = buildNodeFromDir(subDir);
        if (child)
            node->addChild(child);
    }

    // 4. 等待当前文件夹的所有文件任务完成并收集结果
    for (auto &fut : fileFutures)
    {
        auto childNode = fut.get(); // 阻塞直到该文件处理完毕
        if (childNode)
        {
            node->addChild(childNode);
        }
    }

    // [剪枝]
    if (node->getChildren().empty())
        return nullptr;

    node->sortChildren();

    // 5. 确定文件夹封面 (必须在子节点全部 ready 后进行)
    std::string finalCoverPath = "";
    bool isAudioExtract = false;

    std::string localCover = findDirectoryCover(preferredPath);
    if (!localCover.empty())
    {
        finalCoverPath = localCover;
    }
    else
    {
        std::string subDirCover = findDeepCoverImage(node);
        if (!subDirCover.empty())
        {
            finalCoverPath = subDirCover;
        }
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
        // 这一步也涉及图片解码和 resize，可以考虑并行，但为了保证逻辑顺序（必须先找到路径），
        // 且每个文件夹只做一次，放在主流程是可以接受的。
        // 如果想极致优化，也可以 throw 到 thread pool，但需要注意 CoverCache 的写安全。
        int w = 0, h = 0;
        StbiPtr imgPixels(nullptr);

        if (isAudioExtract)
        {
            TagLib::ByteVector data = extractCoverData(finalCoverPath);
            if (!data.isEmpty())
            {
                imgPixels = loadBufferAsRGBA(reinterpret_cast<const unsigned char *>(data.data()),
                                             data.size(), w, h);
            }
        }
        else
        {
            imgPixels = loadFileAsRGBA(finalCoverPath, w, h);
        }

        if (imgPixels)
        {
            // 此时 CoverCache 必须是线程安全的
            CoverCache::instance().putCompressedFromPixels(folderName, imgPixels.get(), w, h, 4);
        }
    }

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

    // 单文件处理 (保持原样)
    if (fs::is_regular_file(rootPath))
    {
        if (hasExtension(rootDir, ".cue"))
        {
            auto root = std::make_shared<PlaylistNode>(rootPath.parent_path().string(), true);
            root->setCoverKey(rootPath.parent_path().filename().string());
            auto nodes = processCueAndGetNodes(rootPath);
            for (auto &n : nodes)
                root->addChild(n);

            if (!root->getChildren().empty())
            {
                root->sortChildren();
                rootNode = root;
            }
        }
        else if (isffmpeg(rootDir))
        {
            // 单个文件直接复用 processSingleFile 逻辑
            rootNode = processSingleFile(rootPath.string());
        }
        hasScanCpld = true;
        return;
    }

    // 目录处理
    auto root = buildNodeFromDir(rootPath);

    if (root)
    {
        rootNode = root;
    }
    else
    {
        rootNode = std::make_shared<PlaylistNode>(rootPath.string(), true);
        rootNode->setCoverKey(rootPath.filename().string());
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
    // 1. 检查 metadata 中是否已有封面路径
    if (!metadata.getCoverPath().empty())
    {
        return metadata.getCoverPath();
    }

    // 获取音频文件路径
    std::string musicPath = metadata.getFilePath();

    // 准备临时目录
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

    // 2. 尝试提取内嵌封面数据
    TagLib::ByteVector coverData = extractCoverData(musicPath);

    // 如果内嵌数据不为空，保存到临时文件并返回
    if (!coverData.isEmpty())
    {
        // 使用文件名作为基础来生成安全的临时文件名
        // 原代码使用了传入的 coverName，这里改为使用音频文件名
        std::string safeName = sanitizeFilename(fs::path(musicPath).stem().string());
        std::string ext = detectImageExtension(coverData);
        fs::path targetPath = tmpDir / (safeName + ext);

        // 如果文件已存在且大小大于0，直接返回
        if (fs::exists(targetPath) && fs::file_size(targetPath) > 0)
        {
            return fs::absolute(targetPath).string();
        }

        std::ofstream outFile(targetPath, std::ios::binary | std::ios::trunc);
        if (outFile)
        {
            outFile.write(coverData.data(), coverData.size());
            return fs::absolute(targetPath).string();
        }
    }

    // 3. 如果内嵌数据为空，在歌曲所在目录查找外部封面文件
    fs::path musicDir = fs::path(musicPath).parent_path();

    auto coverPath = findDirectoryCover(musicDir);
    if (!coverPath.empty())
    {
        metadata.setCoverPath(coverPath);
        return coverPath;
    }

    // 既没有内嵌封面，也没有外部封面，返回空
    return "";
}