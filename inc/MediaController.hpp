#ifndef _MEDIACONTROLLER_HPP_
#define _MEDIACONTROLLER_HPP_

#include "AudioPlayer.hpp"
#include "FileScanner.hpp"
#include "PlaylistNode.hpp"
#include "Precompiled.h"

namespace fs = std::filesystem;

class SysMediaService;

class MediaController
{
private:
    MediaController();
    ~MediaController();

    // --- 核心模块 ---
    std::shared_ptr<SysMediaService> mediaService = nullptr;
    std::shared_ptr<AudioPlayer> player = nullptr;
    std::unique_ptr<FileScanner> scanner = nullptr;

    // --- 数据与路径 ---
    std::filesystem::path rootPath;
    std::shared_ptr<PlaylistNode> rootNode = nullptr;

    // --- 导航与状态 ---
    std::recursive_mutex controllerMutex; // 使用递归锁以允许内部函数互相调用

    PlaylistNode *currentDir = nullptr;          // 当前UI浏览的文件夹
    PlaylistNode *currentPlayingSongs = nullptr; // 当前正在播放的歌曲节点

    // 播放历史 (用于“上一首”逻辑，存储已播放的歌曲指针)
    std::deque<PlaylistNode *> playHistory;
    // 限制历史记录最大长度，防止无限增长
    const size_t MAX_HISTORY_SIZE = 50;

    // --- 播放参数 ---
    std::atomic<double> volume{1.0};
    std::atomic<bool> isShuffle{false};
    std::atomic<bool> isPlaying{false}; // 逻辑播放状态

    // --- 监控与自动化 ---
    std::thread monitorThread;
    std::atomic<bool> monitorRunning{true};

    // 记录上一次检测到的播放路径，用于判断是否发生了自动切歌
    std::string lastDetectedPath = "";

    // --- 内部辅助函数 ---
    void monitorLoop();

    // 计算并预加载下一首歌曲 (核心无缝播放逻辑)
    void preloadNextSong();

    // 根据当前模式查找下一首节点 (不修改状态，只返回结果)
    PlaylistNode *calculateNextNode(PlaylistNode *current);

    // 在当前目录下随机选取一首
    PlaylistNode *pickRandomSong(PlaylistNode *scope);

    // 播放指定节点 (核心播放入口)
    void playNode(PlaylistNode *node, bool isAutoSwitch = false);

    // 更新系统元数据
    void updateMetaData(PlaylistNode *node);

    // 辅助：检查路径归属
    bool isPathUnderRoot(const fs::path &nodePath) const;

public:
    static MediaController &getInstance()
    {
        static MediaController instance;
        return instance;
    }

    MediaController(const MediaController &) = delete;
    MediaController &operator=(const MediaController &) = delete;

    // --- 播放控制 ---
    void play(); // 恢复播放 或 播放当前选定
    void pause();
    void playpluse(); // 播放/暂停切换
    void stop();
    void next(); // 下一首 (由当前播放模式决定)
    void prev(); // 上一首 (基于 history 队列)
    void seek(int64_t pos_microsec);

    // --- 模式设置 ---
    void setShuffle(bool shuffle);
    bool getShuffle();
    void setVolume(double volume);
    double getVolume();

    // --- 列表与导航交互 ---

    // 用户在UI列表中点击了某首歌
    void setNowPlayingSong(PlaylistNode *node);

    // 目录导航：进入子目录
    void enterDirectory(PlaylistNode *dirNode);
    // 目录导航：返回上一级 (操作 currentDir)
    void returnParentDirectory();

    PlaylistNode *getCurrentDirectory();
    PlaylistNode *getCurrentPlayingNode();

    // --- 状态获取 ---
    int64_t getCurrentPosMicroseconds();
    int64_t getDurationMicroseconds();

    // --- 初始化 ---
    void setRootPath(const std::string &path);
    void startScan();
    bool isScanCplt();
    std::shared_ptr<PlaylistNode> getRootNode();
};

#endif