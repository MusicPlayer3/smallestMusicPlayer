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

        // 获取播放器当前的状态
        std::string realCurrentPath = player->getCurrentPath();
        int64_t currentAbsMicroseconds = player->getCurrentPositionMicroseconds();

        bool justSwitched = false;

        // ---------------------------------------------------------
        // 情况 A: 物理文件发生了切换 (例如从 CD1.flac 切到了 CD2.flac)
        // ---------------------------------------------------------
        if (!realCurrentPath.empty() && realCurrentPath != lastDetectedPath)
        {
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);

            // 尝试在播放列表中找到对应的节点
            PlaylistNode *newNode = nullptr;

            // 1. 优先检查当前节点的下一个 (最常见情况)
            PlaylistNode *potentialNext = calculateNextNode(currentPlayingSongs);
            if (potentialNext && potentialNext->getPath() == realCurrentPath)
            {
                newNode = potentialNext;
            }
            // 2. 如果不是下一首，尝试在当前目录找
            else if (currentPlayingSongs && currentPlayingSongs->getParent())
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
                // 执行切换逻辑
                playHistory.push_back(currentPlayingSongs);
                if (playHistory.size() > MAX_HISTORY_SIZE)
                    playHistory.pop_front();

                currentPlayingSongs = newNode;
                updateMetaData(currentPlayingSongs);
                mediaService->triggerSeeked(std::chrono::microseconds(0));

                justSwitched = true;
                preloadNextSong(); // 预加载再下一首
            }

            lastDetectedPath = realCurrentPath;
        }
        // ---------------------------------------------------------
        // 情况 B: 同一文件内的 CUE 分轨切换 (核心修复)
        // ---------------------------------------------------------
        else if (isPlaying.load() && !realCurrentPath.empty() && realCurrentPath == lastDetectedPath)
        {
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);

            if (currentPlayingSongs)
            {
                // 获取当前逻辑轨道的边界信息
                int64_t startOffset = currentPlayingSongs->getMetaData().getOffset();
                int64_t duration = currentPlayingSongs->getMetaData().getDuration();

                // 只有当 duration 有效 (>0) 时才进行判断
                // 如果 duration 为 0，说明它是单文件且不是 CUE 分轨，或者无法计算时长
                if (duration > 0)
                {
                    // 计算预期的结束时间点 (绝对时间)
                    // 给 500ms 的容错缓冲，避免因定时器抖动导致跳过太快
                    int64_t expectedEndTime = startOffset + duration;

                    // 检测：当前物理播放位置 是否已经超过了 当前轨道的结束时间
                    if (currentAbsMicroseconds >= expectedEndTime)
                    {
                        // 看起来这首歌放完了，看看有没有下一首
                        PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);

                        // 关键判断：下一首是否属于同一个物理文件？
                        // 如果是，说明这是 CUE 的自然过渡，底层播放器不会停，我们需要手动更新 UI 状态
                        if (nextNode && nextNode->getPath() == currentPlayingSongs->getPath())
                        {
                            // --- 执行逻辑切歌 ---

                            // 1. 记录历史
                            playHistory.push_back(currentPlayingSongs);
                            if (playHistory.size() > MAX_HISTORY_SIZE)
                                playHistory.pop_front();

                            // 2. 更新当前指针
                            currentPlayingSongs = nextNode;

                            // 3. 更新系统元数据 (切歌了)
                            updateMetaData(currentPlayingSongs);

                            // 4. 重置 UI 进度条
                            // 因为底层音频流没断，UI 必须知道现在是新的一首歌，进度归零
                            mediaService->triggerSeeked(std::chrono::microseconds(0));

                            // 5. 预加载再下一首
                            preloadNextSong();

                            justSwitched = true;

                            // Log (可选)
                            // SDL_Log("Auto-switched CUE track to: %s", currentPlayingSongs->getMetaData().getTitle().c_str());
                        }
                    }
                }
            }
        }

        // 2. 检测播放是否自然停止
        if (isPlaying.load() && player->getNowPlayingTime() > 0 && !player->isPlaying())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!player->isPlaying())
            {
                isPlaying = false;
                mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        // 更新播放进度
        if (isPlaying.load())
        {
            if (!justSwitched)
            {
                mediaService->setPosition(std::chrono::microseconds(getCurrentPosMicroseconds()));
            }
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
        // 注意：预加载暂不支持 seek 到 offset，这里只支持普通文件的预加载
        // 如果 nextNode 是 CUE 的一部分，可能需要底层支持带 offset 的预加载
        // 暂维持原样，对于单文件CUE，path 是一样的，AudioPlayer 可能不会重新打开，而是继续播放
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

    // [优化] 核心逻辑调整：先 Play，稍作等待让解码器就绪，再 Seek
    // 许多后端在 stop 状态下 seek 会被 play 时的初始化重置，因此采用此顺序
    player->play();

    int64_t offset = node->getMetaData().getOffset();

    // 只有当存在有效偏移时才执行 Seek 逻辑
    if (offset > 0)
    {
        // 给底层解码线程 10~20ms 时间启动
        // 这个时间足以让 audio thread 跑起来，但对用户来说几乎不可感知
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // 执行跳转到 CUE 设定的绝对时间
        player->seek(offset);
    }

    // 3. 更新状态
    isPlaying = true;
    lastDetectedPath = node->getPath(); // 更新 path 防止 monitor 误判

    // 4. 更新元数据
    updateMetaData(node);
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);

    // [关键] 通知 MPRIS 进度归零 (相对于当前 Track 的 00:00)
    mediaService->setPosition(std::chrono::microseconds(0));
    mediaService->triggerSeeked(std::chrono::microseconds(0));

    // 5. 设置下一首预加载
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
    if (getCurrentPosMicroseconds() > 10000000) // 10秒
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

        // --- 这里的逻辑也需要同步优化 ---
        player->setPath(prevNode->getPath());

        // 先 Play
        player->play();

        int64_t offset = prevNode->getMetaData().getOffset();
        if (offset > 0)
        {
            // 等待就绪后 Seek
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            player->seek(offset);
        }

        isPlaying = true;
        lastDetectedPath = prevNode->getPath();
        updateMetaData(prevNode);
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);

        // 通知 UI 进度归零
        mediaService->triggerSeeked(std::chrono::microseconds(0));

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

// [修改] 获取当前相对播放进度 (减去 Offset)
int64_t MediaController::getCurrentPosMicroseconds()
{
    int64_t absPos = player->getCurrentPositionMicroseconds();
    int64_t offset = 0;

    // 加锁太慢，这里 currentPlayingSongs 指针本身是原子的（但在多线程下读取对象内容有风险）
    // 建议使用锁，或者确保 metadata 是不可变的
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (currentPlayingSongs)
    {
        offset = currentPlayingSongs->getMetaData().getOffset();
    }

    int64_t relPos = absPos - offset;
    return (relPos < 0) ? 0 : relPos;
}

int64_t MediaController::getDurationMicroseconds()
{
    // 如果是 CUE 分轨，应该返回 Metadata 里的 Duration，而不是物理文件的总长度
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (currentPlayingSongs)
    {
        return currentPlayingSongs->getMetaData().getDuration();
    }
    return player->getDurationMicroseconds();
}

// [修改] Seek (输入的是相对位置，需要加上 Offset 转换为绝对位置)
void MediaController::seek(int64_t pos_microsec)
{
    int64_t offset = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        if (currentPlayingSongs)
        {
            offset = currentPlayingSongs->getMetaData().getOffset();
        }
    }

    // 物理 Seek = 轨道偏移量 + 请求的相对进度
    player->seek(offset + pos_microsec);

    // UI 反馈的是相对进度
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
    bool cplt = scanner->isScanCompleted();
    if (cplt && rootNode == nullptr)
    {
        rootNode = scanner->getPlaylistTree();
    }
    return cplt;
}

std::shared_ptr<PlaylistNode> MediaController::getRootNode()
{
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