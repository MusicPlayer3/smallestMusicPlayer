#ifndef _METADATA_HPP_
#define _METADATA_HPP_

#include "PCH.h"

class MetaData
{
    using file_time_type = std::filesystem::file_time_type;

private:
    std::string title;            // 歌曲标题
    std::string artist;           // 艺术家
    std::string album;            // 专辑
    std::string year;             // 出版年份
    std::string filePath;         // 文件路径
    std::string parentDir;        // 上级目录
    std::string coverPath;        // 封面路径
    int64_t duration;             // 歌曲时长(单位微秒)
    int64_t offset;               // 起始播放偏移量(单位微秒)，默认为0
    file_time_type lastWriteTime; // 最后修改时间
    std::uint32_t sampleRate;     // 采样率
    std::uint16_t bitDepth;       // 采样深度
    std::string formatType;       // 文件格式类型
    int playCount;                // 播放次数
    int rating;                   // 星级 (0-5)

public:
    // 初始化
    MetaData() : duration(0), offset(0), sampleRate(0), bitDepth(0),
                 playCount(0), rating(0)
    {
    }

    MetaData(const std::string &title,
             const std::string &artist,
             const std::string &album,
             const std::string &year,
             const std::string &filePath,
             const std::string &parentDir,
             const std::string &coverPath,
             int64_t duration,
             int64_t offset = 0,
             file_time_type lastWriteTime = file_time_type::min(),
             std::uint32_t sampleRate = 0,
             std::uint16_t bitDepth = 0,
             const std::string &formatType = "",
             int playCount = 0,
             int rating = 0) :
        title(title),
        artist(artist),
        album(album),
        year(year),
        filePath(filePath),
        parentDir(parentDir),
        coverPath(coverPath),
        duration(duration),
        offset(offset),
        lastWriteTime(lastWriteTime),
        sampleRate(sampleRate),
        bitDepth(bitDepth),
        formatType(formatType),
        playCount(playCount),
        rating(rating)
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
           << "Cover Path: " << coverPath << "\n"
           << "Duration: " << duration << "\n"
           << "Offset: " << offset << "\n"
           << "Last Write Time: " << std::chrono::duration_cast<std::chrono::seconds>(lastWriteTime.time_since_epoch()).count() << " seconds since epoch\n"
           << "Sample Rate: " << sampleRate << "\n"
           << "Bit Depth: " << bitDepth << "\n"
           << "Format Type: " << formatType << "\n"
           << "Play Count: " << playCount << "\n"
           << "Rating: " << rating << "\n";
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
    }
    file_time_type getLastWriteTime() const
    {
        return lastWriteTime;
    }
    std::uint32_t getSampleRate() const
    {
        return sampleRate;
    }
    std::uint16_t getBitDepth() const
    {
        return bitDepth;
    }
    const std::string &getFormatType() const
    {
        return formatType;
    }
    int getPlayCount() const
    {
        return playCount;
    }
    int getRating() const
    {
        return rating;
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
    }
    void setLastWriteTime(file_time_type lastWriteTime)
    {
        this->lastWriteTime = lastWriteTime;
    }
    void setSampleRate(std::uint32_t sampleRate)
    {
        this->sampleRate = sampleRate;
    }
    void setBitDepth(std::uint16_t bitDepth)
    {
        this->bitDepth = bitDepth;
    }
    void setFormatType(const std::string &formatType)
    {
        this->formatType = formatType;
    }
    void setPlayCount(int count)
    {
        this->playCount = count;
    }
    void setRating(int rating)
    {
        // 简单范围限制，虽然数据库层面也有 Trigger 保护
        if (rating < 0)
            rating = 0;
        if (rating > 5)
            rating = 5;
        this->rating = rating;
    }
};

#endif