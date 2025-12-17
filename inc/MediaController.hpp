#ifndef _MEDIACONTROLLER_HPP_
#define _MEDIACONTROLLER_HPP_

#include "AudioPlayer.hpp"
#include "FileScanner.hpp"
#include "PlaylistNode.hpp"
#include "PCH.h"

namespace fs = std::filesystem;

class SysMediaService;

enum class RepeatMode : std::uint8_t
{
    None,     // 不启用重复
    Playlist, // 启用播放列表重复
    Single    // 启用单曲重复
};

class MediaController
{
private:
    // 私有构造，强制使用 init/destroy
    MediaController();
    ~MediaController();

    // 静态指针，用于显式管理生命周期
    static MediaController *s_instance;

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
    std::atomic<bool> isPlaying{false};

    std::atomic<RepeatMode> repeatMode{RepeatMode::None};

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
    PlaylistNode *calculateNextNode(PlaylistNode *current, bool ignoreSingleRepeat = false);

    // 在当前目录下随机选取一首
    PlaylistNode *pickRandomSong(PlaylistNode *scope);

    // 播放指定节点 (核心播放入口)
    void playNode(PlaylistNode *node, bool isAutoSwitch = false);

    // 更新系统元数据
    void updateMetaData(PlaylistNode *node);

    // 检查路径归属
    bool isPathUnderRoot(const fs::path &nodePath) const;

public:
    // 删除拷贝和赋值，保留单例访问
    MediaController(const MediaController &) = delete;
    MediaController &operator=(const MediaController &) = delete;

    // [新增] 显式初始化和销毁
    static void init();
    static void destroy();

    // [修改] 获取实例，不再创建，仅返回指针引用
    // 如果未调用 init()，此处行为未定义（或抛出异常），由 main 保证顺序
    static MediaController &getInstance()
    {
        if (!s_instance)
        {
            throw std::runtime_error("MediaController not initialized! Call init() first.");
        }
        return *s_instance;
    }

    // [新增] 之前的 cleanup 可以保留，由 destroy 调用
    void cleanup();

    // --- 播放控制 ---
    void play();
    void pause();
    void playpluse();
    void stop();
    void next();
    void prev();
    void seek(int64_t pos_microsec);

    // --- 模式设置 ---
    void setShuffle(bool shuffle);
    bool getShuffle();
    void setVolume(double volume);
    double getVolume();
    bool getIsPlaying()
    {
        return isPlaying.load();
    }
    void setRepeatMode(RepeatMode mode);
    RepeatMode getRepeatMode();
    void setMixingParameters(int sampleRate, AVSampleFormat smapleFormat);
    void setOUTPUTMode(outputMod mode);
    outputMod getOUTPUTMode();
    AudioParams getMixingParameters();
    AudioParams getDeviceParameters();

    // --- 列表与导航交互 ---
    void setNowPlayingSong(PlaylistNode *node);
    PlaylistNode *findFirstValidAudio(PlaylistNode *node);
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