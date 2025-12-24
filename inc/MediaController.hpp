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

struct IMediaControllerListener
{
    virtual ~IMediaControllerListener() = default;
    virtual void onPlaybackStateChanged(bool isPlaying)
    {
    }
    virtual void onTrackChanged(PlaylistNode *newNode)
    {
    }
    virtual void onMetadataChanged(PlaylistNode *node)
    {
    }
    virtual void onPositionChanged(int64_t microsec)
    {
    }
    virtual void onVolumeChanged(double volume)
    {
    }
    virtual void onShuffleChanged(bool shuffle)
    {
    }
    virtual void onRepeatModeChanged(RepeatMode mode)
    {
    }
    virtual void onScanFinished()
    {
    }
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
    // std::thread monitorThread;
    // std::atomic<bool> monitorRunning{true};
    // std::string lastDetectedPath = ""; // 用于检测底层播放器是否切换了文件

    // // --- 内部核心逻辑 ---

    // // 后台循环：检测播放位置，处理自动切歌
    // void monitorLoop();

    // 新增：观察者列表
    std::vector<IMediaControllerListener *> listeners;
    std::mutex listenerMutex;

    // 新增：内部回调处理函数
    void handlePlayerStateChange(PlayerState state);
    void handlePlayerPosition(int64_t pos);
    void handlePlayerFileComplete();
    void handlePlayerPathChanged(std::string newPath);
    void handleScanFinished(std::shared_ptr<PlaylistNode> tree);

    // 辅助：通知函数
    void notifyStateChanged(bool isPlaying);
    void notifyTrackChanged(PlaylistNode *node);
    void notifyPositionChanged(int64_t pos);
    void notifyScanFinished();

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
     * @param forcePause 是否强制暂停 (用于启动时加载资源但不播放)
     */
    void playNode(PlaylistNode *node, bool isAutoSwitch = false, bool forcePause = false);

    // 更新系统媒体中心的元数据
    void updateMetaData(PlaylistNode *node);

    // 检查路径是否在当前根目录下
    bool isPathUnderRoot(const fs::path &nodePath) const;

    // 内部辅助：递归向上更新节点的歌曲总数和时长
    void updateStatsUpwards(PlaylistNode *startNode, int64_t deltaSongs, int64_t deltaDuration);

    // 内部辅助：检查节点是否包含当前正在播放的歌曲
    bool isPlayingNodeOrChild(PlaylistNode *node);

public:
    // 禁用拷贝
    MediaController(const MediaController &) = delete;
    MediaController &operator=(const MediaController &) = delete;

    // --- 生命周期管理 ---
    static void init();
    static void destroy();

    // 注册/注销监听器
    void addListener(IMediaControllerListener *listener);
    void removeListener(IMediaControllerListener *listener);

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

    int getSongsRating(PlaylistNode *node);              // 获取歌曲星级
    void setSongsRating(PlaylistNode *node, int rating); // 设置歌曲星级

    int getSongsPlayCount(PlaylistNode *node);     // 获取歌曲播放次数
    void updateSongsPlayCount(PlaylistNode *node); // 更新歌曲播放次数（+1）

    // 音频输出参数设置
    void setMixingParameters(int sampleRate, AVSampleFormat smapleFormat);
    void setOUTPUTMode(OutputMode mode);
    OutputMode getOUTPUTMode();
    AudioParams getMixingParameters();
    AudioParams getDeviceParameters();

    // --- 列表交互 ---
    void setNowPlayingSong(PlaylistNode *node);
    // 仅加载歌曲资源并更新UI，但不开始播放 (用于启动时)
    void prepareSong(PlaylistNode *node);

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
    /**
     * @brief 添加单首歌曲
     * @param path 音频文件的绝对路径
     * @param parent 要添加到的目标父节点（必须是文件夹）
     * @return true 添加成功, false 失败（路径不存在或非音频）
     */
    bool addSong(const std::string &path, PlaylistNode *parent);

    /**
     * @brief 删除歌曲
     * @param node 待删除的歌曲节点
     * @param deletePhysicalFile true则同时删除磁盘上的物理文件
     */
    void removeSong(PlaylistNode *node, bool deletePhysicalFile);

    /**
     * @brief 添加文件夹
     * @note 如果扫描后发现该文件夹下没有任何音频文件，则不会添加，并返回 false
     * @param path 文件夹绝对路径
     * @param parent 要添加到的目标父节点
     * @return true 添加成功, false 失败（空文件夹或路径无效）
     */
    bool addFolder(const std::string &path, PlaylistNode *parent);

    /**
     * @brief 删除文件夹及其所有子内容
     * @param node 待删除的文件夹节点
     * @param deletePhysicalFile true则递归删除磁盘上的物理文件夹及内容
     */
    void removeFolder(PlaylistNode *node, bool deletePhysicalFile);
};

#endif