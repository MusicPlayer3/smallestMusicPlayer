#ifndef _MEDIACONTROLLER_HPP_
#define _MEDIACONTROLLER_HPP_

#include "AudioPlayer.hpp"
#include "FileScanner.hpp"
#include "PlaylistNode.hpp"

class SysMediaService;

#define DEBUG

class MediaController
{
private:
    // 构造函数
    MediaController();

    // 根目录路径
    std::filesystem::path rootPath;
    // 播放列表根节点
    std::shared_ptr<PlaylistNode> rootNode = nullptr;

    PlaylistNode *currentNode = nullptr;

    // 音量
    std::atomic<double> volume;

    // 元数据共享器实例
    std::shared_ptr<SysMediaService> mediaService = nullptr;

    // 播放器实例
    std::shared_ptr<AudioPlayer> player = nullptr;

    // 文件扫描器实例
    std::unique_ptr<FileScanner> scanner = nullptr;

    // 当前播放位置（微秒）
    std::atomic<int64_t> currentPosMicroseconds;
    // 音乐长度（微秒）
    std::atomic<int64_t> durationMicroseconds;

    // 当前播放位置（毫秒）
    std::atomic<int64_t> currentPosMillisecond;
    // 音乐长度（毫秒）
    std::atomic<int64_t> durationMillisecond;

    std::atomic<bool> isPlaying;

#ifdef DEBUG

    std::vector<MetaData> playlist;

    void initPlayList();

#endif

public:
    static MediaController &getInstance()
    {
        static MediaController instance;
        return instance;
    }
    std::filesystem::path getRootPath()
    {
        return rootPath;
    }
    MediaController(const MediaController &) = delete;
    MediaController &operator=(const MediaController &) = delete;

    // 控制函数
    void play();            // 播放
    void pause();           // 暂停
    void playpluse();       // 播放暂停（切换）
    void stop();            // 停止
    void next();            // 下一首
    void prev();            // 上一首
    void seek(int64_t pos); // 跳转

    // 音量控制
    void setVolume(double volume);
    double getVolume();

    // 获取当前播放位置
    int64_t getCurrentPosMicroseconds()
    {
        return currentPosMicroseconds.load();
    }
    int64_t getDurationMicroseconds()
    {
        return durationMicroseconds.load();
    }
    int64_t getCurrentPosMillisecond()
    {
        return currentPosMillisecond.load();
    }
    int64_t getDurationMillisecond()
    {
        return durationMillisecond.load();
    }

    // 播放列表相关函数

    /**
     * @brief 设置根目录路径
     *
     * @param path 根目录路径（绝对路径）
     */
    void setRootPath(const std::string &path)
    {
        rootPath = path;
        scanner->setRootDir(rootPath);
    }
    /**
     * @brief 开始扫描
     *
     */
    void startScan()
    {
        scanner->startScan();
    }
    /**
     * @brief 查看是否完成扫描
     *
     * @return true 完成
     * @return false 未完成
     */
    bool isScanCplt()
    {
        return scanner->isScanCompleted();
    }

    std::shared_ptr<PlaylistNode> getRootNode()
    {
        if (isScanCplt() && rootNode == nullptr)
        {
            rootNode = scanner->getPlaylistTree();
            currentNode = rootNode.get();
        }
        return rootNode;
    }

    PlaylistNode *getCurrentNode()
    {
        return currentNode;
    }

    void setCurrentNode(PlaylistNode *node)
    {
        currentNode = node;
    }

    void currentNodeUp()
    {
        if (currentNode->getParent() != nullptr)
        {
            currentNode = currentNode->getParent().get();
        }
    }

    void currentNodeDown(const std::string &dirName)
    {

    }

    void currentNodeNext()
    {
        
    }
    void currentNodePrev()
    {
        
    }
};

#endif