#ifndef _MEDIACONTROLLER_HPP_
#define _MEDIACONTROLLER_HPP_

#include "AudioPlayer.hpp"
#include "FileScanner.hpp"

#define DEBUG

class MediaController
{
private:
    // 媒体控制器实例（单例）
    static MediaController *instance;

    // 音量
    std::atomic<double> volume;

    // 元数据共享器实例

    // 播放器实例
    std::shared_ptr<AudioPlayer> player;

    // 文件扫描器实例
    std::unique_ptr<FileScanner> scanner;

    // 当前播放位置（微秒）
    std::atomic<int64_t> currentPosMicroseconds;
    // 音乐长度（微秒）
    std::atomic<int64_t> durationMicroseconds;

    // 当前播放位置（毫秒）
    std::atomic<int64_t> currentPosMillisecond;
    // 音乐长度（毫秒）
    std::atomic<int64_t> durationMillisecond;

#ifdef DEBUG

    std::vector<MetaData> playlist;

    void initPlayList();

#endif
};

#endif