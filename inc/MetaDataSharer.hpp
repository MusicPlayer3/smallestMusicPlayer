#ifndef _METADATASHARER_HPP_
#define _METADATASHARER_HPP_

#include "Precompiled.h"
#include "AudioPlayer.hpp"
#include "MetaData.hpp"

#include <memory>
#include <string>
#include <iostream>
#include <chrono>

#ifdef __linux__
#include "mpris_server.hpp"
// 引入 sdbus 头文件以使用 sdbus::ObjectPath 和 sdbus::Variant
#include <sdbus-c++/sdbus-c++.h>
#endif

class MetaDataSharer
{
private:
    // 后端AudioPlayer实例
    std::shared_ptr<AudioPlayer> player;

#ifdef __linux__
    // mpris服务器实例
    std::unique_ptr<mpris::Server> server;

    // --- MPRIS 回调函数 ---
    void onQuit();
    void onNext();
    void onPrevious();
    void onPause();
    void onPlayPause();
    void onStop();
    void onPlay();
    // MPRIS Seek 传入的是微秒偏移量
    void onSeek(int64_t offset);
    // MPRIS SetPosition 传入的是绝对位置(微秒)
    void onSetPosition(int64_t position);

    void onVolumeChange(double vol);

    /**
     * @brief 辅助函数：将本地路径转换为 file:// URI
     */
    std::string localPathToUri(const std::string &path);
#endif

#ifdef _WIN32
    // Windows 下 SMTC 的回调预留
    void onSeek(int64_t off);
#endif

public:
    MetaDataSharer(std::shared_ptr<AudioPlayer> player);
    ~MetaDataSharer();

    /**
     * @brief 设置元数据
     * @param title 标题
     * @param artist 艺术家
     * @param album 专辑
     * @param uri 歌曲文件路径
     * @param coverPath 封面图片路径 (可选)
     * @param duration 歌曲长度(微秒) (可选，默认0)
     */
    void setMetaData(const std::string &title, const std::vector<std::string> &artist,
                     const std::string &album, const std::string &uri,
                     const std::string &coverPath = "", int64_t duration = 0);

    /**
     * @brief 设置元数据 (重载)
     */
    void setMetaData(const MetaData &metadata);

    /**
     * @brief 设置当前播放状态
     * @param status 播放状态 (为了跨平台兼容，建议使用包装枚举，这里为演示直接用int或条件编译)
     */
#ifdef __linux__
    void setPlayBackStatus(mpris::PlaybackStatus status);
#else
    void setPlayBackStatus(int status); // Windows 占位
#endif

    /**
     * @brief 设置当前播放位置
     * @param position 播放位置（微秒为单位）
     */
    void setPosition(std::chrono::microseconds position);
};

#endif // _METADATASHARER_HPP_