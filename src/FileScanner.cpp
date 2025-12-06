#include "FileScanner.hpp"
#include "CoverCache.hpp"
#include "MetaData.hpp"
#include "AudioPlayer.hpp"
#include "Precompiled.h"

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
// 2. CUE 解析
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

static std::string cleanString(std::string str)
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
    static const std::vector<std::string> coverNames = {"cover", "folder", "front", "album", "art"};
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
                    if (stem == name)
                        return entry.path().string();
                }
            }
        }
    }
    catch (...)
    {
    }
    return "";
}

// ==========================================
// 4. 图片加载与通道强制 (关键修改)
// ==========================================

// 辅助：从内存加载，强制转换为4通道 (RGBA)
static StbiPtr loadBufferAsRGBA(const unsigned char *data, int size, int &w, int &h)
{
    if (!data || size <= 0)
        return nullptr;
    int c = 0;
    // req_comp = 4 强制加载为 RGBA
    return StbiPtr(stbi_load_from_memory(data, size, &w, &h, &c, 4));
}

// 辅助：从文件加载，强制转换为4通道 (RGBA)
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

// 核心逻辑：处理并缓存单曲封面
// 1. 尝试内嵌 -> 强制4通道
// 2. 失败则用 fallbackPixels (文件夹封面，已是4通道)
// 3. 存入 Cache
static void processAndCacheCover(const std::string &musicPath,
                                 const std::string &albumKey,
                                 const unsigned char *fallbackPixels, int fbW, int fbH)
{
    if (albumKey.empty())
        return;

    // 1. 提取内嵌
    TagLib::ByteVector data = extractCoverData(musicPath);

    // 2. 尝试解码内嵌为4通道
    int w = 0, h = 0;
    StbiPtr imgPixels(nullptr);

    if (!data.isEmpty())
    {
        imgPixels = loadBufferAsRGBA(reinterpret_cast<const unsigned char *>(data.data()),
                                     data.size(), w, h);
    }

    // 3. 决策：如果有内嵌且解码成功，存入；否则如果有文件夹封面，存入
    if (imgPixels)
    {
        // 内嵌优先
        CoverCache::instance().putCompressedFromPixels(albumKey, imgPixels.get(), w, h, 4);
    }
    else if (fallbackPixels && fbW > 0 && fbH > 0)
    {
        // 使用文件夹封面
        CoverCache::instance().putCompressedFromPixels(albumKey, fallbackPixels, fbW, fbH, 4);
    }
}

// 缓存文件夹自身的封面 (Key = 文件夹名)
static void cacheDirCover(const std::string &dirPath,
                          const std::string &explicitCoverPath,
                          const std::string &fallbackAudioPath)
{
    int w = 0, h = 0;
    StbiPtr imgPixels(nullptr);

    // A. 显式文件 (强制4通道)
    if (!explicitCoverPath.empty())
    {
        imgPixels = loadFileAsRGBA(explicitCoverPath, w, h);
    }

    // B. 内嵌封面 (强制4通道)
    if (!imgPixels && !fallbackAudioPath.empty())
    {
        auto coverData = extractCoverData(fallbackAudioPath);
        if (!coverData.isEmpty())
        {
            imgPixels = loadBufferAsRGBA(reinterpret_cast<const unsigned char *>(coverData.data()),
                                         coverData.size(), w, h);
        }
    }

    // C. 存入缓存
    if (imgPixels)
    {
        fs::path p(dirPath);
        std::string key = p.filename().string();
        CoverCache::instance().putCompressedFromPixels(key, imgPixels.get(), w, h, 4);
    }
}

// ==========================================
// 5. 元数据获取 (移除了缓存逻辑)
// ==========================================

static int64_t getFFmpegDuration(const std::string &filePath)
{
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

        // [修改] 此处不再尝试缓存封面，将控制权交给调用者 (scanDir等)
    }
    return musicData;
}

// ==========================================
// 6. 目录扫描与构建
// ==========================================

static std::set<std::string> processCueAndAddNodes(
    const fs::path &cuePath,
    const std::shared_ptr<PlaylistNode> &parentNode,
    const std::string &dirCoverPath,
    const unsigned char *fallbackPixels, int fbW, int fbH) // 传入文件夹封面(4通道)
{
    std::set<std::string> handledAudioFiles;
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
                handledAudioFiles.insert(pAudio.string());
                try
                {
                    handledAudioFiles.insert(fs::canonical(pAudio).string());
                }
                catch (...)
                {
                }

                auto trackNode = std::make_shared<PlaylistNode>(pAudio.string(), false);
                MetaData md = FileScanner::getMetaData(pAudio.string()); // 仅获取文本

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
                if (!dirCoverPath.empty())
                    md.setCoverPath(dirCoverPath);

                std::string albumKey = md.getAlbum().empty() ? "Unknown" : md.getAlbum();
                trackNode->setCoverKey(albumKey);

                // [修改] 调用统一逻辑：内嵌 -> 文件夹封面(fallbackPixels) -> 缓存
                processAndCacheCover(pAudio.string(), albumKey, fallbackPixels, fbW, fbH);

                trackNode->setMetaData(md);
                parentNode->addChild(trackNode);
            }
        }
    }
    catch (...)
    {
    }
    return handledAudioFiles;
}

static std::shared_ptr<PlaylistNode> buildNodeFromDir(const fs::path &dirPath)
{
    if (fs::is_symlink(dirPath))
        return nullptr;

    fs::path preferredPath = dirPath;
    preferredPath.make_preferred();
    std::string dirPathStr = preferredPath.string();
    std::string folderName = preferredPath.filename().string();

    auto node = std::make_shared<PlaylistNode>(dirPathStr, true);
    node->setCoverKey(folderName);

    std::string dirCoverPath = findDirectoryCover(preferredPath);

    // [修改] 预加载文件夹封面到内存 (强制4通道)
    int dcW = 0, dcH = 0;
    StbiPtr dcPixels = loadFileAsRGBA(dirCoverPath, dcW, dcH); // channels隐式为4

    std::set<std::string> processedFiles;

    // A. 处理 CUE
    for (const auto &entry : fs::directory_iterator(preferredPath))
    {
        if (entry.is_regular_file() && hasExtension(entry.path().string(), ".cue"))
        {
            auto files = processCueAndAddNodes(entry.path(), node, dirCoverPath, dcPixels.get(), dcW, dcH);
            processedFiles.insert(files.begin(), files.end());
        }
    }

    // B. 常规文件
    for (const auto &entry : fs::directory_iterator(preferredPath))
    {
        try
        {
            if (entry.is_directory())
            {
                auto child = buildNodeFromDir(entry.path());
                if (child)
                    node->addChild(child);
            }
            else if (entry.is_regular_file())
            {
                fs::path childPath = entry.path();
                childPath.make_preferred();
                std::string filePath = childPath.string();

                // 去重逻辑
                std::string canon = filePath;
                try
                {
                    canon = fs::canonical(childPath).string();
                }
                catch (...)
                {
                }
                if (processedFiles.count(filePath) || processedFiles.count(canon))
                    continue;

                if (isffmpeg(filePath))
                {
                    auto fileNode = std::make_shared<PlaylistNode>(filePath, false);
                    MetaData md = FileScanner::getMetaData(filePath);
                    if (!dirCoverPath.empty())
                        md.setCoverPath(dirCoverPath);

                    std::string albumKey = md.getAlbum().empty() ? "Unknown" : md.getAlbum();
                    fileNode->setCoverKey(albumKey);

                    // [修改] 处理单曲封面：内嵌 -> 文件夹封面(dcPixels) -> 缓存
                    processAndCacheCover(filePath, albumKey, dcPixels.get(), dcW, dcH);

                    fileNode->setMetaData(md);
                    node->addChild(fileNode);
                }
            }
        }
        catch (...)
        {
        }
    }

    node->sortChildren();

    // 缓存文件夹封面 (Key = Folder Name)
    std::string fallbackAudio;
    for (const auto &child : node->getChildren())
    {
        if (!child->isDir())
        {
            fallbackAudio = child->getPath();
            break;
        }
    }
    // 注意：cacheDirCover内部也会强制转4通道
    cacheDirCover(preferredPath.string(), dirCoverPath, fallbackAudio);

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

    // 单文件处理
    if (fs::is_regular_file(rootPath))
    {
        if (hasExtension(rootDir, ".cue"))
        {
            auto root = std::make_shared<PlaylistNode>(rootPath.parent_path().string(), true);
            root->setCoverKey(rootPath.parent_path().filename().string());

            std::string dirCover = findDirectoryCover(rootPath.parent_path());
            int dcW = 0, dcH = 0;
            StbiPtr dcPixels = loadFileAsRGBA(dirCover, dcW, dcH);

            processCueAndAddNodes(rootPath, root, dirCover, dcPixels.get(), dcW, dcH);
            if (!root->getChildren().empty())
            {
                root->sortChildren();
                rootNode = root;
            }
        }
        else if (isffmpeg(rootDir))
        {
            auto fileNode = std::make_shared<PlaylistNode>(rootPath.string(), false);
            MetaData md = FileScanner::getMetaData(rootPath.string());

            std::string albumKey = md.getAlbum().empty() ? "Unknown" : md.getAlbum();
            fileNode->setCoverKey(albumKey);

            std::string dirCover = findDirectoryCover(rootPath.parent_path());
            int dcW = 0, dcH = 0;
            StbiPtr dcPixels = loadFileAsRGBA(dirCover, dcW, dcH);

            // [修改] 统一逻辑
            processAndCacheCover(rootPath.string(), albumKey, dcPixels.get(), dcW, dcH);

            fileNode->setMetaData(md);
            rootNode = fileNode;
        }
        hasScanCpld = true;
        return;
    }

    // 目录处理
    auto root = std::make_shared<PlaylistNode>(rootPath.string(), true);
    root->setCoverKey(rootPath.filename().string());

    std::vector<fs::directory_entry> entries;
    try
    {
        for (const auto &entry : fs::directory_iterator(rootPath))
            entries.push_back(entry);
    }
    catch (...)
    {
    }

    if (entries.empty())
    {
        rootNode = root;
        hasScanCpld = true;
        return;
    }

    // 预处理根目录 CUE
    std::set<std::string> processed;
    std::string rootCover = findDirectoryCover(rootPath);
    int rcW = 0, rcH = 0;
    StbiPtr rootPixels = loadFileAsRGBA(rootCover, rcW, rcH);

    for (const auto &entry : entries)
    {
        if (entry.is_regular_file() && hasExtension(entry.path().string(), ".cue"))
        {
            auto h = processCueAndAddNodes(entry.path(), root, rootCover, rootPixels.get(), rcW, rcH);
            processed.insert(h.begin(), h.end());
        }
    }

    std::vector<fs::directory_entry> workEntries;
    for (const auto &entry : entries)
    {
        if (entry.is_directory())
        {
            workEntries.push_back(entry);
        }
        else if (entry.is_regular_file())
        {
            fs::path p = entry.path();
            p.make_preferred();
            if (processed.find(p.string()) == processed.end() && isffmpeg(p.string()))
            {
                workEntries.push_back(entry);
            }
        }
    }

    // 多线程扫描
    unsigned int threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0)
        threadCount = 4;
    threadCount = std::min<unsigned int>(threadCount, workEntries.size());

    if (threadCount > 0)
    {
        std::vector<std::thread> threads;
        std::vector<std::vector<std::shared_ptr<PlaylistNode>>> results(threadCount);

        auto worker = [&](unsigned int idx, unsigned int begin, unsigned int end)
        {
            for (unsigned int i = begin; i < end; ++i)
            {
                try
                {
                    const auto &entry = workEntries[i];
                    if (entry.is_directory())
                    {
                        auto node = buildNodeFromDir(entry.path());
                        if (node)
                            results[idx].push_back(node);
                    }
                    else
                    {
                        // 根目录下的单个文件
                        fs::path p = entry.path();
                        p.make_preferred();
                        auto node = std::make_shared<PlaylistNode>(p.string(), false);
                        MetaData md = getMetaData(p.string());

                        std::string albumKey = md.getAlbum().empty() ? "Unknown" : md.getAlbum();
                        node->setCoverKey(albumKey);

                        // [修改] 统一逻辑
                        processAndCacheCover(p.string(), albumKey, rootPixels.get(), rcW, rcH);

                        if (!rootCover.empty())
                            md.setCoverPath(rootCover);
                        node->setMetaData(md);
                        results[idx].push_back(node);
                    }
                }
                catch (...)
                {
                }
            }
        };

        size_t chunkSize = (workEntries.size() + threadCount - 1) / threadCount;
        for (unsigned int t = 0; t < threadCount; ++t)
        {
            size_t begin = t * chunkSize;
            size_t end = std::min(begin + chunkSize, workEntries.size());
            if (begin < end)
                threads.emplace_back(worker, t, begin, end);
        }

        for (auto &t : threads)
            if (t.joinable())
                t.join();
        for (auto &res : results)
            for (auto &ch : res)
                root->addChild(ch);
    }

    root->sortChildren();

    // 根目录封面缓存 (Key = Folder Name)
    std::string fallbackAudio;
    for (const auto &child : root->getChildren())
    {
        if (!child->isDir())
        {
            fallbackAudio = child->getPath();
            break;
        }
    }
    cacheDirCover(rootPath.string(), rootCover, fallbackAudio);

    rootNode = root;
    hasScanCpld = true;
}

// ==========================================
// 6. 辅助工具
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

std::string FileScanner::extractCoverToTempFile(const std::string &musicPath, const std::string &coverName)
{
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

    TagLib::ByteVector coverData = extractCoverData(musicPath);
    if (coverData.isEmpty())
        return "";

    std::string safeName = sanitizeFilename(coverName);
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
    return "";
}