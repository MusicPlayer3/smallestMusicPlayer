#include "MediaController.hpp"
#include "AudioPlayer.hpp"
#include "DatabaseService.hpp"
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
        return candidates[0];

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, candidates.size() - 1);

    PlaylistNode *picked = candidates[dis(gen)];
    // 如果随到了当前正在放的，再试一次 (但不强制死循环)
    if (picked == currentPlayingSongs && candidates.size() > 1)
    {
        picked = candidates[dis(gen)];
    }
    return picked;
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