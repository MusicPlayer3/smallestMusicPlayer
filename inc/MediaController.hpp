#ifndef _MEDIACONTROLLER_HPP_
#define _MEDIACONTROLLER_HPP_

#include "AudioPlayer.hpp"
#include "FileScanner.hpp"
#include "PlaylistNode.hpp"
#include "PCH.h"

namespace fs = std::filesystem;

class SysMediaService;

// 播放重复模式枚举
enum class RepeatMode : std::uint8_t
{
    None,     // 顺序播放，播完停止
    Playlist, // 列表循环
    Single    // 单曲循环
};

/**
 * @brief 核心媒体控制类 (单例)
 * 负责协调播放器(AudioPlayer)、文件扫描(FileScanner)和播放列表(PlaylistNode)。
 * 包含后台监控线程以处理自动切歌逻辑。
 */
class MediaController
{
private:
    // --- 构造与析构 (单例模式) ---
    MediaController();
    ~MediaController();

    static MediaController *s_instance;

    // --- 核心子模块 ---
    std::shared_ptr<SysMediaService> mediaService = nullptr; // MPRIS/系统媒体集成
    std::shared_ptr<AudioPlayer> player = nullptr;           // 底层音频播放器
    std::unique_ptr<FileScanner> scanner = nullptr;          // 文件扫描器

    // --- 数据路径与节点 ---
    std::filesystem::path rootPath;
    std::shared_ptr<PlaylistNode> rootNode = nullptr;

    // --- 线程安全与状态 ---
    // 使用递归锁，允许同一线程中的函数（如 play -> setNowPlaying）重入
    std::recursive_mutex controllerMutex;

    PlaylistNode *currentDir = nullptr;          // UI当前浏览的目录
    PlaylistNode *currentPlayingSongs = nullptr; // 当前正在播放的歌曲

    // 播放历史 (用于"上一首"功能)
    std::deque<PlaylistNode *> playHistory;
    const size_t MAX_HISTORY_SIZE = 50;

    // --- 播放控制参数 (原子变量，无锁读取) ---
    std::atomic<double> volume{1.0};
    std::atomic<bool> isShuffle{false};
    std::atomic<bool> isPlaying{false};
    std::atomic<RepeatMode> repeatMode{RepeatMode::None};

    // --- 后台监控线程 ---
    std::thread monitorThread;
    std::atomic<bool> monitorRunning{true};
    std::string lastDetectedPath = ""; // 用于检测底层播放器是否切换了文件

    // --- 内部核心逻辑 ---

    // 后台循环：检测播放位置，处理自动切歌
    void monitorLoop();

    // 预加载下一首歌曲 (实现无缝播放)
    void preloadNextSong();

    /**
     * @brief 计算下一首歌曲的节点
     * @param current 当前节点
     * @param ignoreSingleRepeat 强制忽略单曲循环(用于用户点击"下一首"时)
     * @return 下一首 PlaylistNode 指针，无则返回 nullptr
     */
    PlaylistNode *calculateNextNode(PlaylistNode *current, bool ignoreSingleRepeat = false);

    // 在当前列表范围内随机选取一首
    PlaylistNode *pickRandomSong(PlaylistNode *scope);

    /**
     * @brief 执行播放某个节点的逻辑
     * @param node 目标节点
     * @param isAutoSwitch 是否为自动切歌 (影响历史记录逻辑)
     */
    void playNode(PlaylistNode *node, bool isAutoSwitch = false);

    // 更新系统媒体中心的元数据
    void updateMetaData(PlaylistNode *node);

    // 检查路径是否在当前根目录下
    bool isPathUnderRoot(const fs::path &nodePath) const;

public:
    // 禁用拷贝
    MediaController(const MediaController &) = delete;
    MediaController &operator=(const MediaController &) = delete;

    // --- 生命周期管理 ---
    static void init();
    static void destroy();

    static MediaController &getInstance()
    {
        if (!s_instance)
        {
            throw std::runtime_error("MediaController not initialized! Call init() first.");
        }
        return *s_instance;
    }

    // 资源清理
    void cleanup();

    // --- 播放控制接口 ---
    void play();
    void pause();
    void playpluse(); // 播放/暂停切换
    void stop();
    void next();
    void prev();
    void seek(int64_t pos_microsec);

    // --- 参数设置 ---
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

    // 音频输出参数设置
    void setMixingParameters(int sampleRate, AVSampleFormat smapleFormat);
    void setOUTPUTMode(OutputMode mode);
    OutputMode getOUTPUTMode();
    AudioParams getMixingParameters();
    AudioParams getDeviceParameters();

    // --- 列表交互 ---
    void setNowPlayingSong(PlaylistNode *node);

    // 递归查找第一个可播放的音频文件 (用于初始化播放)
    PlaylistNode *findFirstValidAudio(PlaylistNode *node);

    PlaylistNode *getCurrentPlayingNode();

    // --- 状态查询 ---
    int64_t getCurrentPosMicroseconds();
    int64_t getDurationMicroseconds();

    // --- 扫描相关 ---
    void setRootPath(const std::string &path);
    void startScan();
    bool isScanCplt();
    std::shared_ptr<PlaylistNode> getRootNode();
    void setRootNode(std::shared_ptr<PlaylistNode> node)
    {
        rootNode = node;
    }
};

#endif