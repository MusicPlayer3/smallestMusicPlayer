#include "MediaController.hpp"
#include "AudioPlayer.hpp"
#include "DatabaseService.hpp"
#include "PlaylistNode.hpp"
#include "SysMediaService.hpp"

// 静态成员初始化
MediaController *MediaController::s_instance = nullptr;

void MediaController::init()
{
    if (!s_instance)
    {
        s_instance = new MediaController();
    }
}

void MediaController::destroy()
{
    if (s_instance)
    {
        s_instance->cleanup();
        delete s_instance;
        s_instance = nullptr;
    }
}

PlaylistNode *MediaController::findFirstValidAudio(PlaylistNode *node)
{
    if (!node)
        return nullptr;

    const auto &children = node->getChildren();
    for (const auto &child : children)
    {
        if (child->isDir())
        {
            PlaylistNode *found = findFirstValidAudio(child.get());
            if (found)
                return found;
        }
        else
        {
            if (AudioPlayer::isValidAudio(child->getPath()))
            {
                return child.get();
            }
        }
    }
    return nullptr;
}

MediaController::MediaController()
{
    player = std::make_shared<AudioPlayer>();
    scanner = std::make_unique<FileScanner>();

    monitorRunning = true;
    monitorThread = std::thread(&MediaController::monitorLoop, this);

#ifndef __WIN32__
    try
    {
        mediaService = std::make_shared<SysMediaService>(*this);
    }
    catch (const std::exception &e)
    {
        spdlog::warn("Failed to initialize SysMediaService: {}", e.what());
    }
#endif
}

void MediaController::cleanup()
{
    spdlog::info("[MediaController] Cleanup started.");

    // 1. 停止监控线程 (必须最先停止，防止访问已释放的 player)
    monitorRunning = false;
    if (monitorThread.joinable())
    {
        monitorThread.join();
    }
    spdlog::info("[MediaController] Monitor thread stopped.");

    // 2. 停止扫描器
    if (scanner)
    {
        scanner->stopScan();
        scanner.reset();
    }

    // 3. 停止播放器
    if (player)
    {
        player->pause();
        player.reset(); // 触发析构，停止底层音频设备
    }

    // 4. 服务清理
    if (mediaService)
    {
        mediaService.reset();
    }

    spdlog::info("[MediaController] Cleanup finished.");
}

MediaController::~MediaController()
{
    // 保底清理，防止未调用 destroy 直接 delete 的情况
    if (monitorThread.joinable())
    {
        monitorRunning = false;
        monitorThread.join();
    }
}

// 核心监控循环：检测播放进度与状态变化
void MediaController::monitorLoop()
{
    while (monitorRunning)
    {
        // 降低频率以节省 CPU，但保持足够响应
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (!player)
            break;

        std::string realCurrentPath = player->getCurrentPath();
        int64_t currentAbsMicroseconds = player->getCurrentPositionMicroseconds();
        bool justSwitched = false;

        // ----------------------------------------------------
        // 情况 A: 物理文件切换 (如播放完一首 MP3 自动切到下一首)
        // ----------------------------------------------------
        if (!realCurrentPath.empty() && realCurrentPath != lastDetectedPath)
        {
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);

            PlaylistNode *newNode = nullptr;
            PlaylistNode *potentialNext = calculateNextNode(currentPlayingSongs);

            // 检查计算出的下一首是否匹配当前播放器实际加载的路径
            if (potentialNext && potentialNext->getPath() == realCurrentPath)
            {
                newNode = potentialNext;
            }
            // 容错：如果不匹配，尝试在当前目录找
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
                playHistory.push_back(currentPlayingSongs);
                if (playHistory.size() > MAX_HISTORY_SIZE)
                    playHistory.pop_front();

                currentPlayingSongs = newNode;
                updateMetaData(currentPlayingSongs);

                if (mediaService)
                    mediaService->triggerSeeked(std::chrono::microseconds(0));

                justSwitched = true;
                preloadNextSong(); // 再次预加载下下一首
            }
            lastDetectedPath = realCurrentPath;
        }
        // ----------------------------------------------------
        // 情况 B: CUE 分轨切换 (同一个文件，时间戳越界)
        // ----------------------------------------------------
        else if (isPlaying.load() && !realCurrentPath.empty() && realCurrentPath == lastDetectedPath)
        {
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);

            if (currentPlayingSongs)
            {
                int64_t startOffset = currentPlayingSongs->getMetaData().getOffset();
                int64_t duration = currentPlayingSongs->getMetaData().getDuration();

                if (duration > 0)
                {
                    int64_t expectedEndTime = startOffset + duration;
                    // 容忍 500ms 误差防止误切
                    if (currentAbsMicroseconds >= expectedEndTime)
                    {
                        PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);
                        // CUE 切换条件：下一首依然在同一个物理文件中
                        if (nextNode && nextNode->getPath() == currentPlayingSongs->getPath())
                        {
                            playHistory.push_back(currentPlayingSongs);
                            if (playHistory.size() > MAX_HISTORY_SIZE)
                                playHistory.pop_front();

                            currentPlayingSongs = nextNode;
                            updateMetaData(currentPlayingSongs);

                            if (mediaService)
                                mediaService->triggerSeeked(std::chrono::microseconds(0));

                            preloadNextSong();
                            justSwitched = true;
                        }
                    }
                }
            }
        }
        // ----------------------------------------------------
        // 情况 C: 播放结束 (路径变空)
        // ----------------------------------------------------
        else if (isPlaying.load() && realCurrentPath.empty() && !lastDetectedPath.empty())
        {
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);
            PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);

            if (nextNode)
            {
                playNode(nextNode, true); // 自动切歌
                justSwitched = true;
            }
            else
            {
                // 列表播完，停止
                isPlaying = false;
                lastDetectedPath = "";
                if (mediaService)
                    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        // 自然停止检测 (防止状态不同步)
        if (player && isPlaying.load() && !player->isPlaying() && player->getNowPlayingTime() > 0)
        {
            // 双重确认
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (player && !player->isPlaying())
            {
                isPlaying = false;
                if (mediaService)
                    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        // 同步 MPRIS 进度
        if (isPlaying.load() && !justSwitched && mediaService)
        {
            mediaService->setPosition(std::chrono::microseconds(getCurrentPosMicroseconds()));
        }
    }
}

PlaylistNode *MediaController::calculateNextNode(PlaylistNode *current, bool ignoreSingleRepeat)
{
    if (!current)
        return nullptr;
    auto parent = current->getParent();
    if (!parent)
        return nullptr;

    // 单曲循环逻辑
    if (repeatMode.load() == RepeatMode::Single && !ignoreSingleRepeat)
    {
        return current;
    }
    // 随机播放逻辑
    if (isShuffle.load())
    {
        return pickRandomSong(parent.get());
    }

    const auto &siblings = parent->getChildren();
    // 寻找当前节点在兄弟列表中的位置
    auto it = std::find_if(siblings.begin(), siblings.end(),
                           [current](const std::shared_ptr<PlaylistNode> &node)
                           { return node.get() == current; });

    if (it == siblings.end())
        return nullptr;

    // 向后查找第一个非目录节点（即音频文件）
    auto nextIt = it;
    while (++nextIt != siblings.end())
    {
        if (!(*nextIt)->isDir())
            return (*nextIt).get();
    }

    // 列表循环逻辑：回到开头找
    if (repeatMode.load() == RepeatMode::Playlist)
    {
        for (auto loopIt = siblings.begin(); loopIt != siblings.end(); ++loopIt)
        {
            if (!(*loopIt)->isDir())
                return (*loopIt).get();
        }
    }

    return nullptr;
}

void MediaController::preloadNextSong()
{
    if (!player)
        return;
    PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);
    if (nextNode)
    {
        player->setPreloadPath(nextNode->getPath());
    }
    else
    {
        player->setPreloadPath("");
    }
}

PlaylistNode *MediaController::pickRandomSong(PlaylistNode *scope)
{
    if (!scope)
    {
        return nullptr;
    }

    // 预分配空间以提高性能
    std::vector<PlaylistNode *> candidates;
    candidates.reserve(scope->getChildren().size());

    // 使用范围for循环和emplace_back提高效率
    for (auto &child : scope->getChildren())
    {
        if (!child->isDir())
        {
            candidates.emplace_back(child.get());
        }
    }

    if (candidates.empty())
    {
        return nullptr;
    }
    if (candidates.size() == 1)
    {
        return candidates[0];
    }

    // 使用线程局部存储的随机数生成器
    thread_local static std::random_device rd;
    thread_local static std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dis(0, candidates.size() - 1);

    // 使用unordered_set存储历史记录以提高查找效率
    static std::unordered_set<PlaylistNode *> historySet(playHistory.begin(), playHistory.end());

    // 更直接的逻辑，避免无限循环
    size_t attempts = 0;
    const size_t maxAttempts = candidates.size() * 2;

    while (attempts++ < maxAttempts)
    {
        PlaylistNode *picked = candidates[dis(gen)];

        if (picked != currentPlayingSongs || candidates.size() == 1 || historySet.find(picked) == historySet.end())
        {
            return picked;
        }
    }

    // 如果多次尝试未找到合适的歌曲，返回第一个候选
    return candidates[0];
}

void MediaController::playNode(PlaylistNode *node, bool isAutoSwitch)
{
    if (!player || !node || node->isDir())
        return;

    // 1. 记录历史 (非自动切歌时)
    if (currentPlayingSongs && currentPlayingSongs != node && !isAutoSwitch)
    {
        playHistory.push_back(currentPlayingSongs);
        if (playHistory.size() > MAX_HISTORY_SIZE)
            playHistory.pop_front();
    }

    std::string oldPath = player->getCurrentPath();
    std::string newPath = node->getPath();

    // 如果是用户点击，或者已经在播放，则新歌应该处于播放状态
    bool shouldPlay = isPlaying.load() || (oldPath.empty());

    currentPlayingSongs = node;

    // 2. 切换逻辑
    if (oldPath != newPath)
    {
        // 物理文件改变
        player->setPath(newPath);

        // 微小延迟等待解码器重置
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        int64_t offset = node->getMetaData().getOffset();
        if (offset > 0)
        {
            player->seek(offset);
        }
    }
    else
    {
        // 物理文件相同 (CUE)
        if (shouldPlay && !player->isPlaying())
        {
            player->play();
        }

        int64_t offset = node->getMetaData().getOffset();
        player->seek(offset > 0 ? offset : 0);
    }

    // 3. 更新状态
    isPlaying = shouldPlay;
    lastDetectedPath = newPath;
    updateMetaData(node);

    if (mediaService)
    {
        mediaService->setPlayBackStatus(shouldPlay ? mpris::PlaybackStatus::Playing : mpris::PlaybackStatus::Paused);
        mediaService->setPosition(std::chrono::microseconds(0));
        mediaService->triggerSeeked(std::chrono::microseconds(0));
    }

    preloadNextSong();
}

void MediaController::play()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (!player)
        return;

    if (!currentPlayingSongs)
    {
        // 首次播放，自动寻找第一首歌
        PlaylistNode *searchStart = currentDir ? currentDir : rootNode.get();
        PlaylistNode *firstSong = findFirstValidAudio(searchStart);

        if (firstSong)
        {
            playNode(firstSong);
            player->play();
        }
        return;
    }

    player->play();
    isPlaying = true;
    if (mediaService)
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
}

void MediaController::pause()
{
    if (!player)
        return;
    player->pause();
    isPlaying = false;
    if (mediaService)
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
    if (!player)
        return;
    player->pause();
    seek(0);
    isPlaying = false;
    if (mediaService)
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
}

void MediaController::next()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs, true);
    if (nextNode)
        playNode(nextNode);
    else
        stop();
}

void MediaController::prev()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (!player)
        return;

    // 如果当前播放超过10秒，上一首等于重头开始
    if (getCurrentPosMicroseconds() > 10000000)
    {
        seek(0);
        return;
    }

    if (!playHistory.empty())
    {
        PlaylistNode *prevNode = playHistory.back();
        playHistory.pop_back();

        playNode(prevNode, true); // 视为自动切换，不再次压入历史栈（或根据需求调整）
    }
    else
    {
        seek(0);
    }
}

void MediaController::setNowPlayingSong(PlaylistNode *node)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    playNode(node);
    if (!isPlaying)
        play();
}

void MediaController::setShuffle(bool shuffle)
{
    bool changed = (isShuffle.load() != shuffle);
    isShuffle.store(shuffle);
#ifdef __linux__
    if (mediaService)
        mediaService->setShuffle(shuffle);
#endif

    if (changed)
    {
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
    if (player)
        player->setVolume(vol);
}

double MediaController::getVolume()
{
    return volume;
}

PlaylistNode *MediaController::getCurrentPlayingNode()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    return currentPlayingSongs;
}

void MediaController::updateMetaData(PlaylistNode *node)
{
    if (!node || !mediaService)
        return;
    auto data = node->getMetaData();

    // 如果没有封面路径，尝试提取
    if (data.getCoverPath().empty())
    {
        data.setCoverPath(FileScanner::extractCoverToTempFile(data));
        node->setMetaData(data); // 缓存回节点
    }
    mediaService->setMetaData(data);
}

int64_t MediaController::getCurrentPosMicroseconds()
{
    if (!player)
        return 0;
    int64_t absPos = player->getCurrentPositionMicroseconds();
    int64_t offset = 0;

    // 短暂加锁读取 offset
    {
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        if (currentPlayingSongs)
        {
            offset = currentPlayingSongs->getMetaData().getOffset();
        }
    }
    return std::max((int64_t)0, absPos - offset);
}

int64_t MediaController::getDurationMicroseconds()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (currentPlayingSongs)
        return currentPlayingSongs->getMetaData().getDuration();
    if (player)
        return player->getDurationMicroseconds();
    return 0;
}

void MediaController::seek(int64_t pos_microsec)
{
    if (!player)
        return;

    // 防抖动
    static auto lastSeekTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - lastSeekTime < std::chrono::milliseconds(100))
        return;
    lastSeekTime = now;

    int64_t offset = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        if (currentPlayingSongs)
            offset = currentPlayingSongs->getMetaData().getOffset();
    }
    player->seek(offset + pos_microsec);

    if (mediaService)
        mediaService->setPosition(std::chrono::microseconds(pos_microsec));
}

void MediaController::setRootPath(const std::string &path)
{
    rootPath = path;
    if (scanner)
        scanner->setRootDir(rootPath.string());
}

void MediaController::startScan()
{
    if (scanner)
        scanner->startScan();
}

bool MediaController::isScanCplt()
{
    if (!scanner)
        return false;
    bool cplt = scanner->isScanCompleted();
    if (cplt && rootNode == nullptr)
    {
        rootNode = scanner->getPlaylistTree();
        currentDir = rootNode.get();
        DatabaseService::instance().saveFullTree(rootNode);
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

void MediaController::setRepeatMode(RepeatMode mode)
{
    bool changed = (repeatMode.load() != mode);
    repeatMode.store(mode);
    if (changed)
    {
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        preloadNextSong();
    }
}

RepeatMode MediaController::getRepeatMode()
{
    return repeatMode.load();
}

void MediaController::setMixingParameters(int sampleRate, AVSampleFormat smapleFormat)
{
    if (!player)
        return;
    AudioParams params;
    params.sampleRate = sampleRate;
    params.sampleFormat = smapleFormat;
    params.ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    params.channels = 2;
    player->setMixingParameters(params);
}

void MediaController::setOUTPUTMode(OutputMode mode)
{
    if (player)
        player->setOutputMode(mode);
}

OutputMode MediaController::getOUTPUTMode()
{
    return player ? player->getOutputMode() : OutputMode::Mixing;
}

AudioParams MediaController::getMixingParameters()
{
    return player ? player->getMixingParameters() : AudioParams();
}

AudioParams MediaController::getDeviceParameters()
{
    return player ? player->getDeviceParameters() : AudioParams();
}

void MediaController::updateStatsUpwards(PlaylistNode *startNode, int64_t deltaSongs, int64_t deltaDuration)
{
    if (deltaSongs == 0 && deltaDuration == 0)
        return;

    PlaylistNode *curr = startNode;
    while (curr)
    {
        // 更新歌曲数，防止减过头
        int64_t newSongs = (int64_t)curr->getTotalSongs() + deltaSongs;
        curr->setTotalSongs(newSongs < 0 ? 0 : newSongs);

        // 更新时长
        int64_t newDur = (int64_t)curr->getTotalDuration() + deltaDuration;
        curr->setTotalDuration(newDur < 0 ? 0 : newDur);

        // 向上移动
        curr = curr->getParent().get();
    }
}

bool MediaController::isPlayingNodeOrChild(PlaylistNode *node)
{
    if (!currentPlayingSongs || !node)
        return false;

    // 情况1: 删除的就是当前正在播放的歌曲节点
    if (currentPlayingSongs == node)
        return true;

    // 情况2: 删除的是目录，需要检查当前播放的歌曲是否在这个目录树下
    if (node->isDir())
    {
        PlaylistNode *p = currentPlayingSongs;
        while (p)
        {
            if (p == node)
                return true;
            p = p->getParent().get();
        }
    }
    return false;
}

// ==========================================
// 新增：增删接口实现
// ==========================================

bool MediaController::addSong(const std::string &path, PlaylistNode *parent)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 1. 参数校验
    if (!parent || !parent->isDir())
    {
        spdlog::error("addSong: Invalid parent node (null or not a directory).");
        return false;
    }
    if (!fs::exists(path))
    {
        spdlog::error("addSong: File path does not exist: {}", path);
        return false;
    }

    // 2. 扫描文件生成节点
    auto newNode = FileScanner::scanFile(path);
    if (!newNode)
    {
        spdlog::warn("addSong: File is not a valid audio format: {}", path);
        return false;
    }

    // 3. 添加到播放列表树
    parent->addChild(newNode);
    parent->sortChildren(); // 维持排序

    // 4. 更新统计数据 (duration 单位是秒)
    int64_t durSec = newNode->getMetaData().getDuration() / 1000000;
    updateStatsUpwards(parent, 1, durSec);

    return true;
}

void MediaController::removeSong(PlaylistNode *node, bool deletePhysicalFile)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    if (!node || node->isDir())
        return;
    auto parentShared = node->getParent();
    PlaylistNode *parent = parentShared.get();

    if (!parent)
    {
        spdlog::warn("removeSong: Node has no parent, cannot remove from tree.");
        return;
    }

    // 1. 播放状态保护
    if (currentPlayingSongs == node)
    {
        // 尝试自动切到下一首
        next();
        // 如果 next() 没起作用（例如单曲且列表中只有这一首），则强制停止
        if (currentPlayingSongs == node)
        {
            stop();
            currentPlayingSongs = nullptr;
            if (mediaService)
                mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
        }
    }

    // 2. 清理历史记录
    std::erase(playHistory, node);

    // 3. 获取待扣除的统计数据
    int64_t durSec = node->getMetaData().getDuration() / 1000000;

    // 4. 从树结构中移除
    parent->removeChild(node);

    // 5. 更新统计
    updateStatsUpwards(parent, -1, -durSec);

    // 6. 物理删除文件
    if (deletePhysicalFile)
    {
        std::error_code ec;
        fs::remove(node->getPath(), ec);
        if (ec)
        {
            spdlog::error("removeSong: Failed to delete physical file {}: {}", node->getPath(), ec.message());
        }
        else
        {
            spdlog::info("removeSong: Deleted file {}", node->getPath());
        }
    }
}

bool MediaController::addFolder(const std::string &path, PlaylistNode *parent)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 1. 参数校验
    if (!parent || !parent->isDir())
    {
        spdlog::error("addFolder: Invalid parent node.");
        return false;
    }
    if (!fs::exists(path) || !fs::is_directory(path))
    {
        spdlog::error("addFolder: Directory path does not exist: {}", path);
        return false;
    }

    // 2. 扫描整个文件夹树
    auto newDirNode = FileScanner::scanDirectory(path);
    if (!newDirNode)
    {
        return false;
    }

    // [关键修改] 检查是否为空文件夹（或不包含有效音频）
    if (newDirNode->getTotalSongs() == 0)
    {
        spdlog::info("addFolder: Folder '{}' contains no audio files, skipped.", path);
        return false; // 不添加，返回失败
    }

    // 3. 添加到树中
    parent->addChild(newDirNode);
    parent->sortChildren();

    // 4. 更新统计
    updateStatsUpwards(parent, newDirNode->getTotalSongs(), newDirNode->getTotalDuration());

    return true;
}

void MediaController::removeFolder(PlaylistNode *node, bool deletePhysicalFile)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    if (!node || !node->isDir())
        return;
    auto parentShared = node->getParent();
    PlaylistNode *parent = parentShared.get();

    if (!parent)
    {
        spdlog::warn("removeFolder: Cannot remove root node or orphan node.");
        return;
    }

    // 1. 播放状态保护：检查当前播放歌曲是否在待删除文件夹内
    if (isPlayingNodeOrChild(node))
    {
        stop();
        currentPlayingSongs = nullptr;
    }

    // 2. 清理历史记录：移除所有属于该文件夹及其子文件夹的节点
    std::erase_if(playHistory, [node](PlaylistNode *historyNode)
                  {
        PlaylistNode* p = historyNode;
        while (p) {
            if (p == node) return true;
            p = p->getParent().get();
        }
        return false; });

    // 3. 获取待扣除的统计数据
    int64_t totalSongs = node->getTotalSongs();
    int64_t totalDur = node->getTotalDuration();

    // 4. 从树结构中移除
    // 此时智能指针引用计数减少，整棵子树会自动析构
    parent->removeChild(node);

    // 5. 更新统计
    updateStatsUpwards(parent, -totalSongs, -totalDur);

    // 6. 物理删除文件夹
    if (deletePhysicalFile)
    {
        std::error_code ec;
        fs::remove_all(node->getPath(), ec); // 递归删除物理文件
        if (ec)
        {
            spdlog::error("removeFolder: Failed to delete directory {}: {}", node->getPath(), ec.message());
        }
        else
        {
            spdlog::info("removeFolder: Deleted directory {}", node->getPath());
        }
    }
}