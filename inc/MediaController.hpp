#ifndef _MEDIACONTROLLER_HPP_
#define _MEDIACONTROLLER_HPP_

#include "AudioPlayer.hpp"
#include "FileScanner.hpp"
#include "MetaDataSharer.hpp"

#define DEBUG

class MediaController
{
private:
    // 媒体控制器实例（单例）
    static MediaController *instance;

    // 音量
    std::atomic<double> volume;

    // 元数据共享器实例
    std::unique_ptr<MetaDataSharer> sharer;

    // 播放器实例
    std::shared_ptr<AudioPlayer> player;

    // 文件扫描器实例
    std::unique_ptr<FileScanner> scanner;

    std::atomic<int64_t> currentPosMicroseconds;
    std::atomic<int64_t> durationMicroseconds;

    std::atomic<int64_t> currentPosMillisecond;
    std::atomic<int64_t> durationMillisecond;

#ifdef DEBUG

    std::vector<MetaData> playlist;

    void initPlayList();

#endif
    /**
     * @brief 设置当前播放状态
     *
     */
    void setPlayBackStatus(mpris::PlaybackStatus);

    /**
     * @brief 设置当前播放位置
     *
     */
    void setPosition(std::chrono::microseconds);

    /**
     * @brief 设置与系统共享的
     *
     * @param metadata
     */
    void setMetaData(const MetaData &metadata)
    {
        sharer->setMetaData(metadata);
    }

public:
    // 构造函数
    explicit MediaController()
    {
        if (MediaController::instance == nullptr)
        {
            MediaController::instance = this;
            player = std::make_unique<AudioPlayer>();
            sharer = std::make_unique<MetaDataSharer>(player);
            scanner = std::make_unique<FileScanner>("/home/kaizen857/Music/#ffffff Records - The Unfinished - DELUXE Edition/");
#ifdef DEBUG
            initPlayList();
#endif
        }
    }

    // TODO:播放相关函数
    void play()
    {
        player->play();
    }
    void pause()
    {
        player->pause();
    }
    void seek(int64_t timeMicroSeconds)
    {
        player->seek(timeMicroSeconds);
    }

    void setVolume(double vol)
    {
        volume.store(vol);
        player->setVolume(vol);
    }

    double getVolume() const
    {
        return volume.load();
    }

    bool setNowPlayingPath(const std::string &path)
    {
        return player->setPath(path);
    }

    void setPreloadPath(const std::string &path)
    {
        player->setPreloadPath(path);
    }

    bool isPlaying() const
    {
        return player->isPlaying();
    }

    const std::string getCurrentPlayingPath() const
    {
        return player->getCurrentPath();
    }

    /**
     * 获取当前播放位置的时间
     * @return 返回当前播放位置的时间，单位为秒
     */
    int64_t getCurrentPositionTime() const
    {
        return player->getNowPlayingTime();
    }

    /**
     * 获取音频持续时间，单位为秒
     * @return 返回音频的持续时间，类型为int64_t
     */
    int64_t getAudioDuration() const
    {
        return player->getAudioDuration();
    }

    /**
     * 获取当前播放位置（微秒）
     * @return 返回当前播放位置的微秒数
     */
    int64_t getCurrentPositionMicroseconds()
    {
        return player->getCurrentPositionMicroseconds();
    }

    /**
     * 获取媒体播放时长的毫秒数
     * @return int64_t 返回播放时长的毫秒数
     */
    int64_t getDurationMillisecond()
    {
        return player->getDurationMillisecond();
    }

    /**
     * 获取音频/视频的持续时间（以微秒为单位）
     *
     * @return int64_t 返回媒体总时长的微秒数
     */
    int64_t getDurationMicroseconds()
    {
        return player->getDurationMicroseconds();
    }
    /**
     * @brief 上一首
     */
    void previous();
    /**
     * @brief 下一首
     */
    void next();

    // TODO:播放列表相关函数
};

#endif