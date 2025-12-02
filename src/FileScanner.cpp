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

// 确保包含 filesystem
#include <filesystem>
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

static std::string findRealAudioFile(const fs::path &dirPath, const std::string &cueFileName)
{
    fs::path target = dirPath / cueFileName;

    // 1. 直接存在，完美
    if (fs::exists(target))
        return target.string();

    // 2. 尝试替换后缀名
    // CUE 中常见的后缀是 .wav，但实际可能是 flac, ape, wv, tak, tta
    static const std::vector<std::string> fallbackExts = {
        ".flac", ".ape", ".wv", ".wav", ".m4a", ".tak", ".tta", ".mp3"};

    fs::path stem = target.parent_path() / target.stem(); // 去掉后缀的路径

    for (const auto &ext : fallbackExts)
    {
        fs::path tryPath = stem;
        tryPath.replace_extension(ext);
        if (fs::exists(tryPath))
        {
            return tryPath.string();
        }
    }

    return ""; // 真的找不到
}

static std::vector<CueTrackInfo> parseCueFile(const fs::path &cuePath)
{
    std::vector<CueTrackInfo> tracks;
    // C++17 filesystem::path 支持直接用于 ifstream 构造
    // 在 Windows 上它会自动处理宽字符路径
    std::ifstream file(cuePath);
    if (!file.is_open())
        return tracks;

    std::string line, globalPerformer, globalTitle, currentFile;
    CueTrackInfo currentTrack;
    bool inTrack = false;

    // 正则优化：兼容不同格式的空白字符
    std::regex regFile("FILE\\s+\"(.*)\"");
    std::regex regTrack("TRACK\\s+(\\d+)\\s+AUDIO");
    std::regex regTitle("TITLE\\s+\"(.*)\"");
    std::regex regPerformer("PERFORMER\\s+\"(.*)\"");
    std::regex regIndex("INDEX\\s+01\\s+(\\d{2}:\\d{2}:\\d{2})");
    std::smatch match;

    while (std::getline(file, line))
    {
        // 去除首尾空白
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (std::regex_search(line, match, regFile))
        {
            currentFile = match[1].str();
        }
        else if (std::regex_search(line, match, regTrack))
        {
            // 保存上一首 Track
            if (inTrack)
            {
                if (currentTrack.performer.empty())
                    currentTrack.performer = globalPerformer;
                tracks.push_back(currentTrack);
            }

            // 开始新 Track
            inTrack = true;
            currentTrack = CueTrackInfo();
            currentTrack.trackNum = std::stoi(match[1].str());
            // [修正] Track 属于当前上下文的 File
            currentTrack.audioFile = currentFile;
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
    // 保存最后一首
    if (inTrack)
    {
        if (currentTrack.performer.empty())
            currentTrack.performer = globalPerformer;
        tracks.push_back(currentTrack);
    }

    // 计算时长
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (i < tracks.size() - 1)
        {
            // 只有当两首歌属于同一个文件时，才能通过时间差计算时长
            if (tracks[i].audioFile == tracks[i + 1].audioFile)
                tracks[i].duration = tracks[i + 1].startTime - tracks[i].startTime;
            else
                tracks[i].duration = 0; // 如果换文件了，时长通常由文件自身决定，或者需要读取文件长度
        }
        else
        {
            tracks[i].duration = 0; // 最后一首，稍后通过文件总时长计算
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
    std::string ext = getLowerExtension(musicPath);

    // [关键修改] 使用 fs::path 并转为 c_str()
    // 在 Windows 下 c_str() 返回 const wchar_t*，在 POSIX 下返回 const char*
    // TagLib 提供了宽字符的构造函数重载，能正确打开包含 Unicode 的路径
    fs::path p(musicPath);
    p.make_preferred(); // 确保分隔符正确

    // 1. MP3 / MP2 / MP1 (ID3v2)
    if (ext == ".mp3" || ext == ".mp2" || ext == ".mp1")
    {
        TagLib::MPEG::File file(p.c_str(), false); // false = 不读音频属性，只读 Tag
        if (file.isValid() && file.ID3v2Tag())
        {
            data = extractID3v2Cover(file.ID3v2Tag());
        }
    }
    // 2. FLAC (FLAC Picture Block)
    else if (ext == ".flac")
    {
        TagLib::FLAC::File file(p.c_str(), false);
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
        TagLib::MP4::File file(p.c_str(), false);
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
        TagLib::ASF::File file(p.c_str(), false);
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
        TagLib::RIFF::WAV::File file(p.c_str(), false);
        if (file.isValid() && file.ID3v2Tag())
        {
            data = extractID3v2Cover(file.ID3v2Tag());
        }
    }
    // 6. AIFF / AIF (ID3v2 Chunk)
    else if (ext == ".aiff" || ext == ".aif")
    {
        TagLib::RIFF::AIFF::File file(p.c_str(), false);
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
    //     TagLib::DSF::File file(p.c_str(), false);
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
    // 注意：stbi_load 默认使用 const char*，在 Windows 处理中文路径可能需要 stbi_load_from_memory + ifstream(wide)
    // 或者 stbi_load 内部如果有宽字符支持。
    // 这里简单处理：如果 explicitCoverPath 存在，用 fs::path 转 wchar 读入内存再 load
    if (!explicitCoverPath.empty())
    {
        fs::path p(explicitCoverPath);
        p.make_preferred();
        if (fs::exists(p))
        {
            // stbi_load 在 Windows 上直接用 char* 路径可能有编码问题
            // 为了保险，我们用 ifstream 读取文件流，再从内存加载
            std::ifstream file(p, std::ios::binary | std::ios::ate);
            if (file.good())
            {
                std::streamsize size = file.tellg();
                file.seekg(0, std::ios::beg);
                std::vector<char> buffer(size);
                if (file.read(buffer.data(), size))
                {
                    imgPixels = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(buffer.data()), size, &width, &height, &channels, 4);
                }
            }
        }
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
    // [关键修改] 使用 fs::path 构造，自动处理分隔符
    fs::path p(musicPath);
    p.make_preferred();
    static int cnt = 0;
    cnt++;
    MetaData musicData;

    // isffmpeg 保证了后缀是音频且文件有效
    if (fs::exists(p) && isffmpeg(musicPath))
    {
        // 1. 获取基本元数据
        // [关键修改] 使用 p.c_str() 传递宽字符路径给 TagLib
        auto dir = p.c_str();
        TagLib::FileRef f(dir);

        if (!f.isNull() && f.tag())
        {
            TagLib::Tag *tag = f.tag();
            // 注意：musicPath 依然保存为 string 用于 QML 显示，但读取过程用宽字符
            musicData.setFilePath(p.string());
            musicData.setParentDir(p.parent_path().string());
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
            // SDL_Log("[getMetaData] %s", musicData.getFilePath().c_str());
            // SDL_Log("title: %s", musicData.getTitle().c_str());
            // SDL_Log("artist: %s", musicData.getArtist().c_str());
            // SDL_Log("album: %s", musicData.getAlbum().c_str());
            // SDL_Log("year: %s", musicData.getYear().c_str());
            // SDL_Log("duration: %d", musicData.getDuration());
            // SDL_Log("offset: %d", musicData.getOffset());
            // SDL_Log("now cnt:%d", cnt);
        }
    }
    return musicData;
}

// [新增] 封装 CUE 处理逻辑，供递归和根目录扫描复用
// 返回值：成功解析并关联的音频文件路径集合
static std::set<std::string> processCueAndAddNodes(
    const fs::path &cuePath,
    const std::shared_ptr<PlaylistNode> &parentNode,
    const std::string &dirCoverPath)
{
    std::set<std::string> handledAudioFiles;
    try
    {
        fs::path dirPath = cuePath.parent_path();
        auto tracks = parseCueFile(cuePath);

        for (auto &track : tracks)
        {
            // 使用之前定义的模糊查找函数寻找真实音频
            std::string realAudioPath = findRealAudioFile(dirPath, track.audioFile);

            if (!realAudioPath.empty() && isffmpeg(realAudioPath))
            {
                // 记录已处理的音频文件
                handledAudioFiles.insert(realAudioPath);
                try
                {
                    // 统一使用 preferred 格式存入 set，避免重复
                    fs::path p(realAudioPath);
                    p.make_preferred();
                    handledAudioFiles.insert(p.string());
                    handledAudioFiles.insert(fs::canonical(p).string());
                }
                catch (...)
                {
                }

                // 构造节点时也确保路径格式正确
                fs::path pAudio(realAudioPath);
                pAudio.make_preferred();

                auto trackNode = std::make_shared<PlaylistNode>(pAudio.string(), false);
                MetaData md = FileScanner::getMetaData(pAudio.string());

                if (!track.title.empty())
                    md.setTitle(track.title);
                if (!track.performer.empty())
                    md.setArtist(track.performer);

                md.setOffset(track.startTime); // 设置起始时间

                if (track.duration > 0)
                {
                    md.setDuration(track.duration);
                }
                else
                {
                    // 最后一轨时长计算
                    int64_t remaining = md.getDuration() - track.startTime;
                    if (remaining > 0)
                        md.setDuration(remaining);
                }

                if (!dirCoverPath.empty())
                    md.setCoverPath(dirCoverPath);

                trackNode->setMetaData(md);
                parentNode->addChild(trackNode); // 使用之前修复后的 addChild
            }
        }
    }
    catch (...)
    {
    }

    return handledAudioFiles;
}

// ==========================================
// 4. 目录构建
// ==========================================
static std::shared_ptr<PlaylistNode> buildNodeFromDir(const fs::path &dirPath)
{
    // 防止软链接死循环
    if (fs::is_symlink(dirPath))
        return nullptr;

    // [关键修改] 创建副本并转换为系统首选格式 (Windows下转为反斜杠)
    fs::path preferredPath = dirPath;
    preferredPath.make_preferred();
    std::string dirPathStr = preferredPath.string();

    auto node = std::make_shared<PlaylistNode>(dirPathStr, true);
    node->setCoverKey(dirPathStr);

    std::vector<std::shared_ptr<PlaylistNode>> tmpChildren;

    // 为了暂时复用 node->addChild 逻辑，我们先构造好 children 再排序，
    // 但因为 processCueAndAddNodes 直接操作 parentNode，我们需要稍微调整策略：
    // 这里我们直接传 node 进去，最后再统一对 node->children 排序。

    std::string dirCoverPath = findDirectoryCover(preferredPath);
    std::set<std::string> processedFiles;

    // A. 处理 CUE (使用新封装的函数)
    for (const auto &entry : fs::directory_iterator(preferredPath))
    {
        if (entry.is_regular_file() && hasExtension(entry.path().string(), ".cue"))
        {
            std::set<std::string> audioFiles = processCueAndAddNodes(entry.path(), node, dirCoverPath);
            processedFiles.insert(audioFiles.begin(), audioFiles.end());
        }
    }

    // B. 处理普通文件和子目录
    for (const auto &entry : fs::directory_iterator(preferredPath))
    {
        try
        {
            if (entry.is_directory())
            {
                // 递归调用
                auto childDirNode = buildNodeFromDir(entry.path());
                if (childDirNode)
                {
                    node->addChild(childDirNode);
                }
            }
            else if (entry.is_regular_file())
            {
                // [关键修改] 对子文件也进行路径格式化
                fs::path childPath = entry.path();
                childPath.make_preferred();
                std::string filePath = childPath.string();

                // 检查是否已被 CUE 处理过
                std::string canonicalPath = filePath;
                try
                {
                    canonicalPath = fs::canonical(childPath).string();
                }
                catch (...)
                {
                }

                if (processedFiles.count(filePath) || processedFiles.count(canonicalPath))
                {
                    continue;
                }

                if (isffmpeg(filePath))
                {
                    auto fileNode = std::make_shared<PlaylistNode>(filePath, false);
                    MetaData md = FileScanner::getMetaData(filePath);
                    if (!dirCoverPath.empty())
                        md.setCoverPath(dirCoverPath);
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
    // 处理文件夹封面 fallback
    std::string fallbackAudio;
    for (const auto &child : node->getChildren())
    {
        if (!child->isDir())
        {
            fallbackAudio = child->getPath();
            break;
        }
    }
    cacheDirCover(preferredPath.string(), dirCoverPath, fallbackAudio);

    return node;
}

void FileScanner::scanDir()
{
    FileScanner::initSupportedExtensions();

    // [关键修改] 入口处统一 RootPath 格式，后续所有子路径都会继承这个格式
    fs::path rootPath(rootDir);
    rootPath.make_preferred();

    if (!fs::exists(rootPath))
    {
        hasScanCpld = true;
        return;
    }

    // ==========================================
    // Case 1: Root 是单文件
    // ==========================================
    if (fs::is_regular_file(rootPath))
    {
        // [修复] 如果拖入的是 .cue 文件
        if (hasExtension(rootDir, ".cue"))
        {
            // 创建一个虚拟的根节点，或者用 CUE 所在目录做根
            // 这里为了界面显示，我们创建一个包含 CUE 内容的根节点
            auto root = std::make_shared<PlaylistNode>(rootPath.parent_path().string(), true);
            root->setCoverKey(rootPath.parent_path().string());

            std::string dirCover = findDirectoryCover(rootPath.parent_path());

            // 解析 CUE
            processCueAndAddNodes(rootPath, root, dirCover);

            if (!root->getChildren().empty())
            {
                root->sortChildren(); // 记得加排序
                rootNode = root;
            }
            else
            {
                rootNode = nullptr; // 解析失败
            }
        }
        // 普通音频文件
        else if (isffmpeg(rootDir))
        {
            auto fileNode = std::make_shared<PlaylistNode>(rootPath.string(), false);
            MetaData md = FileScanner::getMetaData(rootPath.string());
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

    // ==========================================
    // Case 2: Root 是目录
    // ==========================================
    auto root = std::make_shared<PlaylistNode>(rootPath.string(), true);
    root->setCoverKey(rootPath.string());

    // 1. 获取所有条目
    std::vector<fs::directory_entry> entries;
    try
    {
        // rootPath 已经是 preferred (Windows下全是反斜杠)，遍历出来的 entry 也会是统一的
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

    // 2. [关键修复] 先在主线程处理根目录下的 CUE 文件
    std::set<std::string> rootProcessedFiles;
    std::string rootDirCover = findDirectoryCover(rootPath);

    for (const auto &entry : entries)
    {
        if (entry.is_regular_file() && hasExtension(entry.path().string(), ".cue"))
        {
            // 解析 CUE 并直接挂载到 root 下
            std::set<std::string> handled = processCueAndAddNodes(entry.path(), root, rootDirCover);
            rootProcessedFiles.insert(handled.begin(), handled.end());
        }
    }

    // 3. 准备并发任务：过滤掉已被 CUE 处理的文件
    std::vector<fs::directory_entry> workEntries;
    for (const auto &entry : entries)
    {
        if (entry.is_directory())
        {
            workEntries.push_back(entry);
        }
        else if (entry.is_regular_file())
        {
            // 在检查是否已处理时，统一格式，确保字符串匹配准确
            fs::path p = entry.path();
            p.make_preferred();
            std::string path = p.string();

            std::string canon = path;
            try
            {
                canon = fs::canonical(entry.path()).string();
            }
            catch (...)
            {
            }

            // 如果不在黑名单里，才添加进待处理列表
            if (rootProcessedFiles.find(path) == rootProcessedFiles.end() && rootProcessedFiles.find(canon) == rootProcessedFiles.end())
            {
                workEntries.push_back(entry);
            }
        }
    }

    // 4. 并行扫描剩余内容 (子目录 + 未处理的单曲)
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    unsigned int threadCount = std::min<unsigned int>(hw, workEntries.size());

    if (threadCount > 0)
    {
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
                const auto &entry = workEntries[i];
                try
                {
                    if (entry.is_directory())
                    {
                        auto childDirNode = buildNodeFromDir(entry.path());
                        if (childDirNode)
                        {
                            local.children.push_back(childDirNode);
                        }
                    }
                    else if (entry.is_regular_file())
                    {
                        // 统一格式
                        fs::path p = entry.path();
                        p.make_preferred();
                        std::string filePath = p.string();

                        if (isffmpeg(filePath))
                        {
                            auto fileNode = std::make_shared<PlaylistNode>(filePath, false);
                            MetaData md = FileScanner::getMetaData(filePath);
                            if (!rootDirCover.empty())
                                md.setCoverPath(rootDirCover);
                            fileNode->setMetaData(md);
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

        unsigned int chunkSize = (workEntries.size() + threadCount - 1) / threadCount;
        for (unsigned int t = 0; t < threadCount; ++t)
        {
            unsigned int begin = t * chunkSize;
            unsigned int end = std::min<unsigned int>(begin + chunkSize, workEntries.size());
            if (begin < end)
                threads.emplace_back(worker, t, begin, end);
        }

        for (auto &th : threads)
            if (th.joinable())
                th.join();

        // 将并行结果合并到 root (此时 root 已经包含了 CUE 的分轨节点)
        for (auto &r : results)
        {
            for (auto &ch : r.children)
            {
                root->addChild(ch);
            }
        }
    }

    // 5. 最终排序
    root->sortChildren();

    // 6. 处理根目录封面缓存
    std::string fallbackAudio;
    for (const auto &child : root->getChildren())
    {
        if (!child->isDir())
        {
            fallbackAudio = child->getPath();
            break;
        }
    }
    cacheDirCover(rootPath.string(), rootDirCover, fallbackAudio);

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

    // 使用 fs::path 构造 ofstream，Windows 下自动处理宽字符路径
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