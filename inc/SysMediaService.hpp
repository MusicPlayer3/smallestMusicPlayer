#ifndef _SYSTEM_MEDIA_SERVICE_HPP_
#define _SYSTEM_MEDIA_SERVICE_HPP_

#include <chrono>
#include "MediaController.hpp"
#ifdef __linux__

#include "mpris_server.hpp"
#include <sdbus-c++/sdbus-c++.h>
#endif

class SysMediaService
{
    MediaController &controller;

#ifdef __linux__
    // linux下mpris实例
    std::unique_ptr<mpris::Server> server;
    // --- MPRIS 回调函数 ---
    void onQuit();                                          // 退出
    void onNext();                                          // 下一首
    void onPrevious();                                      // 上一首
    void onPause();                                         // 暂停
    void onPlayPause();                                     // 播放/暂停
    void onStop();                                          // 停止
    void onPlay();                                          // 播放
    void onLoopStatusChanged(mpris::LoopStatus status);     // 循环状态改变
    void onShuffleChanged(bool shuffle);                    // 随机播放状态改变
    void onVolumeChanged(double vol);                       // 音量改变
    void onSeek(std::chrono::microseconds offset);          // 播放位置偏移
    void onSetPosition(std::chrono::microseconds position); // 设置播放位置
    /**
     * @brief 辅助函数：将本地路径转换为 file:// URI
     */
    static std::string localPathToUri(const std::string &path);
#endif

public:
    SysMediaService(MediaController &controller);
    ~SysMediaService();

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
                     const std::string &album,
                     const std::string &coverPath = "", int64_t duration = 0, const std::string &uri = "");

    /**
     * @brief 设置元数据
     * @param metadata MetaData 对象
     */
    void setMetaData(const MetaData &metadata);

    /**
     * @brief 设置当前播放位置
     * @param position 播放位置（微秒为单位）
     */
    void setPosition(std::chrono::microseconds position);

    /**
     * @brief 设置播放状态
     *
     * @param status
     */
    void setPlayBackStatus(mpris::PlaybackStatus status);
};

#endif