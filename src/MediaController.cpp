#include "MediaController.hpp"
#include "SysMediaService.hpp" // 确保包含此头文件
#include <random>

// --- 静态辅助函数：递归查找第一个有效音频 ---
static PlaylistNode *findFirstValidAudio(PlaylistNode *node)
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

// --- 构造与析构 ---

MediaController::MediaController()
{
    player = std::make_shared<AudioPlayer>();
    scanner = std::make_unique<FileScanner>();

    monitorRunning = true;
    monitorThread = std::thread(&MediaController::monitorLoop, this);

    // [修改] 保持原有逻辑，Windows下不实例化真正的 Service，
    // 但下面的所有调用都需要判空。
#ifndef __WIN32__
    mediaService = std::make_shared<SysMediaService>(*this);
#endif
}

MediaController::~MediaController()
{
    monitorRunning = false;
    if (monitorThread.joinable())
    {
        monitorThread.join();
    }
}

// --- 监控线程 ---

void MediaController::monitorLoop()
{
    while (monitorRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::string realCurrentPath = player->getCurrentPath();
        int64_t currentAbsMicroseconds = player->getCurrentPositionMicroseconds();

        bool justSwitched = false;

        // ---------------------------------------------------------
        // 情况 A: 物理文件发生了切换
        // ---------------------------------------------------------
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

                // [修改] 判空保护
                if (mediaService)
                    mediaService->triggerSeeked(std::chrono::microseconds(0));

                justSwitched = true;
                preloadNextSong();
            }

            lastDetectedPath = realCurrentPath;
        }
        // ---------------------------------------------------------
        // 情况 B: 同一文件内的 CUE 分轨切换
        // ---------------------------------------------------------
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

                            // [修改] 判空保护
                            if (mediaService)
                                mediaService->triggerSeeked(std::chrono::microseconds(0));

                            preloadNextSong();
                            justSwitched = true;
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
                // [修改] 判空保护
                if (mediaService)
                    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        // 更新播放进度
        if (isPlaying.load())
        {
            if (!justSwitched)
            {
                // [修改] 判空保护
                if (mediaService)
                    mediaService->setPosition(std::chrono::microseconds(getCurrentPosMicroseconds()));
            }
        }
    }
}

// --- 核心逻辑 ---

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
    if (!node || node->isDir())
        return;

    // 1. 历史记录处理
    if (currentPlayingSongs && currentPlayingSongs != node && !isAutoSwitch)
    {
        playHistory.push_back(currentPlayingSongs);
        if (playHistory.size() > MAX_HISTORY_SIZE)
            playHistory.pop_front();
    }

    std::string oldPath = player->getCurrentPath();
    std::string newPath = node->getPath();

    // [修改] 确定预期状态：
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
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
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

    // [修改] 判空保护并设置 MPRIS 状态
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

    // 用户明确点击播放，强制播放
    player->play();
    isPlaying = true;

    // [修改] 判空保护
    if (mediaService)
    {
        mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
    }
}

void MediaController::pause()
{
    player->pause();
    isPlaying = false;

    // [修改] 判空保护
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
    player->pause();
    seek(0);
    isPlaying = false;

    // [修改] 判空保护
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

    if (getCurrentPosMicroseconds() > 10000000) // 10秒
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

        // [修改] 状态判断逻辑：同 playNode
        bool shouldPlay = isPlaying.load();

        currentPlayingSongs = prevNode;

        if (oldPath != newPath)
        {
            // 切换不同文件：让 AudioPlayer 处理状态保持
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
            // 同文件回退
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

        // [修改] 判空保护
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
    // 这里调用 playNode。根据上面 playNode 的逻辑，它会保持当前状态。
    // 如果想要"点击歌单中的歌强制播放"，可以在这里判断：
    playNode(node);
    if (!isPlaying)
    {
        play();
    }
    // 但根据需求"保持状态"，直接调用即可。
    // playNode(node);
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
        data.setCoverPath(FileScanner::extractCoverToTempFile(data.getFilePath(), data.getAlbum()));
    }

    // [修改] 判空保护
    if (mediaService)
    {
        mediaService->setMetaData(data);
    }
}

int64_t MediaController::getCurrentPosMicroseconds()
{
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
    return player->getDurationMicroseconds();
}

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
    player->seek(offset + pos_microsec);

    // [修改] 判空保护
    if (mediaService)
    {
        mediaService->setPosition(std::chrono::microseconds(pos_microsec));
    }
}

// --- 初始化与扫描 ---

void MediaController::setRootPath(const std::string &path)
{
    rootPath = path;
    scanner->setRootDir(rootPath.string());
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

void MediaController::setRepeatMode(RepeatMode mode)
{
    bool changed = (repeatMode.load() != mode);
    repeatMode.store(mode);

    // 模式改变可能会改变“下一首”的预测，更新预加载
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

std::vector<PlaylistNode *> MediaController::searchSongs(const std::string &query)
{
    std::vector<PlaylistNode *> results;
    if (query.empty())
    {
        return results;
    }

    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 确定搜索范围：
    // 题目定义“当前播放列表...包含子文件夹”。
    // 这里定义为：如果正在播放，搜索该歌曲所在文件夹(及其子目录)；
    // 如果没在播放，搜索当前浏览目录(currentDir)；如果都没，搜索根目录。
    PlaylistNode *searchRoot = nullptr;

    if (currentPlayingSongs && currentPlayingSongs->getParent())
    {
        searchRoot = currentPlayingSongs->getParent().get();
    }
    else if (currentDir)
    {
        searchRoot = currentDir;
    }
    else
    {
        searchRoot = rootNode.get();
    }

    if (!searchRoot)
        return results;

    // 预处理 query 为小写，方便模糊匹配
    std::string queryLower = query;
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    searchRecursive(searchRoot, queryLower, results);
    return results;
}

void MediaController::searchRecursive(PlaylistNode *scope, const std::string &queryLower, std::vector<PlaylistNode *> &results)
{
    if (!scope)
        return;

    const auto &children = scope->getChildren();
    for (const auto &child : children)
    {
        if (child->isDir())
        {
            // 递归进入子文件夹
            searchRecursive(child.get(), queryLower, results);
        }
        else
        {
            // 检查歌曲名
            std::string title = child->getMetaData().getTitle();
            // 如果 MetaData 为空，可能回退到检查文件名
            if (title.empty())
            {
                title = fs::path(child->getPath()).filename().string();
            }

            // 转小写
            std::string titleLower = title;
            std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(),
                           [](unsigned char c)
                           { return std::tolower(c); });

            // 模糊匹配：子串查找
            if (titleLower.find(queryLower) != std::string::npos)
            {
                results.push_back(child.get());
            }
        }
    }
}