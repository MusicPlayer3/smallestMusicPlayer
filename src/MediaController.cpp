#include "MediaController.hpp"
#include "AudioPlayer.hpp"
#include "SysMediaService.hpp"

// [新增] 静态成员初始化
MediaController *MediaController::s_instance = nullptr;

// [新增] 显式初始化
void MediaController::init()
{
    if (!s_instance)
    {
        s_instance = new MediaController();
    }
}

// [新增] 显式销毁
void MediaController::destroy()
{
    if (s_instance)
    {
        // 先清理内部线程
        s_instance->cleanup();
        // 再删除对象
        delete s_instance;
        s_instance = nullptr;
    }
}

PlaylistNode *MediaController::findFirstValidAudio(PlaylistNode *node)
{
    if (!node)
    {
        return nullptr;
    }

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
    mediaService = std::make_shared<SysMediaService>(*this);
#endif
}

// [修改] cleanup 逻辑
void MediaController::cleanup()
{
    spdlog::info("[MediaController] Cleanup started.");

    // 1. 停止监控线程
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

    // 3. 停止播放器 (这是最关键的一步)
    if (player)
    {
        // 先暂停，避免更多回调
        player->pause();
        // 销毁对象，这将触发 AudioPlayer 的析构，进而 uninit miniaudio
        player.reset();
    }

    // 4. 服务
    if (mediaService)
    {
        mediaService.reset();
    }

    spdlog::info("[MediaController] Cleanup finished.");
}

MediaController::~MediaController()
{
    // 如果直接 delete 而没调 cleanup，这里做个保底
    if (monitorThread.joinable())
    {
        monitorRunning = false;
        monitorThread.join();
    }
}

void MediaController::monitorLoop()
{
    while (monitorRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 判空保护
        if (!player)
            break;

        std::string realCurrentPath = player->getCurrentPath();
        int64_t currentAbsMicroseconds = player->getCurrentPositionMicroseconds();

        bool justSwitched = false;

        // 情况 A: 物理文件切换
        if (!realCurrentPath.empty() && realCurrentPath != lastDetectedPath)
        {
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);

            PlaylistNode *newNode = nullptr;
            PlaylistNode *potentialNext = calculateNextNode(currentPlayingSongs);
            if (potentialNext && potentialNext->getPath() == realCurrentPath)
            {
                newNode = potentialNext;
            }
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
                preloadNextSong();
            }

            lastDetectedPath = realCurrentPath;
        }
        // 情况 B: CUE 分轨切换
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
                    if (currentAbsMicroseconds >= expectedEndTime)
                    {
                        PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);
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
        // 情况 C: 播放结束
        else if (isPlaying.load() && realCurrentPath.empty() && !lastDetectedPath.empty())
        {
            // 如果 logic 是 Playing，且刚才还有路径，现在突然没了，说明底层 AudioPlayer 跑完了
            std::lock_guard<std::recursive_mutex> lock(controllerMutex);

            PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs);

            // 只有当存在下一首时才自动播放，否则视为播放结束
            if (nextNode)
            {
                // 模拟切歌逻辑
                playNode(nextNode);

                // playNode 会更新 lastDetectedPath，所以这里不需要手动更新
                justSwitched = true;
            }
            else
            {
                // 没有下一首了 (例如单次播放且不循环)，停止状态
                isPlaying = false;
                lastDetectedPath = ""; // 重置检测路径
                if (mediaService)
                    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        // 自然停止检测
        if (player && isPlaying.load() && player->getNowPlayingTime() > 0 && !player->isPlaying())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // 再次检查 player 是否存在
            if (player && !player->isPlaying() && !player->getCurrentPath().empty())
            {
                isPlaying = false;
                if (mediaService)
                    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        if (isPlaying.load() && !justSwitched)
        {
            if (mediaService)
                mediaService->setPosition(std::chrono::microseconds(getCurrentPosMicroseconds()));
        }
    }
}

PlaylistNode *MediaController::calculateNextNode(PlaylistNode *current, bool ignoreSingleRepeat)
{
    if (!current)
    {
        return nullptr;
    }
    auto parent = current->getParent();
    if (!parent)
    {
        return nullptr;
    }

    if (repeatMode.load() == RepeatMode::Single && !ignoreSingleRepeat)
    {
        return current;
    }
    if (isShuffle.load())
    {
        return pickRandomSong(parent.get());
    }

    const auto &siblings = parent->getChildren();
    auto it = std::find_if(siblings.begin(), siblings.end(),
                           [current](const std::shared_ptr<PlaylistNode> &node)
                           {
                               return node.get() == current;
                           });

    if (it == siblings.end())
    {
        return nullptr;
    }

    auto nextIt = it;
    while (++nextIt != siblings.end())
    {
        if (!(*nextIt)->isDir())
        {
            return (*nextIt).get();
        }
    }

    if (repeatMode.load() == RepeatMode::Playlist)
    {
        for (auto loopIt = siblings.begin(); loopIt != siblings.end(); ++loopIt)
        {
            if (!(*loopIt)->isDir())
            {
                return (*loopIt).get();
            }
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
    if (picked == currentPlayingSongs && candidates.size() > 1)
    {
        picked = candidates[dis(gen)];
    }
    return picked;
}

// --- 播放控制实现 ---

void MediaController::playNode(PlaylistNode *node, bool isAutoSwitch)
{
    if (!player)
    {
        return;
    }
    if (!node || node->isDir())
    {
        return;
    }

    // 1. 历史记录处理
    if (currentPlayingSongs && currentPlayingSongs != node && !isAutoSwitch)
    {
        playHistory.push_back(currentPlayingSongs);
        if (playHistory.size() > MAX_HISTORY_SIZE)
            playHistory.pop_front();
    }

    std::string oldPath = player->getCurrentPath();
    std::string newPath = node->getPath();

    // 确定预期状态：
    // 使用 isPlaying.load() 来判断。
    // 如果当前是播放中 (isPlaying==true)，切歌后 shouldPlay=true (继续播放)。
    // 如果当前是暂停中 (isPlaying==false)，切歌后 shouldPlay=false (保持暂停)。
    // SysMediaService 没有 getPlaybackStatus()，所以我们必须信任 MediaController 自己的状态。
    bool shouldPlay = isPlaying.load();

    // 特殊处理：如果是用户手动切歌（!isAutoSwitch）且当前是停止状态，
    // 通常用户期望点击即播放，这里可以根据需求调整。
    // 但为了严格遵守"保持状态"的逻辑，我们这里完全遵从 isPlaying。
    // 注意：如果是首次播放，isPlaying 默认为 false，可能导致点击第一首歌不播放的问题。
    // 为了解决这个问题，如果 oldPath 为空（第一次播放），我们强制播放。
    if (oldPath.empty())
    {
        shouldPlay = true;
    }

    currentPlayingSongs = node;

    // 2. 核心播放控制逻辑
    if (oldPath != newPath)
    {
        // === 情况 A: 切换到了不同的音频文件 ===
        // AudioPlayer::setPath 内部会处理解码器的状态保存和恢复
        player->setPath(newPath);

        // 给一点时间让解码线程启动
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        int64_t offset = node->getMetaData().getOffset();
        if (offset > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            player->seek(offset);
        }
    }
    else
    {
        // === 情况 B: 文件路径相同 (CUE 分轨切换 / 单曲循环重播) ===

        // 如果我们期望播放，且当前实际上没在播放（例如处于 Stopped 状态），则强制 Play。
        // 如果 shouldPlay 为 false (暂停)，我们不需要调用 play()，seek 后会保持暂停。
        if (shouldPlay && !player->isPlaying())
        {
            player->play();
        }

        int64_t offset = node->getMetaData().getOffset();
        if (offset > 0)
        {
            // std::this_thread::sleep_for(std::chrono::milliseconds(15));
            player->seek(offset);
        }
        else
        {
            player->seek(0);
        }
    }

    // 3. 更新状态
    isPlaying = shouldPlay;
    lastDetectedPath = newPath;

    // 4. 更新元数据
    updateMetaData(node);

    // 判空保护并设置 MPRIS 状态
    if (mediaService)
    {
        if (shouldPlay)
            mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
        else
            mediaService->setPlayBackStatus(mpris::PlaybackStatus::Paused);

        mediaService->setPosition(std::chrono::microseconds(0));
        mediaService->triggerSeeked(std::chrono::microseconds(0));
    }

    // 5. 设置下一首预加载
    preloadNextSong();
}

void MediaController::play()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (!player)
    {
        return;
    }

    if (!currentPlayingSongs)
    {
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
    {
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
    }
}

void MediaController::pause()
{
    if (!player)
    {
        return;
    }
    player->pause();
    isPlaying = false;

    if (mediaService)
    {
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Paused);
    }
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
    {
        return;
    }
    player->pause();
    seek(0);
    isPlaying = false;

    if (mediaService)
    {
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
    }
}

void MediaController::next()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    PlaylistNode *nextNode = calculateNextNode(currentPlayingSongs, true);
    if (nextNode)
    {
        playNode(nextNode);
    }
    else
    {
        stop();
    }
}

void MediaController::prev()
{
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (!player)
    {
        return;
    }

    if (getCurrentPosMicroseconds() > 10000000)
    {
        seek(0);
        return;
    }

    if (!playHistory.empty())
    {
        PlaylistNode *prevNode = playHistory.back();
        playHistory.pop_back();

        std::string oldPath = player->getCurrentPath();
        std::string newPath = prevNode->getPath();

        bool shouldPlay = isPlaying.load();

        currentPlayingSongs = prevNode;

        if (oldPath != newPath)
        {
            player->setPath(newPath);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            int64_t offset = prevNode->getMetaData().getOffset();
            if (offset > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                player->seek(offset);
            }
        }
        else
        {
            if (shouldPlay && !player->isPlaying())
            {
                player->play();
            }

            int64_t offset = prevNode->getMetaData().getOffset();
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            player->seek(offset);
        }

        isPlaying = shouldPlay;
        lastDetectedPath = newPath;
        updateMetaData(prevNode);

        if (mediaService)
        {
            if (shouldPlay)
            {
                mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
            }
            else
            {
                mediaService->setPlayBackStatus(mpris::PlaybackStatus::Paused);
            }

            mediaService->triggerSeeked(std::chrono::microseconds(0));
        }
        preloadNextSong();
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
    {
        play();
    }
}

// --- 模式设置 ---

void MediaController::setShuffle(bool shuffle)
{
    bool changed = (isShuffle.load() != shuffle);
    isShuffle.store(shuffle);
#ifdef __linux__
    if (mediaService)
    {
        mediaService->setShuffle(shuffle);
    }
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
    {
        player->setVolume(vol);
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
        data.setCoverPath(FileScanner::extractCoverToTempFile(data));
    }

    if (mediaService)
    {
        mediaService->setMetaData(data);
    }
}

int64_t MediaController::getCurrentPosMicroseconds()
{
    if (!player)
    {
        return 0;
    }
    int64_t absPos = player->getCurrentPositionMicroseconds();
    int64_t offset = 0;

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
    std::lock_guard<std::recursive_mutex> lock(controllerMutex);
    if (currentPlayingSongs)
    {
        return currentPlayingSongs->getMetaData().getDuration();
    }
    if (player)
    {
        return player->getDurationMicroseconds();
    }
    return 0;
}

void MediaController::seek(int64_t pos_microsec)
{
    if (!player)
    {
        return;
    }
    static std::chrono::time_point<std::chrono::steady_clock> lastSeekTime;
    if (std::chrono::steady_clock::now() - lastSeekTime < std::chrono::milliseconds(100))
    {
        return;
    }
    int64_t offset = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(controllerMutex);
        if (currentPlayingSongs)
        {
            offset = currentPlayingSongs->getMetaData().getOffset();
        }
    }
    player->seek(offset + pos_microsec);

    if (mediaService)
    {
        mediaService->setPosition(std::chrono::microseconds(pos_microsec));
    }
    lastSeekTime = std::chrono::steady_clock::now();
}

// --- 初始化与扫描 ---

void MediaController::setRootPath(const std::string &path)
{
    rootPath = path;
    if (scanner)
    {
        scanner->setRootDir(rootPath.string());
    }
}

void MediaController::startScan()
{
    if (scanner)
    {
        scanner->startScan();
    }
}

bool MediaController::isScanCplt()
{
    if (!scanner)
    {
        return false;
    }
    bool cplt = scanner->isScanCompleted();
    if (cplt && rootNode == nullptr)
    {
        rootNode = scanner->getPlaylistTree();
        currentDir = rootNode.get();
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
    {
        return false;
    }
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
    {
        return;
    }
    AudioParams params;
    params.sampleRate = sampleRate;
    params.sampleFormat = smapleFormat;
    params.ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    params.channels = 2;
    player->setMixingParameters(params);
}

void MediaController::setOUTPUTMode(outputMod mode)
{
    if (player)
    {
        player->setOutputMode(mode);
    }
}

outputMod MediaController::getOUTPUTMode()
{
    if (player)
    {
        return player->getOutputMode();
    }
    return OUTPUT_MIXING;
}

AudioParams MediaController::getMixingParameters()
{
    if (player)
    {
        return player->getMixingParameters();
    }
    return AudioParams();
}

AudioParams MediaController::getDeviceParameters()
{
    if (player)
    {
        return player->getDeviceParameters();
    }
    return AudioParams();
}