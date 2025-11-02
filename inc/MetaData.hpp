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

public:
    MetaData();
    MetaData(const std::string &title,
             const std::string &artist,
             const std::string &album,
             const std::string &year,
             const std::string &filePath,
             const std::string &parentDir) :
        title(title),
        artist(artist),
        album(album),
        year(year),
        filePath(filePath),
        parentDir(parentDir)
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
           << "Parent Directory: " << parentDir << "\n";
        return os;
    }

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
};

#endif