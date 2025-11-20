#ifndef _METADATASHARER_HPP_
#define _METADATASHARER_HPP_

#include "Precompiled.h"
#include "mpris_server.hpp"
#include "AudioPlayer.hpp"
#include "MetaData.hpp"

class MetaDataSharer
{
private:
#ifdef __linux__

    // mpris服务器
    std::unique_ptr<mpris::Server> server;

    void onQuit();

    void onNext();

    void onPrevious();

    void onPause();

    void onPlayPause();

    void onStop();

    void onPlay();

    void onSeek(int64_t offset);

#endif

#ifdef _WIN32
    // 包含 32 位和 64 位 Windows
    void onSeek(int64_t off);
#endif
    // 后端AudioPlayer实例
    std::shared_ptr<AudioPlayer> player;

public:
    MetaDataSharer(std::shared_ptr<AudioPlayer> player);

    void setMetaData(const std::string &title, const std::string &artist, const std::string &album, const std::string &uri);

    void setMetaData(const MetaData &metadata);

    /**
     * @brief 设置当前播放状态
     *
     * @param status 播放状态
     */
    void setPlayBackStatus(mpris::PlaybackStatus status);
    /**
     * @brief 设置当前播放位置
     *
     * @param position 播放位置（微秒为单位）
     */
    void setPosition(std::chrono::microseconds position);
};

#endif