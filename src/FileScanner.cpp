#include "FileScanner.hpp"
#include "CoverCache.hpp"
#include "MetaData.hpp"
#include "AudioPlayer.hpp"
#include "Precompiled.h"
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

// 辅助：检查文件后缀
inline bool hasExtension(const std::string &filename, const std::string &ext)
{
    if (filename.length() < ext.length())
        return false;
    std::string fileExt = filename.substr(filename.length() - ext.length());
    std::string targetExt = ext;
    // 简单转小写比较
    std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(), ::tolower);
    std::transform(targetExt.begin(), targetExt.end(), targetExt.begin(), ::tolower);
    return fileExt == targetExt;
}

inline bool isffmpeg(const std::string &route)
{
#ifndef FILESCANNER_TEST
    return AudioPlayer::isValidAudio(route);
#else
    std::string lower = route;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });
    return lower.ends_with(".mp3") || lower.ends_with(".flac") || lower.ends_with(".wav") || lower.ends_with(".ogg") || lower.ends_with(".ape");
#endif
}

// ================= CUE 解析相关 =================

// 将 mm:ss:ff (frames) 转换为微秒
// 1 frame = 1/75 second
static int64_t parseCueTime(const std::string &timeStr)
{
    int m = 0, s = 0, f = 0;
    char d1, d2; // delimiters
    std::stringstream ss(timeStr);
    ss >> m >> d1 >> s >> d2 >> f;

    // Total seconds
    double totalSeconds = m * 60.0 + s + (f / 75.0);
    return static_cast<int64_t>(totalSeconds * 1000000);
}

struct CueTrackInfo
{
    int trackNum = 0;
    std::string title;
    std::string performer;
    int64_t startTime = 0; // 微秒
    int64_t duration = 0;  // 微秒
    std::string audioFile; // 关联的音频文件（相对路径或绝对路径）
};

// 解析 .cue 文件
static std::vector<CueTrackInfo> parseCueFile(const fs::path &cuePath)
{
    std::vector<CueTrackInfo> tracks;
    std::ifstream file(cuePath);
    if (!file.is_open())
        return tracks;

    std::string line;
    std::string globalPerformer;
    std::string globalTitle;
    std::string currentFile;

    CueTrackInfo currentTrack;
    bool inTrack = false;

    // 正则表达式辅助
    std::regex regFile("FILE \"(.*)\"");
    std::regex regTrack("TRACK (\\d+) AUDIO");
    std::regex regTitle("TITLE \"(.*)\"");
    std::regex regPerformer("PERFORMER \"(.*)\"");
    std::regex regIndex("INDEX 01 (\\d{2}:\\d{2}:\\d{2})");

    std::smatch match;

    while (std::getline(file, line))
    {
        // 简单的去除首尾空格
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
                // 保存上一个 Track
                currentTrack.audioFile = currentFile;
                // 如果没有特定 performer，使用 global
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
                globalTitle = match[1].str(); // 专辑名通常可以忽略，或者存入 MetaData album
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
            {
                currentTrack.startTime = parseCueTime(match[1].str());
            }
        }
    }
    // 添加最后一个 Track
    if (inTrack)
    {
        currentTrack.audioFile = currentFile;
        if (currentTrack.performer.empty())
            currentTrack.performer = globalPerformer;
        tracks.push_back(currentTrack);
    }

    // 计算时长 (Track N Duration = Track N+1 Start - Track N Start)
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (i < tracks.size() - 1)
        {
            // 如果两个音轨属于同一个文件，可以计算时长
            if (tracks[i].audioFile == tracks[i + 1].audioFile)
            {
                tracks[i].duration = tracks[i + 1].startTime - tracks[i].startTime;
            }
            else
            {
                tracks[i].duration = 0; // 无法计算，或者读取文件总长
            }
        }
        else
        {
            // 最后一轨，需要获取对应音频文件的总长度来减去 start
            // 这里我们暂时设为0，或者在构建 MetaData 时读取文件获取
            tracks[i].duration = 0;
        }
    }

    return tracks;
}

// ================= 现有功能 =================

static TagLib::ByteVector extractCoverData(const char *fileName)
{
    TagLib::ByteVector data;
    // 1. MP3 (ID3v2)
    {
        TagLib::MPEG::File mpegFile(fileName);
        if (mpegFile.isValid() && mpegFile.ID3v2Tag())
        {
            TagLib::ID3v2::Tag *tag = mpegFile.ID3v2Tag();
            TagLib::ID3v2::FrameList frames = tag->frameList("APIC");
            if (!frames.isEmpty())
            {
                TagLib::ID3v2::AttachedPictureFrame *frame =
                    static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front());
                return frame->picture();
            }
        }
    }
    // 2. FLAC
    {
        TagLib::FLAC::File flacFile(fileName);
        if (flacFile.isValid())
        {
            const TagLib::List<TagLib::FLAC::Picture *> &pictures = flacFile.pictureList();
            if (!pictures.isEmpty())
                return pictures[0]->data();
        }
    }
    return data;
}

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path path(musicPath);
    MetaData musicData;
    // 如果文件存在且是有效音频
    if (fs::exists(path) && isffmpeg(musicPath))
    {
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

            // 获取总时长
            if (f.audioProperties())
            {
                musicData.setDuration(f.audioProperties()->lengthInMilliseconds() * 1000ll);
            }

            // 处理嵌入封面
            TagLib::ByteVector coverData = extractCoverData(musicPath.c_str());
            if (!coverData.isEmpty())
            {
                // ... (原有 stbi_load 逻辑)
                int width, height, channels;
                unsigned char *imgPixels = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char *>(coverData.data()),
                    coverData.size(), &width, &height, &channels, STBI_rgb_alpha);

                if (imgPixels)
                {
                    // 使用 Album 作为 key 缓存
                    std::string key = musicData.getAlbum().empty() ? "Unknown" : musicData.getAlbum();
                    CoverCache::instance().putCompressedFromPixels(key, imgPixels, width, height, channels);
                    stbi_image_free(imgPixels);
                }
            }
        }
    }
    return musicData;
}

// 辅助：在目录下寻找封面文件
static std::string findDirectoryCover(const fs::path &dirPath)
{
    const std::vector<std::string> coverNames = {"cover", "folder", "front", "album", "art"};
    const std::vector<std::string> exts = {".jpg", ".jpeg", ".png", ".bmp"};

    // 优先匹配标准名称
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
                    {
                        if (lowerExt == e)
                            return entry.path().string();
                    }
                }
            }
        }
    }

    // 如果没有找到标准名称，尝试寻找任意图片 (可选，视需求而定，这里暂不开启)
    return "";
}

// 递归构建某个目录对应的 PlaylistNode
static std::shared_ptr<PlaylistNode> buildNodeFromDir(const fs::path &dirPath)
{
    auto node = std::make_shared<PlaylistNode>(dirPath.string(), /*isDir=*/true);
    std::vector<std::shared_ptr<PlaylistNode>> tmpChildren;

    // 1. 扫描当前目录是否有独立封面
    std::string dirCoverPath = findDirectoryCover(dirPath);

    // 记录已经被 CUE 处理过的音频文件，避免重复添加
    std::set<std::string> processedFiles;

    // 2. 优先处理 .cue 文件
    for (const auto &entry : fs::directory_iterator(dirPath))
    {
        if (entry.is_regular_file() && hasExtension(entry.path().string(), ".cue"))
        {
            auto tracks = parseCueFile(entry.path());
            for (auto &track : tracks)
            {
                // 构建音频文件的完整路径
                fs::path audioPath = dirPath / track.audioFile;
                std::string audioPathStr = audioPath.string();

                if (fs::exists(audioPath) && isffmpeg(audioPathStr))
                {
                    // 标记该音频文件已被处理
                    processedFiles.insert(audioPathStr);
                    // 尝试规范化路径以确保匹配准确
                    try
                    {
                        processedFiles.insert(fs::canonical(audioPath).string());
                    }
                    catch (std::exception &e)
                    {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to canonicalize path: %s", e.what());
                    }

                    // 创建节点
                    auto trackNode = std::make_shared<PlaylistNode>(audioPathStr, /*isDir=*/false);

                    // 获取基础 MetaData（从音频文件中读取）
                    // 这样可以获得专辑名、年份、甚至嵌入的图片作为备选
                    MetaData md = FileScanner::getMetaData(audioPathStr);

                    // 用 CUE 信息覆盖
                    if (!track.title.empty())
                        md.setTitle(track.title);
                    if (!track.performer.empty())
                        md.setArtist(track.performer);
                    md.setOffset(track.startTime);

                    // 时长处理
                    if (track.duration > 0)
                    {
                        md.setDuration(track.duration);
                    }
                    else
                    {
                        // 最后一轨，用总时长 - 偏移量
                        int64_t remaining = md.getDuration() - track.startTime;
                        if (remaining > 0)
                            md.setDuration(remaining);
                    }

                    // 封面处理：如果目录有封面，强制覆盖
                    if (!dirCoverPath.empty())
                    {
                        md.setCoverPath(dirCoverPath);
                    }

                    trackNode->setMetaData(md);
                    trackNode->setParent(node);
                    tmpChildren.push_back(trackNode);
                }
            }
        }
    }

    // 3. 处理剩余的普通文件和子目录
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
                std::string canonicalPath = filePath;
                try
                {
                    canonicalPath = fs::canonical(entry.path()).string();
                }
                catch (...)
                {
                }

                // 只有当文件是音频且不在 processedFiles 中时才处理
                if (isffmpeg(filePath) && processedFiles.find(filePath) == processedFiles.end() && processedFiles.find(canonicalPath) == processedFiles.end())
                {
                    auto fileNode = std::make_shared<PlaylistNode>(filePath, /*isDir=*/false);
                    MetaData md = FileScanner::getMetaData(filePath);

                    // 封面处理：如果目录有封面，覆盖
                    if (!dirCoverPath.empty())
                    {
                        md.setCoverPath(dirCoverPath);
                    }

                    fileNode->setMetaData(md);
                    fileNode->setParent(node);
                    tmpChildren.push_back(fileNode);
                }
            }
        }
        catch (const std::exception &e)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Error scanning %s: %s\n", entry.path().string().c_str(), e.what());
        }
    }

    // 排序
    std::sort(tmpChildren.begin(), tmpChildren.end(),
              [](const std::shared_ptr<PlaylistNode> &a,
                 const std::shared_ptr<PlaylistNode> &b)
              {
                  if (a->isDir() != b->isDir())
                      return a->isDir() && !b->isDir();
                  // 如果都是文件，尝试按 Track 号或 Offset 排序逻辑比较复杂，这里暂按 Title/Path
                  // 也可以比较 MetaData 的 Offset
                  // 如果是 CUE 生成的，属于同一文件的可以按 Offset 排序
                  return a->getPath() < b->getPath();
              });

    for (auto &child : tmpChildren)
        node->addChild(child);

    return node;
}

void FileScanner::scanDir()
{
    fs::path rootPath(rootDir);

    if (!fs::exists(rootPath))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Root path not found: %s\n", rootDir.c_str());
        hasScanCpld = true;
        return;
    }

    if (fs::is_regular_file(rootPath))
    {
        // 单文件模式，暂不深入支持单文件的 CUE 关联（通常 CUE 和音频在同一目录）
        if (isffmpeg(rootDir))
        {
            auto fileNode = std::make_shared<PlaylistNode>(rootDir, /*isDir=*/false);
            MetaData md = FileScanner::getMetaData(rootDir);
            // 尝试找同级封面
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

    rootNode = buildNodeFromDir(rootPath);
    hasScanCpld = true;
}