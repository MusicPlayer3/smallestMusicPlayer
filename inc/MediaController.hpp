#ifndef _MEDIACONTROLLER_HPP_
#define _MEDIACONTROLLER_HPP_

#include "AudioPlayer.hpp"
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

#ifdef DEBUG

    std::vector<MetaData> playlist;
    
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
};

#endif