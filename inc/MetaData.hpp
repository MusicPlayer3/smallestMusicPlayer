#ifndef _METADATA_HPP_
#define _METADATA_HPP_

#include "Precompiled.h"

class MetaData
{
private:
    std::string title;     // 歌曲标题
    std::string artist;    // 艺术家
    std::string album;     // 专辑
    std::string year;      // 出版年份
    std::string filePath;  // 文件路径
    std::string parentDir; // 上级目录
    std::string coverPath; // 封面路径
    int64_t duration;      // 歌曲时长(单位微秒)
    int64_t offset;        // [新增] 起始播放偏移量(单位微秒)，默认为0

public:
    MetaData() : duration(0), offset(0)
    {
    } // 初始化

    MetaData(const std::string &title,
             const std::string &artist,
             const std::string &album,
             const std::string &year,
             const std::string &filePath,
             const std::string &parentDir,
             const std::string &coverPath,
             int64_t duration,
             int64_t offset = 0) : // 新增参数
        title(title),
        artist(artist),
        album(album),
        year(year),
        filePath(filePath),
        parentDir(parentDir),
        coverPath(coverPath),
        duration(duration),
        offset(offset)
    {
    }

    MetaData(const MetaData &other) = default;
    MetaData(MetaData &&other) noexcept = default;
    ~MetaData() = default;
    MetaData &operator=(const MetaData &other) = default;
    MetaData &operator=(MetaData &&other) noexcept = default;

    std::ostream &operator<<(std::ostream &os) const
    {
        os << "Title: " << title << "\n"
           << "Artist: " << artist << "\n"
           << "Album: " << album << "\n"
           << "Year: " << year << "\n"
           << "File Path: " << filePath << "\n"
           << "Parent Directory: " << parentDir << "\n"
           << "Duration: " << duration << "\n"
           << "Offset: " << offset << "\n";
        return os;
    }

    // ... (比较运算符保持不变) ...
    bool operator==(const MetaData &other) const
    {
        return title == other.title;
    }
    bool operator!=(const MetaData &other) const
    {
        return !(*this == other);
    }
    bool operator<(const MetaData &other) const
    {
        return title < other.title;
    }
    bool operator>(const MetaData &other) const
    {
        return title > other.title;
    }
    bool operator<=(const MetaData &other) const
    {
        return !(*this > other);
    }
    bool operator>=(const MetaData &other) const
    {
        return !(*this < other);
    }

    // getter
    const std::string &getTitle() const
    {
        return title;
    }
    const std::string &getArtist() const
    {
        return artist;
    }
    const std::string &getAlbum() const
    {
        return album;
    }
    const std::string &getYear() const
    {
        return year;
    }
    const std::string &getFilePath() const
    {
        return filePath;
    }
    const std::string &getParentDir() const
    {
        return parentDir;
    }
    const std::string &getCoverPath() const
    {
        return coverPath;
    }
    int64_t getDuration() const
    {
        return duration;
    }
    int64_t getOffset() const
    {
        return offset;
    } // [新增]

    // setter
    void setTitle(const std::string &title)
    {
        this->title = title;
    }
    void setArtist(const std::string &artist)
    {
        this->artist = artist;
    }
    void setAlbum(const std::string &album)
    {
        this->album = album;
    }
    void setYear(const std::string &year)
    {
        this->year = year;
    }
    void setFilePath(const std::string &filePath)
    {
        this->filePath = filePath;
    }
    void setParentDir(const std::string &parentDir)
    {
        this->parentDir = parentDir;
    }
    void setCoverPath(const std::string &coverPath)
    {
        this->coverPath = coverPath;
    }
    void setDuration(int64_t duration)
    {
        this->duration = duration;
    }
    void setOffset(int64_t offset)
    {
        this->offset = offset;
    } // [新增]
};

#endif