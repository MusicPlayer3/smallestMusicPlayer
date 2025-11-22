#include <iostream>
#include <vector>
#include <filesystem> // C++17 标准库
#include <string>

// TagLib 头文件
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>

// STB 图像库定义 (必须在 include 前定义 IMPLEMENTATION)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

/**
 * @brief 尝试从文件中提取封面数据的辅助函数
 * @param fileName 音乐文件路径
 * @return TagLib::ByteVector 包含图片的原始二进制数据 (若无则为空)
 */
TagLib::ByteVector extractCoverData(const char *fileName)
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

int main(int argc, char *argv[])
{
    // 1. 参数校验
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <path_to_music_file>" << std::endl;
        return 1;
    }

    std::string filePath = argv[1];
    std::string outputDir = "./temp";
    std::string outputFilename = outputDir + "/cover.png";

    // 2. 准备 Temp 目录
    try
    {
        if (!fs::exists(outputDir))
        {
            if (fs::create_directory(outputDir))
            {
                std::cout << "[Info] Created directory: " << outputDir << std::endl;
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Error] Failed to create directory: " << e.what() << std::endl;
        return 1;
    }

    // 3. 提取封面原始数据 (TagLib)
    std::cout << "[Info] Processing file: " << filePath << std::endl;
    TagLib::ByteVector coverData = extractCoverData(filePath.c_str());

    if (coverData.isEmpty())
    {
        std::cerr << "[Warning] No cover art found in this file." << std::endl;
        return 0;
    }

    std::cout << "[Info] Found cover art. Size: " << coverData.size() << " bytes." << std::endl;

    // 4. 图像转换与保存 (stb_image)
    // 4.1 从内存解码图像 (不管是 JPG, PNG 还是 BMP)
    int width, height, channels;
    unsigned char *imgPixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char *>(coverData.data()),
        coverData.size(),
        &width,
        &height,
        &channels,
        0 // 强制通道数，0 表示保持原样
    );

    if (imgPixels == nullptr)
    {
        std::cerr << "[Error] Failed to decode image data (stbi_load)." << std::endl;
        return 1;
    }

    std::cout << "[Info] Image decoded. Resolution: " << width << "x" << height
              << ", Channels: " << channels << std::endl;

    // 4.2 写入为 PNG 文件
    // stbi_write_png 最后一个参数是 stride_in_bytes，0 表示让它自动计算
    int success = stbi_write_png(outputFilename.c_str(), width, height, channels, imgPixels, 0);

    // 4.3 释放内存
    stbi_image_free(imgPixels);

    if (success)
    {
        std::cout << "[Success] Cover saved to: " << outputFilename << std::endl;
    }
    else
    {
        std::cerr << "[Error] Failed to write PNG file." << std::endl;
        return 1;
    }

    return 0;
}