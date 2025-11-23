#include "FileScanner.hpp"
#include "MetaData.hpp"
#include "AudioPlayer.hpp"
#include "Precompiled.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;
inline bool isffmpeg(const std::string &route)
{
    return AudioPlayer::isValidAudio(route);
}

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
        musicData.setDuration(f.audioProperties()->lengthInMilliseconds() * 1000);

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
            }

            SDL_Log("[Info] Image loaded. Width: %d, Height: %d, Channels: %d\n", width, height, channels);

            std::string coverPath = outputDir + "/" + std::string(tag->title().toCString(true)) + ".png";

            int success = stbi_write_png(coverPath.c_str(), width, height, channels, imgPixels, 0);
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

void getinfo(const std::string &route, std::vector<MetaData> &items) // 读取并存储该路径下的音频文件信息
{
    fs::path r(route);
    MetaData music;
    TagLib::FileRef f(route.c_str());

    if (f.isNull() || f.tag() == nullptr)
    {
        return;
    }
    TagLib::Tag *tag = f.tag();
    music.setFilePath(route);
    music.setParentDir(r.parent_path().string());
    music.setTitle(tag->title().toCString(true));
    music.setArtist(tag->artist().toCString(true));
    music.setAlbum(tag->album().toCString(true));
    music.setYear(tag->year() > 0 ? std::to_string(tag->year()) : "");
    items.push_back(music);
}
void FileScanner::scanDir()
{ // 扫描路径并获取路径下所有音频文件信息
    if (!fs::exists(rootDir))
    {
        hasScanCpld = true;
        return;
    }
    if (!(fs::is_directory(rootDir)))
    {
        if (isffmpeg(rootDir))
        {
            getinfo(rootDir, items);
        }
        hasScanCpld = true;
        return;
    }

    std::stack<std::string> dirStack;
    dirStack.push(rootDir);
    while (!dirStack.empty()) // dfs层次遍历文件夹或读入音频文件信息
    {
        std::string currentDir = dirStack.top();
        dirStack.pop();
        for (const auto &entry : fs::directory_iterator(currentDir))
        {
            if (entry.is_directory())
            {
                dirStack.push(entry.path().string());
            }
            else if (isffmpeg(entry.path().string()))
            {
                getinfo(entry.path().string(), items);
            }
        }
    }
    hasScanCpld = true;
    return;
}