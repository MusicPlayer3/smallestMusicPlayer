#include "FileScanner.hpp"
#include "Cover.hpp"
#include "MetaData.hpp"
#include "AudioPlayer.hpp"
#include "Precompiled.h"
#include <filesystem>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;
inline bool isffmpeg(const std::string &route)
{
    #ifndef FILESCANNER_TEST
        // 正式版本：用 AudioPlayer 做准确判断
        return AudioPlayer::isValidAudio(route);
    #else
        // 测试版本：简单按后缀名判断
        std::string lower = route;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c)
                    { return std::tolower(c); });

        return lower.ends_with(".mp3")
            || lower.ends_with(".flac")
            || lower.ends_with(".wav")
            || lower.ends_with(".ogg");
    #endif
}
// 从音频文件中提取封面二进制数据
static TagLib::ByteVector extractCoverData(const char *fileName)
{
    TagLib::ByteVector data;

    // 1. 尝试作为 MPEG (MP3) 文件处理 (ID3v2)
    TagLib::MPEG::File mpegFile(fileName);
    if (mpegFile.isValid() && mpegFile.ID3v2Tag())
    {
        TagLib::ID3v2::Tag *tag = mpegFile.ID3v2Tag();
        // 查找 APIC (Attached Picture) 帧
        TagLib::ID3v2::FrameList frames = tag->frameList("APIC");
        if (!frames.isEmpty())
        {
            // 通常取第一个图片
            TagLib::ID3v2::AttachedPictureFrame *frame =
                static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front());
            return frame->picture();
        }
    }

    // 2. 尝试作为 FLAC 文件处理
    TagLib::FLAC::File flacFile(fileName);
    if (flacFile.isValid())
    {
        const TagLib::List<TagLib::FLAC::Picture *> &pictures = flacFile.pictureList();
        if (!pictures.isEmpty())
        {
            return pictures[0]->data();
        }
    }

    // TODO: 你可以在这里添加 MP4/M4A 或其他格式的支持逻辑

    return data; // 返回空数据
}

MetaData FileScanner::getMetaData(const std::string &musicPath)
{
    fs::path path(musicPath);
    MetaData musicData;
    if (fs::is_regular_file(path) && isffmpeg(musicPath))
    {
        TagLib::FileRef f(musicPath.c_str());
        if (f.isNull() || f.tag() == nullptr)
        {
            return musicData;
        }
        TagLib::Tag *tag = f.tag();
        musicData.setFilePath(musicPath);
        musicData.setParentDir(path.parent_path().string());
        musicData.setTitle(tag->title().toCString(true));
        musicData.setArtist(tag->artist().toCString(true));
        musicData.setAlbum(tag->album().toCString(true));
        musicData.setYear(tag->year() > 0 ? std::to_string(tag->year()) : "");
        musicData.setDuration(f.audioProperties()->lengthInMilliseconds());

        // 提取封面到tmp目录下
        TagLib::ByteVector coverData = extractCoverData(musicPath.c_str());
        if (coverData.isEmpty())
        {
            musicData.setCoverPath("");
        }
        else
        {
            std::string outputDir = std::filesystem::current_path().string() + "/tmp";
            if (!fs::exists(outputDir))
            {
                fs::create_directory(outputDir);
            }
            SDL_Log("[Info] Found cover art. Size: %d bytes.\n", coverData.size());

            int width, height, channels;
            unsigned char *imgPixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char *>(coverData.data()),
                coverData.size(),
                &width,
                &height,
                &channels,
                0 // 强制通道数，0 表示保持原样
            );

            if (!imgPixels)
            {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to load image from memory: %s\n", stbi_failure_reason());
                musicData.setCoverPath("");
                return musicData;
            }

            SDL_Log("[Info] Image loaded. Width: %d, Height: %d, Channels: %d\n", width, height, channels);

            std::string coverPath = outputDir + "/" + std::string(tag->title().toCString(true)) + ".png";

            int success = stbi_write_png(coverPath.c_str(), width, height, channels, imgPixels, 0);
            
            CoverCache::instance().putCompressedFromPixels(musicData.getAlbum(), imgPixels, width, height, channels);

            stbi_image_free(imgPixels);

            if (success)
            {
                musicData.setCoverPath(coverPath);
            }
            else
            {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write PNG image: %s\n", stbi_failure_reason());
            }
        }
    }
    return musicData;
}

// 递归构建某个目录对应的 PlaylistNode
static std::shared_ptr<PlaylistNode>
buildNodeFromDir(const fs::path &dirPath)
{
    auto node = std::make_shared<PlaylistNode>(dirPath.string(), /*isDir=*/true);

    std::vector<std::shared_ptr<PlaylistNode>> tmpChildren;

    for (const auto &entry : fs::directory_iterator(dirPath))
    {
        try
        {
            if (entry.is_directory())
            {
                auto childDirNode = buildNodeFromDir(entry.path());
                tmpChildren.push_back(childDirNode);
            }
            else if (entry.is_regular_file())
            {
                std::string filePath = entry.path().string();
                if (isffmpeg(filePath))
                {
                    auto fileNode = std::make_shared<PlaylistNode>(filePath, /*isDir=*/false);

                    // 用 FileScanner::getMetaData 获取元数据
                    MetaData md = FileScanner::getMetaData(filePath);
                    fileNode->setMetaData(md);

                    tmpChildren.push_back(fileNode);
                }
            }
        }
        catch (const std::exception &e)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Error while scanning %s: %s\n",
                         entry.path().string().c_str(),
                         e.what());
        }
    }

    // 排序：目录优先，然后按路径字符串排序（可选）
    std::sort(tmpChildren.begin(), tmpChildren.end(),
              [](const std::shared_ptr<PlaylistNode> &a,
                 const std::shared_ptr<PlaylistNode> &b)
              {
                  if (a->getIsDir() != b->getIsDir())
                      return a->getIsDir() && !b->getIsDir(); // 目录在前
                  return a->getPath() < b->getPath();
              });

    for (auto &child : tmpChildren)
        node->addChild(child);

    return node;
}

void FileScanner::scanDir() // 扫描路径并获取路径下所有音频文件信息
{ 
    fs::path rootPath(rootDir);

    if (!fs::exists(rootPath))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Root path does not exist: %s\n", rootDir.c_str());
        hasScanCpld = true;
        return;
    }

    if(fs::is_regular_file(rootPath))
    {
        if (isffmpeg(rootDir))
        {
            auto fileNode = std::make_shared<PlaylistNode>(rootDir, /*isDir=*/false);
            MetaData md = FileScanner::getMetaData(rootDir);
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

    // 根是目录：递归构建整棵树
    rootNode = buildNodeFromDir(rootPath);
    //建树完成后，对rootNode遍历可以按照目录结构遍历所有文件

    hasScanCpld = true; // 扫描完成
}