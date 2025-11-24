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
    double volume;

    // 元数据共享器实例
    std::unique_ptr<MetaDataSharer> sharer;

    // 播放器实例
    std::shared_ptr<AudioPlayer> player;

    // 文件扫描器实例
    std::unique_ptr<FileScanner> scanner;

#ifdef DEBUG

    std::vector<MetaData> playlist;

    void initPlayList();

#endif

public:
    // 构造函数
    explicit MediaController()
    {
        if (instance == nullptr)
        {
            instance = this;
            player = std::make_unique<AudioPlayer>();
            sharer = std::make_unique<MetaDataSharer>(player);
#ifdef DEBUG
            initPlayList();
#endif
        }
    }

    // player的控制函数
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
        player->setVolume(vol);
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

    // TODO:播放列表相关函数


    

};

#endif