#include "FileScanner.hpp"
#include "CoverCache.hpp"
#include "MetaData.hpp"
#include "AudioPlayer.hpp"
#include "Precompiled.h"

// ================= TagLib Headers =================
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
// MP4 / M4A / AAC
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
// WMA / ASF
#include <taglib/asffile.h>
#include <taglib/asftag.h>
#include <taglib/asfpicture.h>
// WAV / AIFF
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
// DSF (DSD) - 需要 TagLib 1.9+
#include <taglib/dsffile.h>

namespace fs = std::filesystem;

// ==========================================
// 1. 后缀名支持与校验 (FFmpeg)
// ==========================================

static std::set<std::string> g_supportedAudioExts;
static std::once_flag g_initFlag;

static std::vector<std::string> splitString(const std::string &s, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}

// 定义一个极全的音频后缀列表（包含常见、发烧、游戏、古老格式）
static const std::vector<std::string> kKnownAudioExtensions = {
    // 常见有损
    "mp3", "aac", "m4a", "ogg", "wma", "opus", "mpc", "mp+", "mpp",
    // 无损格式
    "flac", "ape", "wav", "aiff", "aif", "wv", "tta", "alac", "shn", "tak",
    // DSD / 高解析度
    "dsf", "dff", "dxd",
    // 容器格式（通常用于纯音频的）
    "mka", "webm", // 注意：webm 和 mka 也可能含视频，但常用于音频
    // 模组音乐 (Tracker Music - MOD/XM/IT) - FFmpeg 通常支持
    "mod", "it", "s3m", "xm", "mtm", "umx", "mdz", "s3z", "itgz", "xmz",
    // 游戏音频 / Chiptune
    "vgm", "vgz", "spc", "psf", "psf2", "minipsf", "usf", "miniusf", "ssdl",
    "adx", "hca", "brstm", "bcstm", "bfstm",
    // 老旧或特定格式
    "au", "snd", "voc", "ra", "rm", "amr", "awb", "gsm", "act", "3g2", "3gp", "caf", "qcp",
    // DTS / AC3 / 影院音频
    "dts", "dtshd", "ac3", "eac3", "mlp", "truehd"};

void FileScanner::initSupportedExtensions()
{
    std::call_once(g_initFlag, []()
                   {
                       SDL_Log("[FileScanner] Detecting supported audio extensions via FFmpeg...");

                       // 确保 ffmpeg 网络/设备已注册（新版 ffmpeg 不需要，但为了兼容旧版）
                       // av_register_all();

                       for (const auto &ext : kKnownAudioExtensions)
                       {
                           // 核心逻辑：av_guess_format
                           // 问 FFmpeg：如果有一个文件名叫 test.xxx，你有对应的 Demuxer 吗？
                           const AVInputFormat *fmt = av_find_input_format(ext.c_str());

                           if (fmt)
                           {
                               // 找到了对应的 Demuxer，说明当前链接的 FFmpeg 库支持这种格式
                               g_supportedAudioExts.insert("." + ext);
                           }
                       } });
}

inline bool isffmpeg(const std::string &route)
{
    fs::path p(route);
    if (!p.has_extension())
        return false;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (g_supportedAudioExts.find(ext) == g_supportedAudioExts.end())
        return false;

#ifndef FILESCANNER_TEST
    return AudioPlayer::isValidAudio(route);
#else
    return true;
#endif
}

inline bool hasExtension(const std::string &filename, const std::string &ext)
{
    if (filename.length() < ext.length())
        return false;
    std::string fileExt = filename.substr(filename.length() - ext.length());
    std::string targetExt = ext;
    std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(), ::tolower);
    std::transform(targetExt.begin(), targetExt.end(), targetExt.begin(), ::tolower);
    return fileExt == targetExt;
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

// ==========================================
// 2. CUE 解析
// ==========================================

static int64_t parseCueTime(const std::string &timeStr)
{
    int m = 0, s = 0, f = 0;
    char d1, d2;
    std::stringstream ss(timeStr);
    ss >> m >> d1 >> s >> d2 >> f;
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

static std::vector<CueTrackInfo> parseCueFile(const fs::path &cuePath)
{
    std::vector<CueTrackInfo> tracks;
    std::ifstream file(cuePath);
    if (!file.is_open())
        return tracks;

    std::string line, globalPerformer, globalTitle, currentFile;
    CueTrackInfo currentTrack;
    bool inTrack = false;

    std::regex regFile("FILE \"(.*)\"");
    std::regex regTrack("TRACK (\\d+) AUDIO");
    std::regex regTitle("TITLE \"(.*)\"");
    std::regex regPerformer("PERFORMER \"(.*)\"");
    std::regex regIndex("INDEX 01 (\\d{2}:\\d{2}:\\d{2})");
    std::smatch match;

    while (std::getline(file, line))
    {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (std::regex_search(line, match, regFile))
        {
            currentFile = match[1].str();
        }
        else if (std::regex_search(line, match, regTrack))
        {
            if (inTrack)
            {
                currentTrack.audioFile = currentFile;
                if (currentTrack.performer.empty())
                    currentTrack.performer = globalPerformer;
                tracks.push_back(currentTrack);
            }
            inTrack = true;
            currentTrack = CueTrackInfo();
            currentTrack.trackNum = std::stoi(match[1].str());
        }
        else if (std::regex_search(line, match, regTitle))
        {
            if (inTrack)
                currentTrack.title = match[1].str();
            else
                globalTitle = match[1].str();
        }
        else if (std::regex_search(line, match, regPerformer))
        {
            if (inTrack)
                currentTrack.performer = match[1].str();
            else
                globalPerformer = match[1].str();
        }
        else if (std::regex_search(line, match, regIndex))
        {
            if (inTrack)
                currentTrack.startTime = parseCueTime(match[1].str());
        }
    }
    if (inTrack)
    {
        currentTrack.audioFile = currentFile;
        if (currentTrack.performer.empty())
            currentTrack.performer = globalPerformer;
        tracks.push_back(currentTrack);
    }

    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (i < tracks.size() - 1)
        {
            if (tracks[i].audioFile == tracks[i + 1].audioFile)
                tracks[i].duration = tracks[i + 1].startTime - tracks[i].startTime;
            else
                tracks[i].duration = 0;
        }
        else
        {
            tracks[i].duration = 0;
        }
    }
    return tracks;
}

// ==========================================
// 3. 封面提取 (修复了性能问题的版本)
// ==========================================

// 辅助函数：从 ID3v2 标签中提取图片 (共用于 MP3, WAV, AIFF, DSF)
static TagLib::ByteVector extractID3v2Cover(TagLib::ID3v2::Tag *tag)
{
    TagLib::ByteVector data;
    if (tag)
    {
        TagLib::ID3v2::FrameList frames = tag->frameList("APIC");
        if (!frames.isEmpty())
        {
            // 通常取第一张图片
            auto *frame = static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front());
            data = frame->picture();
        }
    }
    return data;
}

static TagLib::ByteVector extractCoverData(const std::string &musicPath)
{
    TagLib::ByteVector data;
    std::string ext = getLowerExtension(musicPath); // 使用之前定义的 helper

    // 1. MP3 / MP2 / MP1 (ID3v2)
    if (ext == ".mp3" || ext == ".mp2" || ext == ".mp1")
    {
        TagLib::MPEG::File file(musicPath.c_str(), false); // false = 不读音频属性，只读 Tag
        if (file.isValid() && file.ID3v2Tag())
        {
            data = extractID3v2Cover(file.ID3v2Tag());
        }
    }
    // 2. FLAC (FLAC Picture Block)
    else if (ext == ".flac")
    {
        TagLib::FLAC::File file(musicPath.c_str(), false);
        if (file.isValid())
        {
            const auto &pictures = file.pictureList();
            if (!pictures.isEmpty())
            {
                data = pictures[0]->data();
            }
        }
    }
    // 3. MP4 / M4A / AAC / ALAC (MP4 Atoms)
    else if (ext == ".m4a" || ext == ".mp4" || ext == ".aac" || ext == ".alac" || ext == ".m4b")
    {
        TagLib::MP4::File file(musicPath.c_str(), false);
        if (file.isValid() && file.tag())
        {
            TagLib::MP4::Tag *tag = file.tag();
            // MP4 封面存储在 'covr' 原子中
            if (tag->itemMap().contains("covr"))
            {
                TagLib::MP4::Item coverItem = tag->itemMap()["covr"];
                TagLib::MP4::CoverArtList coverArtList = coverItem.toCoverArtList();
                if (!coverArtList.isEmpty())
                {
                    data = coverArtList.front().data();
                }
            }
        }
    }
    // 4. WMA / ASF (Windows Media Attributes)
    else if (ext == ".wma" || ext == ".asf")
    {
        TagLib::ASF::File file(musicPath.c_str(), false);
        if (file.isValid() && file.tag())
        {
            // WMA 封面存储在 "WM/Picture" 属性中
            const auto &attrMap = file.tag()->attributeListMap();
            if (attrMap.contains("WM/Picture"))
            {
                const auto &attrList = attrMap["WM/Picture"];
                if (!attrList.isEmpty())
                {
                    // WMA 图片需要转换
                    TagLib::ASF::Picture picture = attrList.front().toPicture();
                    data = picture.picture();
                }
            }
        }
    }
    // 5. WAV (ID3v2 Chunk)
    else if (ext == ".wav")
    {
        // TagLib 处理 WAV 时，readProperties=false 非常重要，否则会扫描整个大文件
        TagLib::RIFF::WAV::File file(musicPath.c_str(), false);
        if (file.isValid() && file.ID3v2Tag())
        {
            data = extractID3v2Cover(file.ID3v2Tag());
        }
    }
    // 6. AIFF / AIF (ID3v2 Chunk)
    else if (ext == ".aiff" || ext == ".aif")
    {
        TagLib::RIFF::AIFF::File file(musicPath.c_str(), false);
        if (file.isValid() && file.tag())
        {
            // AIFF 可能有 ID3v2 标签
            if (file.hasID3v2Tag())
            {
                data = extractID3v2Cover(file.tag());
            }
        }
    }
    // 7. DSF (DSD Stream File) - ID3v2
    // else if (ext == ".dsf")
    // {
    //     TagLib::DSF::File file(musicPath.c_str(), false);
    //     if (file.isValid() && file.hasID3v2Tag())
    //     {
    //         data = extractID3v2Cover(file.tag());
    //     }
    // }

    return data;
}

static std::string findDirectoryCover(const fs::path &dirPath)
{
    const std::vector<std::string> coverNames = {"cover", "folder", "front", "album", "art"};
    const std::vector<std::string> exts = {".jpg", ".jpeg", ".png", ".bmp"};
    for (const auto &entry : fs::directory_iterator(dirPath))
    {
        if (entry.is_regular_file())
        {
            std::string stem = entry.path().stem().string();
            std::string ext = entry.path().extension().string();
            std::string lowerStem = stem;
            std::string lowerExt = ext;
            std::transform(lowerStem.begin(), lowerStem.end(), lowerStem.begin(), ::tolower);
            std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);

            for (const auto &name : coverNames)
            {
                if (lowerStem == name)
                {
                    for (const auto &e : exts)
                        if (lowerExt == e)
                            return entry.path().string();
                }
            }
        }
    }
    return "";
}

// [新增] 专门用于处理目录封面的缓存逻辑
// 1. 如果有 explicitCoverPath (cover.jpg等)，则加载并以 dirPath 为 key 存入缓存
// 2. 如果没有，则尝试从 fallbackAudioPath (第一首子歌曲) 提取内嵌封面，同样以 dirPath 为 key 存入
static void cacheDirCover(const std::string &dirPath, const std::string &explicitCoverPath, const std::string &fallbackAudioPath)
{
    int width, height, channels;
    unsigned char *imgPixels = nullptr;

    // A. 尝试加载显式的封面文件 (如 cover.jpg)
    if (!explicitCoverPath.empty() && fs::exists(explicitCoverPath))
    {
        // 使用 stbi_load 加载文件
        imgPixels = stbi_load(explicitCoverPath.c_str(), &width, &height, &channels, 4);
    }

    // B. 如果没有显式封面，且有备选音频文件，尝试提取内嵌封面
    if (!imgPixels && !fallbackAudioPath.empty())
    {
        TagLib::ByteVector coverData = extractCoverData(fallbackAudioPath);
        if (!coverData.isEmpty())
        {
            imgPixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char *>(coverData.data()),
                coverData.size(), &width, &height, &channels, 4);
        }
    }

    // C. 如果成功获取到了像素数据，存入 CoverCache，key = 文件夹路径
    if (imgPixels)
    {
        CoverCache::instance().putCompressedFromPixels(dirPath, imgPixels, width, height, 4);
        stbi_image_free(imgPixels);
    }
}

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path path(musicPath);
    MetaData musicData;

    // isffmpeg 保证了后缀是音频且文件有效
    if (fs::exists(path) && isffmpeg(musicPath))
    {
        // 1. 获取基本元数据
        TagLib::FileRef f(musicPath.c_str());

        if (!f.isNull() && f.tag())
        {
            TagLib::Tag *tag = f.tag();
            musicData.setFilePath(musicPath);
            musicData.setParentDir(path.parent_path().string());
            musicData.setTitle(tag->title().toCString(true));
            musicData.setArtist(tag->artist().toCString(true));
            musicData.setAlbum(tag->album().toCString(true));
            musicData.setYear(tag->year() > 0 ? std::to_string(tag->year()) : "");

            if (f.audioProperties())
            {
                musicData.setDuration(f.audioProperties()->lengthInMilliseconds() * 1000ll);
            }

            // 2. 提取内嵌封面
            TagLib::ByteVector coverData = extractCoverData(musicPath);

            if (!coverData.isEmpty())
            {
                int width, height, channels;
                unsigned char *imgPixels = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char *>(coverData.data()),
                    coverData.size(), &width, &height, &channels, STBI_rgb_alpha);

                if (imgPixels)
                {
                    std::string key = musicData.getAlbum().empty() ? "Unknown" : musicData.getAlbum();
                    CoverCache::instance().putCompressedFromPixels(key, imgPixels, width, height, 4);
                    stbi_image_free(imgPixels);
                }
            }
        }
    }
    return musicData;
}

// ==========================================
// 4. 目录构建
// ==========================================

static std::shared_ptr<PlaylistNode> buildNodeFromDir(const fs::path &dirPath)
{
    auto node = std::make_shared<PlaylistNode>(dirPath.string(), true);
    // 设置默认的 Cover Key (即文件夹路径)，后续 cacheDirCover 会尝试填充这个 Key 对应的内容
    node->setCoverKey(dirPath.string());

    std::vector<std::shared_ptr<PlaylistNode>> tmpChildren;

    std::string dirCoverPath = findDirectoryCover(dirPath);
    std::set<std::string> processedFiles;

    // A. 处理 CUE
    try
    {
        for (const auto &entry : fs::directory_iterator(dirPath))
        {
            if (entry.is_regular_file() && hasExtension(entry.path().string(), ".cue"))
            {
                auto tracks = parseCueFile(entry.path());
                for (auto &track : tracks)
                {
                    fs::path audioPath = dirPath / track.audioFile;
                    std::string audioPathStr = audioPath.string();

                    if (fs::exists(audioPath) && isffmpeg(audioPathStr))
                    {
                        processedFiles.insert(audioPathStr);
                        try
                        {
                            processedFiles.insert(fs::canonical(audioPath).string());
                        }
                        catch (...)
                        {
                        }

                        auto trackNode = std::make_shared<PlaylistNode>(audioPathStr, false);
                        MetaData md = FileScanner::getMetaData(audioPathStr);

                        if (!track.title.empty())
                            md.setTitle(track.title);
                        if (!track.performer.empty())
                            md.setArtist(track.performer);
                        md.setOffset(track.startTime);
                        if (track.duration > 0)
                            md.setDuration(track.duration);
                        else
                        {
                            int64_t remaining = md.getDuration() - track.startTime;
                            if (remaining > 0)
                                md.setDuration(remaining);
                        }
                        if (!dirCoverPath.empty())
                            md.setCoverPath(dirCoverPath);

                        trackNode->setMetaData(md);
                        trackNode->setParent(node);
                        tmpChildren.push_back(trackNode);
                    }
                }
            }
        }
    }
    catch (...)
    {
    }

    // B. 处理普通文件
    for (const auto &entry : fs::directory_iterator(dirPath))
    {
        try
        {
            if (entry.is_directory())
            {
                auto childDirNode = buildNodeFromDir(entry.path());
                childDirNode->setParent(node);
                tmpChildren.push_back(childDirNode);
            }
            else if (entry.is_regular_file())
            {
                std::string filePath = entry.path().string();
                if (isffmpeg(filePath))
                {
                    std::string canonicalPath = filePath;
                    try
                    {
                        canonicalPath = fs::canonical(entry.path()).string();
                    }
                    catch (...)
                    {
                    }

                    if (processedFiles.find(filePath) == processedFiles.end() && processedFiles.find(canonicalPath) == processedFiles.end())
                    {
                        auto fileNode = std::make_shared<PlaylistNode>(filePath, false);
                        MetaData md = FileScanner::getMetaData(filePath);
                        if (!dirCoverPath.empty())
                            md.setCoverPath(dirCoverPath);
                        fileNode->setMetaData(md);
                        fileNode->setParent(node);
                        tmpChildren.push_back(fileNode);
                    }
                }
            }
        }
        catch (...)
        {
        }
    }

    std::sort(tmpChildren.begin(), tmpChildren.end(), [](auto &a, auto &b)
              {
        if (a->isDir() != b->isDir()) return a->isDir() && !b->isDir();
        return a->getPath() < b->getPath(); });

    for (auto &child : tmpChildren)
        node->addChild(child);

    // [新增] 处理文件夹封面
    // 寻找第一首音频文件作为 fallback
    std::string fallbackAudio;
    for (const auto &child : tmpChildren)
    {
        if (!child->isDir()) // 找到第一个文件节点
        {
            fallbackAudio = child->getPath();
            break;
        }
    }
    // 执行缓存逻辑：优先用 dirCoverPath，没有则提取 fallbackAudio 的封面，存入 Key=dirPath
    cacheDirCover(dirPath.filename().string(), dirCoverPath, fallbackAudio);

    return node;
}

void FileScanner::scanDir()
{
    FileScanner::initSupportedExtensions();
    fs::path rootPath(rootDir);

    if (!fs::exists(rootPath))
    {
        hasScanCpld = true;
        return;
    }

    // 情况1：rootDir 就是一个单独文件
    if (fs::is_regular_file(rootPath))
    {
        if (isffmpeg(rootDir))
        {
            auto fileNode = std::make_shared<PlaylistNode>(rootDir, false);
            MetaData md = FileScanner::getMetaData(rootDir);
            std::string dirCover = findDirectoryCover(rootPath.parent_path());
            if (!dirCover.empty())
                md.setCoverPath(dirCover);
            fileNode->setMetaData(md);
            rootNode = fileNode;
        }
        else
        {
            rootNode = nullptr;
        }
        hasScanCpld = true;
        return;
    }

    // 情况2：rootDir 是目录，启用“根目录并行扫描”

    // 给整个库建一个根节点
    auto root = std::make_shared<PlaylistNode>(rootPath.string(), true);
    // 设置根节点的封面Key
    root->setCoverKey(rootPath.string());

    // 为了并行，我们先把根目录下的所有 entry 收集起来
    std::vector<fs::directory_entry> entries;
    try
    {
        for (const auto &entry : fs::directory_iterator(rootPath))
        {
            entries.push_back(entry);
        }
    }
    catch (...)
    {
    }

    // 没有子项，直接结束
    if (entries.empty())
    {
        rootNode = root;
        hasScanCpld = true;
        return;
    }

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    unsigned int threadCount = std::min<unsigned int>(hw, entries.size());

    struct WorkerResult
    {
        std::vector<std::shared_ptr<PlaylistNode>> children;
    };

    std::vector<std::thread> threads;
    std::vector<WorkerResult> results(threadCount);

    auto worker = [&](unsigned int idx, unsigned int begin, unsigned int end)
    {
        WorkerResult local;
        for (unsigned int i = begin; i < end; ++i)
        {
            const auto &entry = entries[i];
            try
            {
                if (entry.is_directory())
                {
                    auto childDirNode = buildNodeFromDir(entry.path());
                    if (childDirNode)
                    {
                        childDirNode->setParent(root);
                        local.children.push_back(childDirNode);
                    }
                }
                else if (entry.is_regular_file())
                {
                    std::string filePath = entry.path().string();
                    if (isffmpeg(filePath))
                    {
                        auto fileNode = std::make_shared<PlaylistNode>(filePath, false);
                        MetaData md = FileScanner::getMetaData(filePath);
                        std::string dirCover = findDirectoryCover(entry.path().parent_path());
                        if (!dirCover.empty())
                            md.setCoverPath(dirCover);

                        fileNode->setMetaData(md);
                        fileNode->setParent(root);
                        local.children.push_back(fileNode);
                    }
                }
            }
            catch (...)
            {
            }
        }
        results[idx] = std::move(local);
    };

    unsigned int chunkSize = (entries.size() + threadCount - 1) / threadCount;
    for (unsigned int t = 0; t < threadCount; ++t)
    {
        unsigned int begin = t * chunkSize;
        unsigned int end = std::min<unsigned int>(begin + chunkSize, entries.size());
        if (begin >= end)
            break;
        threads.emplace_back(worker, t, begin, end);
    }

    for (auto &th : threads)
    {
        if (th.joinable())
            th.join();
    }

    std::vector<std::shared_ptr<PlaylistNode>> tmpChildren;
    for (auto &r : results)
    {
        for (auto &ch : r.children)
        {
            if (ch)
                tmpChildren.push_back(ch);
        }
    }

    std::sort(tmpChildren.begin(), tmpChildren.end(), [](auto &a, auto &b)
              {
                  if (a->isDir() != b->isDir())
                      return a->isDir() && !b->isDir();
                  return a->getPath() < b->getPath(); });

    for (auto &child : tmpChildren)
    {
        root->addChild(child);
    }

    // [新增] 处理根目录的封面（并行扫描结束后）
    std::string rootCoverPath = findDirectoryCover(rootPath);
    std::string fallbackAudio;
    for (const auto &child : tmpChildren)
    {
        if (!child->isDir())
        {
            fallbackAudio = child->getPath();
            break;
        }
    }
    cacheDirCover(rootPath.filename().string(), rootCoverPath, fallbackAudio);

    rootNode = root;
    hasScanCpld = true;
}

// ==========================================
// 5. 封面缓存文件生成工具 (新增功能)
// ==========================================

#if defined(_WIN32) || defined(_WIN64)
#define OS_WINDOWS
#endif

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

static std::string detectImageExtension(const TagLib::ByteVector &data)
{
    if (data.size() >= 3 && data[0] == (char)0xFF && data[1] == (char)0xD8 && data[2] == (char)0xFF)
        return ".jpg";
    if (data.size() >= 8 && data[0] == (char)0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return ".png";
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M')
        return ".bmp";
    if (data.size() >= 4 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        return ".gif";
    return ".jpg";
}

void FileScanner::extractCoverToTempFile(const std::string &musicPath, MetaData &data)
{
    fs::path tmpDir = std::filesystem::temp_directory_path() / "SmallestMusicPlayer";

    try
    {
        if (!fs::exists(tmpDir))
        {
            fs::create_directories(tmpDir);
        }
    }
    catch (const std::exception &e)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tmp dir: %s", e.what());
        return;
    }

    std::string safeAlbumName = sanitizeFilename(data.getAlbum());
    TagLib::ByteVector coverData = extractCoverData(musicPath);

    if (coverData.isEmpty())
        return;

    std::string ext = detectImageExtension(coverData);
    fs::path targetPath = tmpDir / (safeAlbumName + ext);

    if (fs::exists(targetPath) && fs::file_size(targetPath) > 0)
    {
        try
        {
            data.setCoverPath(fs::absolute(targetPath).string());
        }
        catch (std::exception &e)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to set album path: %s\nerror:%s", targetPath.string().c_str(), e.what());
            data.setCoverPath(targetPath.string());
        }
    }

    std::ofstream outFile(targetPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (outFile.is_open())
    {
        outFile.write(coverData.data(), coverData.size());
        outFile.close();

        try
        {
            data.setCoverPath(fs::absolute(targetPath).string());
        }
        catch (std::exception &e)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to set album path: %s\nerror:%s", targetPath.string().c_str(), e.what());
            data.setCoverPath(targetPath.string());
        }
    }
    else
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write cover file: %s", targetPath.string().c_str());
    }

    return;
}