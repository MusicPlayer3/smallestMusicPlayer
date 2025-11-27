#include "MediaController.hpp"
#include "mpris_server.hpp"
#include "SysMediaService.hpp"
#include <random>

// --- 静态辅助函数：递归查找第一个有效音频 ---
// 解决根目录下只有文件夹（如 Disc 01/02）导致无法播放的问题
static PlaylistNode *findFirstValidAudio(PlaylistNode *node)
{
    if (!node)
        return nullptr;

    const auto &children = node->getChildren();
    for (const auto &child : children)
    {
        if (child->isDir())
        {
            // 如果是目录，递归查找
            PlaylistNode *found = findFirstValidAudio(child.get());
            if (found)
                return found;
        }
        else
        {
            // 如果是文件，检查是否为有效音频
            if (AudioPlayer::isValidAudio(child->getPath()))
            {
                return child.get();
            }
        }
    }
    return nullptr;
}

// --- 构造与析构 ---

MediaController::MediaController()
{
    player = std::make_shared<AudioPlayer>();
    scanner = std::make_unique<FileScanner>();
    // SysMediaService 需要引用 *this

    monitorRunning = true;
    monitorThread = std::thread(&MediaController::monitorLoop, this);
    mediaService = std::make_shared<SysMediaService>(*this);
}

MediaController::~MediaController()
{
    monitorRunning = false;
    if (monitorThread.joinable())
    {
        monitorThread.join();
    }
}

// --- 监控线程 (核心：处理自动切歌与状态同步) ---

void MediaController::monitorLoop()
{
    while (monitorRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 获取播放器当前正在播放的实际文件路径
        std::string realCurrentPath = player->getCurrentPath();

        // 1. 检测是否发生了底层自动切歌 (AudioPlayer 完成了无缝切换)
        // 条件：路径变了，且不为空，且上一首也不为空（避免刚启动时的误判）
        if (!realCurrentPath.empty() && realCurrentPath != lastDetectedPath)
        {
            // 加锁更新逻辑状态
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);

            // 如果逻辑上的 currentPlayingSongs 还没更新 (说明是底层自动切的，不是用户手动点的)
            if (currentPlayingSongs && currentPlayingSongs->getPath() != realCurrentPath)
            {
                // 此时我们需要在当前目录(或整个树)中找到这个 path 对应的节点
                // 为了简化性能，我们假设预加载的就是我们计算出的那个下一首
                // 但为了严谨，我们尝试去 currentPlayingSongs 的父目录下找
                PlaylistNode *newNode = nullptr;
                if (currentPlayingSongs->getParent())
                {
                    for (auto &child : currentPlayingSongs->getParent()->getChildren())
                    {
                        if (child->getPath() == realCurrentPath)
                        {
                            newNode = child.get();
                            break;
                        }
                    }
                }

                if (newNode)
                {
                    // 记录旧歌到历史
                    playHistory.push_back(currentPlayingSongs);
                    if (playHistory.size() > MAX_HISTORY_SIZE)
                        playHistory.pop_front();

                    // 更新指针
                    currentPlayingSongs = newNode;

                    // 同步元数据到系统
                    updateMetaData(currentPlayingSongs);

                    // *** 关键：这首歌开始放了，立刻预加载再下一首 ***
                    preloadNextSong();
                }
            }

            lastDetectedPath = realCurrentPath;
        }

        // 2. 检测播放是否自然停止 (比如列表播完了，没有预加载歌曲)
        if (isPlaying.load() && player->getNowPlayingTime() > 0 && !player->isPlaying())
        {
            // 简单的防抖
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!player->isPlaying())
            {
                // 确实停了
                isPlaying = false;
                mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        // 更新播放进度
        if (isPlaying.load())
        {
            mediaService->setPosition(std::chrono::microseconds(player->getCurrentPositionMicroseconds()));
        }
    }
}

// --- 核心逻辑：计算与预加载 ---

PlaylistNode *MediaController::calculateNextNode(PlaylistNode *current)
{
    if (!current)
        return nullptr;
    auto parent = current->getParent();
    if (!parent)
        return nullptr; // 根节点下通常没有直接歌曲，或者没有父节点

    // 如果是乱序模式
    if (isShuffle.load())
    {
        // 在当前目录下随机找一首
        return pickRandomSong(parent.get());
    }

    // 顺序模式
    const auto &siblings = parent->getChildren();
    auto it = std::find_if(siblings.begin(), siblings.end(),
                           [current](const std::shared_ptr<PlaylistNode> &node)
                           {
                               return node.get() == current;
                           });

    if (it == siblings.end())
        return nullptr;

    // 往后找
    auto nextIt = it;
    while (++nextIt != siblings.end())
    {
        if (!(*nextIt)->isDir())
            return (*nextIt).get();
    }

    // 如果到了末尾，循环回到头部 (可选，这里实现循环列表)
    for (auto loopIt = siblings.begin(); loopIt != it; ++loopIt)
    {
        if (!(*loopIt)->isDir())
            return (*loopIt).get();
    }

    return nullptr;
}

void MediaController::preloadNextSong()
{
    // 这个函数必须在持有锁的情况下调用，或者确保 currentPlayingSongs 安全
    PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);

    if (nextNode)
    {
        // 调用 AudioPlayer 的预加载接口
        player->setPreloadPath(nextNode->getPath());
    }
    else
    {
        // 没有下一首了，清除预加载
        player->setPreloadPath("");
    }
}

PlaylistNode *MediaController::pickRandomSong(PlaylistNode *scope)
{
    if (!scope)
        return nullptr;
    std::vector<PlaylistNode *> candidates;
    for (auto &child : scope->getChildren())
    {
        if (!child->isDir())
            candidates.push_back(child.get());
    }

    if (candidates.empty())
        return nullptr;
    if (candidates.size() == 1)
        return candidates[0]; // 只有一首

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, candidates.size() - 1);

    // 简单随机，可能重复上一首，稍微优化可以排除当前 currentPlayingSongs
    PlaylistNode *picked = candidates[dis(gen)];
    if (picked == currentPlayingSongs && candidates.size() > 1)
    {
        // 简单的重试一次避免连续重复
        picked = candidates[dis(gen)];
    }
    return picked;
}

// --- 播放控制实现 ---

void MediaController::playNode(PlaylistNode *node, bool isAutoSwitch)
{
    if (!node || node->isDir())
        return;

    // 1. 历史记录处理
    if (currentPlayingSongs && currentPlayingSongs != node && !isAutoSwitch)
    {
        // 如果不是自动切歌（是手动点的），把前一首加入历史
        playHistory.push_back(currentPlayingSongs);
        if (playHistory.size() > MAX_HISTORY_SIZE)
            playHistory.pop_front();
    }

    currentPlayingSongs = node;

    // 2. 告诉播放器切歌
    player->setPath(node->getPath());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    player->play();

    // 3. 更新状态
    isPlaying = true;
    lastDetectedPath = node->getPath(); // 更新 path 防止 monitor 误判

    // 4. 更新元数据
    updateMetaData(node);
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
    mediaService->setPosition(std::chrono::microseconds(0));

    // 5. *** 关键：设置下一首预加载 ***
    preloadNextSong();
}

void MediaController::play()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 如果指针为空（尚未播放过），尝试从当前目录或根目录查找第一首
    if (!currentPlayingSongs)
    {
        // 确定搜索起点：优先 currentDir，若为空则用 rootNode
        PlaylistNode *searchStart = currentDir ? currentDir : rootNode.get();

        // 使用递归查找，跳过非歌曲文件和空文件夹
        PlaylistNode *firstSong = findFirstValidAudio(searchStart);

        if (firstSong)
        {
            playNode(firstSong);
        }
        return;
    }

    // 正常恢复播放
    player->play();
    isPlaying = true;
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
}

void MediaController::pause()
{
    player->pause();
    isPlaying = false;
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Paused);
}

void MediaController::playpluse()
{
    if (isPlaying)
        pause();
    else
        play();
}

void MediaController::stop()
{
    player->pause();
    seek(0);
    isPlaying = false;
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
}

void MediaController::next()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 计算下一首 (基于 Shuffle 或 顺序)
    PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);

    if (nextNode)
    {
        playNode(nextNode);
    }
    else
    {
        // 列表尽头，停止
        stop();
    }
}

void MediaController::prev()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 1. 如果当前播放时间长，则是重头放
    if (getCurrentPosMicroseconds() > 10000000) // 10秒 (原代码逻辑)
    {
        seek(0);
        return;
    }

    // 2. 检查历史队列
    if (!playHistory.empty())
    {
        PlaylistNode *prevNode = playHistory.back();
        playHistory.pop_back(); // 弹出最近的一首

        currentPlayingSongs = prevNode;
        player->setPath(prevNode->getPath());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        player->play();
        isPlaying = true;
        lastDetectedPath = prevNode->getPath();
        updateMetaData(prevNode);
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);

        // 恢复播放旧歌后，重新计算这首旧歌之后的“下一首”用于预加载
        preloadNextSong();
    }
    else
    {
        // 没有历史，尝试按顺序上一首
        seek(0);
    }
}

void MediaController::setNowPlayingSong(PlaylistNode *node)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    playNode(node);
}

// --- 模式设置 ---

void MediaController::setShuffle(bool shuffle)
{
    bool changed = (isShuffle.load() != shuffle);
    isShuffle.store(shuffle);

    if (changed)
    {
        // 模式改变了，下一首（预加载）的目标也变了
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        preloadNextSong();
    }
}

bool MediaController::getShuffle()
{
    return isShuffle.load();
}

void MediaController::setVolume(double vol)
{
    volume = vol;
    player->setVolume(vol);
}

double MediaController::getVolume()
{
    return volume;
}

// --- 目录导航 ---

void MediaController::enterDirectory(PlaylistNode *dirNode)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (dirNode && dirNode->isDir())
    {
        currentDir = dirNode;
    }
}

void MediaController::returnParentDirectory()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (currentDir && currentDir->getParent())
    {
        currentDir = currentDir->getParent().get();
    }
}

PlaylistNode *MediaController::getCurrentDirectory()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    return currentDir;
}

PlaylistNode *MediaController::getCurrentPlayingNode()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    return currentPlayingSongs;
}

// --- 辅助信息 ---

void MediaController::updateMetaData(PlaylistNode *node)
{
    if (!node)
    {
        return;
    }
    auto data = node->getMetaData();
    if (node->getMetaData().getCoverPath() == "")
    {
        FileScanner::extractCoverToTempFile(data.getFilePath(), data);
    }
    mediaService->setMetaData(data);
}

int64_t MediaController::getCurrentPosMicroseconds()
{
    return player->getCurrentPositionMicroseconds();
}

int64_t MediaController::getDurationMicroseconds()
{
    return player->getDurationMicroseconds();
}

void MediaController::seek(int64_t pos_microsec)
{
    player->seek(pos_microsec);
    mediaService->setPosition(std::chrono::microseconds(pos_microsec));
}

// --- 初始化与扫描 ---

void MediaController::setRootPath(const std::string &path)
{
    rootPath = path;
    scanner->setRootDir(rootPath);
}

void MediaController::startScan()
{
    scanner->startScan();
}

bool MediaController::isScanCplt()
{
    return scanner->isScanCompleted();
}

std::shared_ptr<PlaylistNode> MediaController::getRootNode()
{
    if (rootNode == nullptr && scanner->isScanCompleted())
    {
        rootNode = scanner->getPlaylistTree();
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        currentDir = rootNode.get();
    }
    return rootNode;
}

bool MediaController::isPathUnderRoot(const fs::path &nodePath) const
{
    fs::path canonicalRoot = fs::weakly_canonical(this->rootPath);
    fs::path canonicalNode = fs::weakly_canonical(nodePath);
    if (canonicalNode == canonicalRoot)
        return false;
    fs::path relativePath = canonicalNode.lexically_relative(canonicalRoot);
    return !relativePath.empty() && relativePath.string().find("..") != 0;
}