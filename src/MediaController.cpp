#include "MediaController.hpp"
#include "AudioPlayer.hpp"
#include "DatabaseService.hpp"
#include "PlaylistNode.hpp"
#include "SysMediaService.hpp"
#include <mutex>

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
    scanner->initSupportedExtensions();

    scanner->setScanFinishedCallback([this](std::shared_ptr<PlaylistNode> tree)
                                     { this->handleScanFinished(tree); });
    // [New] 设置 AudioPlayer 回调
    PlayerCallbacks cbs;
    cbs.onStateChanged = [this](PlayerState s)
    { this->handlePlayerStateChange(s); };

    cbs.onPositionChanged = [this](int64_t pos)
    { this->handlePlayerPosition(pos); };

    cbs.onFileComplete = [this]()
    { this->handlePlayerFileComplete(); };

    cbs.onPathChanged = [this](std::string path)
    { this->handlePlayerPathChanged(path); };
    player->setCallbacks(cbs);

    // [Deleted] monitorRunning = true;
    // [Deleted] monitorThread = std::thread(&MediaController::monitorLoop, this);

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

    // [Deleted] monitorRunning = false;
    // [Deleted] monitorThread.join();

    if (scanner)
    {
        scanner->stopScan();
        scanner.reset();
    }
    if (player)
    {
        player->pause();
        player.reset();
    }
    if (mediaService)
    {
        mediaService.reset();
    }

    spdlog::info("[MediaController] Cleanup finished.");
}
// 新增：监听器管理
void MediaController::addListener(IMediaControllerListener *listener)
{
    std::lock_guard<std::mutex> lock(listenerMutex);
    listeners.push_back(listener);
}

void MediaController::removeListener(IMediaControllerListener *listener)
{
    std::lock_guard<std::mutex> lock(listenerMutex);
    std::erase(listeners, listener);
}

// 新增：处理 AudioPlayer 状态变更
void MediaController::handlePlayerStateChange(PlayerState state)
{
    bool playing = (state == PlayerState::Playing);
    bool changed = (isPlaying.load() != playing);
    isPlaying.store(playing);

    // 更新 MPRIS
    if (mediaService)
    {
        if (playing)
            mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
        else if (state == PlayerState::Paused)
            mediaService->setPlayBackStatus(mpris::PlaybackStatus::Paused);
        else
            mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
    }

    if (changed)
    {
        notifyStateChanged(playing);
    }
}

void MediaController::handlePlayerPosition(int64_t currentAbsMicroseconds)
{
    PlaylistNode *curr = currentPlayingSongs;

    if (curr && isPlaying.load())
    {
        int64_t startOffset = curr->getMetaData().getOffset();
        int64_t duration = curr->getMetaData().getDuration();

        // 检查是否为需要手动切歌的 CUE 轨道
        if (duration > 0)
        {
            int64_t expectedEndTime = startOffset + duration;
            if (currentAbsMicroseconds >= expectedEndTime)
            {
                // 【修复】增加一个关键判断：
                // 仅当下一首歌曲与当前歌曲在同一个物理文件时，
                // 才认为这是一个 CUE 分轨切换，由本函数处理。
                // 否则，这是一个正常的歌曲末尾，应交由 onFileComplete 处理。
                PlaylistNode *nextNode = calculateNextNode(curr);
                if (nextNode && nextNode->getPath() == curr->getPath())
                {
                    // 确认是 CUE 分轨切换，执行 next
                    std::thread([this]()
                                { this->next(); })
                        .detach();
                    return; // 处理完毕，直接返回
                }
            }
        }
    }

    // 更新相对位置用于UI显示
    int64_t relPos = 0;
    if (curr)
        relPos = std::max((int64_t)0, currentAbsMicroseconds - curr->getMetaData().getOffset());
    else
        relPos = currentAbsMicroseconds;

    notifyPositionChanged(relPos);
}

// 新增：处理 AudioPlayer 文件播放结束
void MediaController::handlePlayerFileComplete()
{
    // 物理文件播放完毕，自动切歌
    // 在独立线程中执行以避免阻塞解码器回调
    std::thread([this]()
                {
        PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);
        if (nextNode) {
            playNode(nextNode, true);
        } else {
            // 列表播完
            isPlaying = false;
            if (mediaService) mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            notifyStateChanged(false);
        } })
        .detach();
}

MediaController::~MediaController()
{
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

void MediaController::playNode(PlaylistNode *node, bool isAutoSwitch, bool forcePause)
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

    bool shouldPlay = false;
    if (forcePause)
    {
        shouldPlay = false;
    }
    else if (!isAutoSwitch)
    {
        shouldPlay = true;
    }
    else
    {
        shouldPlay = isPlaying.load();
    }
    // ----------------------

    currentPlayingSongs = node;

    isPlaying.store(shouldPlay);

    // 2. 切换逻辑
    if (oldPath != newPath)
    {
        // 物理文件改变
        player->setPath(newPath); // 底层会重置为 Stopped

        // 稍微等待解码器准备好 (可选)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        int64_t offset = node->getMetaData().getOffset();
        if (offset > 0)
        {
            player->seek(offset);
        }
    }
    else
    {
        // 物理文件相同 (CUE 分轨)
        if (shouldPlay && !player->isPlaying())
        {
            player->play();
        }
        else if (!shouldPlay && player->isPlaying())
        {
            player->pause();
        }

        int64_t offset = node->getMetaData().getOffset();
        player->seek(offset > 0 ? offset : 0);
    }

    // 3. 更新状态
    isPlaying = shouldPlay;
    updateMetaData(node);

    // 通知监听者 (UI更新封面、标题等)
    notifyTrackChanged(node);
    // 通知监听者 (UI更新播放/暂停图标)
    notifyStateChanged(shouldPlay);

    if (mediaService)
    {
        mediaService->setPlayBackStatus(shouldPlay ? mpris::PlaybackStatus::Playing : mpris::PlaybackStatus::Paused);
        mediaService->setPosition(std::chrono::microseconds(0));
        mediaService->triggerSeeked(std::chrono::microseconds(0));
    }

    preloadNextSong();

    // 4. 【核心修复】根据最新的 isPlaying 状态决定最终动作
    // 检查 isPlaying 标志，这是反映用户最新指令的真相。
    if (isPlaying.load())
    {
        // 如果最终意图是播放，且播放器当前没有在播放，则启动它。
        if (!player->isPlaying())
        {
            player->play();
        }
    }
    else
    {
        // 如果最终意图是暂停，且播放器当前正在播放，则停止它。
        if (player->isPlaying())
        {
            player->pause();
        }
    }
}

void MediaController::notifyStateChanged(bool isPlayingState)
{
    std::lock_guard<std::mutex> lock(listenerMutex);
    for (auto l : listeners)
    {
        l->onPlaybackStateChanged(isPlayingState);
    }
}

void MediaController::notifyTrackChanged(PlaylistNode *node)
{
    std::lock_guard<std::mutex> lock(listenerMutex);
    for (auto l : listeners)
    {
        l->onTrackChanged(node);
    }
}

void MediaController::notifyPositionChanged(int64_t pos)
{
    std::lock_guard<std::mutex> lock(listenerMutex);
    for (auto l : listeners)
    {
        l->onPositionChanged(pos);
    }
}

// 处理底层无缝切歌导致的路径变更
void MediaController::handlePlayerPathChanged(std::string newPath)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 记录上一首到历史记录
    if (currentPlayingSongs)
    {
        playHistory.push_back(currentPlayingSongs);
        if (playHistory.size() > MAX_HISTORY_SIZE)
            playHistory.pop_front();
    }

    // 尝试找到新路径对应的节点
    // 通常情况下，它应该是 calculateNextNode 的结果
    PlaylistNode *newNode = nullptr;
    PlaylistNode *potentialNext = calculateNextNode(currentPlayingSongs);

    if (potentialNext && potentialNext->getPath() == newPath)
    {
        newNode = potentialNext;
    }
    else
    {
        // 容错：如果不匹配（比如随机模式下预测错误），尝试在当前目录重新查找
        // 这是一个低概率的 fallback
        if (currentPlayingSongs && currentPlayingSongs->getParent())
        {
            for (auto &child : currentPlayingSongs->getParent()->getChildren())
            {
                if (child->getPath() == newPath)
                {
                    newNode = child.get();
                    break;
                }
            }
        }
    }

    // 无论是否找到节点，只要底层切了，我们必须更新状态
    if (newNode)
    {
        currentPlayingSongs = newNode;

        // 更新元数据缓存
        updateMetaData(currentPlayingSongs);

        // 通知 UI 更新 (切歌了！)
        notifyTrackChanged(currentPlayingSongs);

        // 通知状态 (虽然还在播放，但确保 UI 状态正确)
        notifyStateChanged(true);

        // [关键] Mixing 模式下切歌后，必须立刻预加载“下下一首”，
        // 否则下一首播完时就没有 buffer 进行无缝切换了。
        // 由于我们在 Audio 线程回调中，preloadNextSong 调用 player->setPreloadPath 是线程安全的。
        preloadNextSong();

        // 记录播放次数等
        DatabaseService::instance().recordPlay(newPath);
    }
    else
    {
        spdlog::warn("Seamless switch happened to path {}, but MediaController could not find the node.", newPath);
    }

    // 同步 MPRIS 进度归零
    if (mediaService)
    {
        mediaService->triggerSeeked(std::chrono::microseconds(0));
    }
}

// [新增] 处理扫描完成
void MediaController::handleScanFinished(std::shared_ptr<PlaylistNode> tree)
{
    // 注意：此函数在 FileScanner 的后台线程中被调用

    {
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        // 更新 MediaController 的根节点
        this->rootNode = tree;
        this->currentDir = tree.get();

        // 保存到数据库
        DatabaseService::instance().saveFullTree(rootNode);
    }

    spdlog::info("Scan finished. Root node updated.");

    // 通知 UI
    notifyScanFinished();
}

// [新增] 通知监听者
void MediaController::notifyScanFinished()
{
    std::lock_guard<std::mutex> lock(listenerMutex);
    for (auto l : listeners)
    {
        l->onScanFinished();
    }
}

void MediaController::prepareSong(PlaylistNode *node)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    // 调用 playNode，参数3 (forcePause) 设为 true
    // 这将加载文件、更新元数据、重置进度条，但保持暂停状态
    playNode(node, false, true);
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
    // 用户点击列表：isAutoSwitch=false, forcePause=false
    // 这将强制开始播放
    playNode(node, false, false);

    // 双重保险：如果 playNode 内部逻辑因某些原因没播，这里强制播
    if (!isPlaying)
    {
        play();
    }
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
        std::lock_guard<std::mutex> lock2(listenerMutex);
        for (auto l : listeners)
            l->onShuffleChanged(shuffle);
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
    {
        std::lock_guard<std::mutex> lock(listenerMutex);
        for (auto l : listeners)
            l->onVolumeChanged(vol);
    }
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

    // 获取元数据副本
    auto data = node->getMetaData();

    // 检查封面路径是否为空，或者路径对应的文件是否不存在 (防止缓存了被删除的临时文件)
    bool needUpdate = data.getCoverPath().empty();
    if (!needUpdate)
    {
        if (!std::filesystem::exists(data.getCoverPath()))
        {
            needUpdate = true;
        }
    }

    if (needUpdate)
    {
        // 调用我们修复后的 extractCoverToTempFile
        // 这会查找内嵌 -> 查找目录 -> 更新 data 中的 coverPath
        std::string newPath = FileScanner::extractCoverToTempFile(data);

        // 关键：将更新后的元数据设置回 Node，实现内存缓存
        // 这样 UIController 稍后读取时就能直接拿到路径
        if (!newPath.empty())
        {
            data.setCoverPath(newPath);
            node->setMetaData(data);
        }
    }

    // 将完整的元数据传给后端播放服务 (MPRIS 会使用其中的 coverPath)
    mediaService->setMetaData(data);
}

int64_t MediaController::getCurrentPosMicroseconds()
{
    if (!player)
        return 0;

    int64_t absPos = player->getCurrentPositionMicroseconds();
    int64_t offset = 0;
    int64_t duration = 0;

    // 短暂加锁以原子地读取 offset 和 duration
    {
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        if (currentPlayingSongs)
        {
            offset = currentPlayingSongs->getMetaData().getOffset();
            duration = currentPlayingSongs->getMetaData().getDuration();
        }
    }

    int64_t relPos = std::max((int64_t)0, absPos - offset);

    // 【核心修复】
    // 确保返回的相对位置永远不会超过当前歌曲的已知总时长。
    // 这可以防止因元数据与实际流时长不符而导致的UI跳变。
    if (duration > 0)
    {
        return std::min(relPos, duration);
    }

    return relPos;
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
    params.sample_rate = sampleRate;
    params.fmt = smapleFormat;
    params.ch_layout = AV_CHANNEL_LAYOUT_STEREO;
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

    // 5. [新增] 同步写入数据库
    // 注意：DatabaseService::addSong 内部会根据文件路径自动查找父目录ID，
    // 前提是 parent 对应的目录已经在数据库中（通常情况下是存在的）。
    // [Modified] 传入 coverKey 参数
    DatabaseService::instance().addSong(newNode->getMetaData(), newNode->getCoverKey());

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

    // 3. [新增] 同步删除数据库记录
    // 必须在节点被析构前获取路径进行删除
    DatabaseService::instance().removeSong(node->getPath());

    // 4. 获取待扣除的统计数据
    int64_t durSec = node->getMetaData().getDuration() / 1000000;

    // 7. 物理删除文件
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
    // 5. 从树结构中移除
    parent->removeChild(node);

    // 6. 更新统计
    updateStatsUpwards(parent, -1, -durSec);
}
void printPlaylistTree(const std::shared_ptr<PlaylistNode> &root);
bool MediaController::addFolder(const std::string &path, PlaylistNode *parent)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    spdlog::info("addFolder: Adding folder '{}' to parent '{}'", path, parent->getPath());
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
    printPlaylistTree(newDirNode);
    // 检查是否为空文件夹
    if (newDirNode->getTotalSongs() == 0)
    {
        spdlog::info("addFolder: Folder '{}' contains no audio files, skipped.", path);
        return false;
    }

    // 3. 添加到树中
    // 此时 newDirNode 的 parent 指针会被设置为参数中的 parent
    parent->addChild(newDirNode);
    parent->sortChildren();

    // 4. 更新统计
    updateStatsUpwards(parent, newDirNode->getTotalSongs(), newDirNode->getTotalDuration());

    // 由于 DatabaseService::addDirectory 只能添加单个目录，我们需要遍历新生成的子树
    std::function<void(PlaylistNode *)> recursiveDbAdd = [&](PlaylistNode *curr)
    {
        if (!curr)
            return;

        if (curr->isDir())
        {
            // 添加目录
            std::string pPath = "";
            if (curr->getParent())
            {
                pPath = curr->getParent()->getPath();
            }

            // 这里的 name 提取文件名即可
            std::string dirName = fs::path(curr->getPath()).filename().string();

            DatabaseService::instance().addDirectory(
                curr->getPath(),
                dirName,
                pPath,
                curr->getCoverKey());

            // 递归处理子节点
            for (const auto &child : curr->getChildren())
            {
                recursiveDbAdd(child.get());
            }
        }
        else
        {
            // 添加歌曲
            // [Modified] 传入 coverKey 参数
            if (!DatabaseService::instance().addSong(curr->getMetaData(), curr->getCoverKey()))
            {
                spdlog::error("addFolder: Failed to add song to database: {}", curr->getPath());
            }
        }
    };

    // 开始递归写入
    recursiveDbAdd(newDirNode.get());

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

    // 2. 清理历史记录
    std::erase_if(playHistory, [node](PlaylistNode *historyNode)
                  {
        PlaylistNode* p = historyNode;
        while (p) {
            if (p == node) return true;
            p = p->getParent().get();
        }
        return false; });

    // 3. [新增] 同步删除数据库记录
    // 数据库表定义了 ON DELETE CASCADE，删除父目录会自动级联删除所有子目录和歌曲
    DatabaseService::instance().removeDirectory(node->getPath());

    // 4. 获取待扣除的统计数据
    int64_t totalSongs = node->getTotalSongs();
    int64_t totalDur = node->getTotalDuration();

    // 5. 物理删除文件夹
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
            spdlog::info("removeFolder: Deleted directory {} : ", node->getPath(), ec.message());
        }
    }
    // 6. 从树结构中移除
    parent->removeChild(node);

    // 7. 更新统计
    updateStatsUpwards(parent, -totalSongs, -totalDur);
}

int MediaController::getSongsRating(PlaylistNode *node)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (node == nullptr)
    {
        return 0;
    }
    else if (node->isDir())
    {
        return 0;
    }
    else
    {
        auto md = node->getMetaData();
        auto rating = DatabaseService::instance().getRating(md.getFilePath());
        md.setRating(rating);
        node->setMetaData(md);
        return rating;
    }
}

void MediaController::setSongsRating(PlaylistNode *node, int rating)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (node == nullptr)
    {
        return;
    }
    else if (node->isDir())
    {
        return;
    }
    else
    {
        auto md = node->getMetaData();
        DatabaseService::instance().updateRating(md.getFilePath(), rating);
        md.setRating(rating);
        node->setMetaData(md);
    }
}

void MediaController::updateSongsPlayCount(PlaylistNode *node)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (node == nullptr)
    {
        return;
    }
    else if (node->isDir())
    {
        return;
    }
    else
    {
        auto md = node->getMetaData();
        DatabaseService::instance().recordPlay(md.getFilePath());
        md.setPlayCount(DatabaseService::instance().getPlayCount(md.getFilePath()));
        node->setMetaData(md);
    }
}

int MediaController::getSongsPlayCount(PlaylistNode *node)
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (node == nullptr)
    {
        return 0;
    }
    else if (node->isDir())
    {
        return 0;
    }
    else
    {
        auto md = node->getMetaData();
        auto res = DatabaseService::instance().getPlayCount(md.getFilePath());
        md.setPlayCount(res);
        node->setMetaData(md);
        return res;
    }
}