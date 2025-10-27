#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm> // for std::transform
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tbytevector.h>
#include <taglib/tstring.h>
#include <taglib/tlist.h> // for List<T>
// MP3
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>             // for ID3v2::Tag
#include <taglib/id3v2frame.h>           // for ID3v2::Frame
#include <taglib/attachedpictureframe.h> // for AttachedPictureFrame
// FLAC
#include <taglib/flacfile.h>    // for FLAC::File
#include <taglib/flacpicture.h> // for FLAC::Picture
// MP4
#include <taglib/mp4file.h>     // for MP4::File
#include <taglib/mp4tag.h>      // for MP4::Tag and ItemMap
#include <taglib/mp4coverart.h> // for CoverArtList

namespace fs = std::filesystem;

const std::vector<std::string> AUDIO_EXTS = {".mp3", ".flac", ".ogg", ".oga", ".m4a", ".mp4"};

class Metadata
{
public:
    std::string file;
    std::string title;
    std::string artist;
    std::string album;
    std::string cover;

    Metadata() :
        title("未知"), artist("未知"), album("未知"), cover("无")
    {
    }

    void print() const
    {
        std::cout << "文件: " << file << std::endl;
        std::cout << "  歌名: " << title << std::endl;
        std::cout << "  歌手: " << artist << std::endl;
        std::cout << "  专辑: " << album << std::endl;
        std::cout << "  封面: " << cover << std::endl
                  << std::endl;
    }

    bool has_cover() const
    {
        return cover != "无";
    }
};

class MusicLibrary
{
private:
    std::vector<Metadata> items;

    Metadata extract_metadata(const std::string &file_path)
    {
        Metadata meta;
        meta.file = file_path;

        std::string ext = file_path.substr(file_path.find_last_of("."));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (std::find(AUDIO_EXTS.begin(), AUDIO_EXTS.end(), ext) == AUDIO_EXTS.end())
        {
            return meta;
        }

        TagLib::FileRef f(file_path.c_str());
        if (f.isNull() || !f.tag())
        {
            return meta;
        }

        TagLib::Tag *tag = f.tag();
        meta.title = tag->title().to8Bit(true).data();
        meta.artist = tag->artist().to8Bit(true).data();
        meta.album = tag->album().to8Bit(true).data();

        // 提取封面
        TagLib::ByteVector cover_data;
        if (ext == ".mp3")
        {
            TagLib::MPEG::File *mp3file = dynamic_cast<TagLib::MPEG::File *>(f.file());
            if (mp3file)
            {
                TagLib::ID3v2::Tag *id3tag = mp3file->ID3v2Tag(true);
                if (id3tag)
                {
                    TagLib::ID3v2::FrameList frames = id3tag->frameList("APIC");
                    if (!frames.isEmpty())
                    {
                        TagLib::ID3v2::AttachedPictureFrame *apic =
                            static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front());
                        cover_data = apic->picture();
                    }
                }
            }
        }
        else if (ext == ".flac")
        {
            TagLib::FLAC::File *flacfile = dynamic_cast<TagLib::FLAC::File *>(f.file());
            if (flacfile)
            {
                TagLib::List<TagLib::FLAC::Picture *> pics = flacfile->pictureList();
                if (!pics.isEmpty())
                {
                    cover_data = pics.front()->data();
                }
            }
        }
        else if (ext == ".ogg" || ext == ".oga")
        {
            // OGG 通常无嵌入图片，跳过
        }
        else if (ext == ".m4a" || ext == ".mp4")
        {
            TagLib::MP4::File *mp4file = dynamic_cast<TagLib::MP4::File *>(f.file());
            if (mp4file)
            {
                TagLib::MP4::Tag *mtag = mp4file->tag();
                if (mtag)
                {
                    const TagLib::MP4::ItemMap &items = mtag->itemMap(); // 修复: ItemMap 而非 ItemListMap
                    auto it = items.find("covr");
                    if (it != items.end())
                    {
                        const TagLib::MP4::CoverArtList &covers = it->second.toCoverArtList();
                        if (!covers.isEmpty())
                        {
                            cover_data = covers.front().data();
                        }
                    }
                }
            }
        }

        if (!cover_data.isEmpty())
        {
            fs::create_directory("covers");
            std::string safe_name = fs::path(file_path).stem().string() + ".jpg";
            std::string cover_path = "covers/" + safe_name;
            std::ofstream cover_file(cover_path, std::ios::binary);
            if (cover_file)
            {
                cover_file.write(cover_data.data(), cover_data.size());
                meta.cover = cover_path;
            }
        }

        return meta;
    }

public:
    void scan(const std::string &folder_path)
    {
        items.clear();
        try
        {
            for (const auto &entry : fs::recursive_directory_iterator(folder_path))
            {
                if (entry.is_regular_file())
                {
                    std::string path = entry.path().string();
                    Metadata meta = extract_metadata(path);
                    if (meta.title != "未知" || meta.artist != "未知" || meta.album != "未知" || meta.cover != "无")
                    {
                        items.push_back(meta);
                    }
                }
            }
        }
        catch (const fs::filesystem_error &e)
        {
            std::cerr << "文件系统错误: " << e.what() << std::endl;
        }
    }

    void print_all() const
    {
        for (const auto &item : items)
        {
            item.print();
        }
    }

    void print_summary() const
    {
        std::cout << "总歌曲数: " << items.size() << std::endl;
        int with_cover = 0;
        for (const auto &item : items)
        {
            if (item.has_cover())
                ++with_cover;
        }
        std::cout << "有封面的歌曲: " << with_cover << std::endl;
    }

    size_t size() const
    {
        return items.size();
    }

    const Metadata &get(size_t index) const
    {
        if (index < items.size())
            return items[index];
        static Metadata empty;
        return empty;
    }
};

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        std::cerr << "用法: ./extract_music_metadata /path/to/music/folder" << std::endl;
        return 1;
    }

    MusicLibrary library;
    std::string folder_path = argv[1];
    library.scan(folder_path);
    library.print_all();
    library.print_summary();

    return 0;
}