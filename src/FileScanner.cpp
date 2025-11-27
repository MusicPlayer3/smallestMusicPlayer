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

void FileScanner::initSupportedExtensions()
{
    std::call_once(g_initFlag, []()
                   {
        std::set<std::string> audioWhitelist = {
            "mp3", "flac", "wav", "ogg", "opus", 
            "m4a", "aac", "alac", "ape", "wma", 
            "aiff", "aif", "dsf", "dff", "mp2", "mp1", "wv"
        };

        SDL_Log("[FileScanner] Initializing supported audio extensions from FFmpeg...");
        void *opaque = nullptr;
        const AVInputFormat *fmt = nullptr;
        while ((fmt = av_demuxer_iterate(&opaque))) {
            if (fmt->extensions) {
                auto exts = splitString(fmt->extensions, ',');
                for (auto &ext : exts) {
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (audioWhitelist.count(ext)) {
                        g_supportedAudioExts.insert("." + ext);
                    }
                }
            }
        } });
}

inline bool isffmpeg(const std::string &route)
{
    FileScanner::initSupportedExtensions();
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
    // 8. Ogg / Opus / Ape / WavPack (暂不支持或较少见)
    // Ogg/Opus 封面通常是 Base64 编码在 METADATA_BLOCK_PICTURE 中，TagLib 核心库不直接提供解码。
    // APE 标签支持二进制 Cover Art (Item key: "Cover Art (Front)"), 但较为少见。

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

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path path(musicPath);
    MetaData musicData;

    // isffmpeg 保证了后缀是音频且文件有效
    if (fs::exists(path) && isffmpeg(musicPath))
    {
        // 1. 获取基本元数据
        // FileRef 是智能的，它会根据扩展名自动选择解析器，所以这里不会有性能问题
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

            // 2. 提取内嵌封面 (这里调用了优化后的函数)
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

    rootNode = buildNodeFromDir(rootPath);
    hasScanCpld = true;
}

// ==========================================
// 5. 封面缓存文件生成工具 (新增功能)
// ==========================================

// 辅助：清洗文件名，移除非法字符
static std::string sanitizeFilename(const std::string &name)
{
    std::string safeName = name;
    const std::string illegalChars = "\\/:?\"<>|*";
    for (char &c : safeName)
    {
        if (illegalChars.find(c) != std::string::npos || c < 32)
        {
            c = '_'; // 将非法字符替换为下划线
        }
    }
    // 如果名字为空或全是空格，给个默认名
    if (safeName.empty() || std::all_of(safeName.begin(), safeName.end(), [](unsigned char c)
                                        { return std::isspace(c); }))
    {
        return "Unknown_Album";
    }
    return safeName;
}

// 辅助：通过二进制魔数检测图片后缀
static std::string detectImageExtension(const TagLib::ByteVector &data)
{
    if (data.size() >= 3 && data[0] == (char)0xFF && data[1] == (char)0xD8 && data[2] == (char)0xFF)
    {
        return ".jpg";
    }
    if (data.size() >= 8 && data[0] == (char)0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
    {
        return ".png";
    }
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M')
    {
        return ".bmp";
    }
    if (data.size() >= 4 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
    {
        return ".gif";
    }
    return ".jpg"; // 默认回退到 jpg
}

// [新增实现] 提取封面到临时文件
std::string FileScanner::extractCoverToTempFile(const std::string &musicPath)
{
    fs::path tmpDir = "./tmp";

    // 1. 确保 tmp 目录存在
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
        return "";
    }

    // 2. 为了获取专辑名，我们需要简单解析一下 Tag
    // 注意：这里为了获取专辑名作为文件名，必须读一次 Tag。
    // 如果调用者已经有了 MetaData，最好传 MetaData 进来以避免这次重复 IO。
    // 但按照要求，参数仅为 musicPath。
    std::string albumName = "Unknown_Album";
    {
        // 使用 TagLib::FileRef 是获取通用 Tag 最简单的方法
        TagLib::FileRef f(musicPath.c_str());
        if (!f.isNull() && f.tag())
        {
            std::string tempAlbum = f.tag()->album().toCString(true);
            if (!tempAlbum.empty())
            {
                albumName = tempAlbum;
            }
        }
    }

    // 3. 清洗文件名
    std::string safeAlbumName = sanitizeFilename(albumName);

    // 4. 调用现有的 extractCoverData 获取二进制数据
    // (复用之前优化过的 extractCoverData 函数)
    TagLib::ByteVector coverData = extractCoverData(musicPath);

    if (coverData.isEmpty())
    {
        return ""; // 无封面
    }

    // 5. 确定后缀名
    std::string ext = detectImageExtension(coverData);

    // 6. 构建目标路径
    fs::path targetPath = tmpDir / (safeAlbumName + ext);

    // 7. 检查文件是否已存在 (避免重复写入同一张专辑封面)
    if (fs::exists(targetPath) && fs::file_size(targetPath) > 0)
    {
        // 甚至可以进一步比较文件大小，但通常同名专辑封面是一样的
        try
        {
            return fs::absolute(targetPath).string();
        }
        catch (...)
        {
            return targetPath.string();
        }
    }

    // 8. 写入文件
    std::ofstream outFile(targetPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (outFile.is_open())
    {
        outFile.write(coverData.data(), coverData.size());
        outFile.close();

        try
        {
            return fs::absolute(targetPath).string();
        }
        catch (...)
        {
            return targetPath.string();
        }
    }
    else
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write cover file: %s", targetPath.string().c_str());
    }

    return "";
}