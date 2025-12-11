#include "MediaController.hpp"
#include "AudioPlayer.hpp"
#include "SysMediaService.hpp" // 确保包含此头文件
#include <random>

// --- 静态辅助函数：递归查找第一个有效音频 ---
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
        // 情况 A: 物理文件发生了切换 (外部调用 setPath 或 列表循环逻辑触发)
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

                if (mediaService)
                    mediaService->triggerSeeked(std::chrono::microseconds(0));

                justSwitched = true;
                preloadNextSong();
            }

            lastDetectedPath = realCurrentPath;
        }
        // ---------------------------------------------------------
        // 情况 B: 同一文件内的 CUE 分轨切换 (常规播放)
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

                            if (mediaService)
                                mediaService->triggerSeeked(std::chrono::microseconds(0));

                            preloadNextSong();
                            justSwitched = true;
                        }
                    }
                }
            }
        }
        // ---------------------------------------------------------
        // [修复] 情况 C: 播放器底层已自然结束并清空路径 (例如 Seek 到末尾)
        // ---------------------------------------------------------
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

        // 2. 检测播放是否自然停止
        if (isPlaying.load() && player->getNowPlayingTime() > 0 && !player->isPlaying())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!player->isPlaying() && !player->getCurrentPath().empty())
            {
                isPlaying = false;
                if (mediaService)
                    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
            }
        }

        // 更新播放进度
        if (isPlaying.load())
        {
            if (!justSwitched)
            {
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
#ifdef DEBUG
    SDL_Log("[MediaController] play()");
#endif
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
#ifdef DEBUG
    SDL_Log("[MediaController] pause()");
#endif
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

// void MediaController::enterDirectory(PlaylistNode *dirNode)
// {
//     std::lock_guard<std::recursive_mutex> lock(controllerMutex);
//     if (dirNode && dirNode->isDir())
//     {
//         currentDir = dirNode;
//     }
// }

// void MediaController::returnParentDirectory()
// {
//     std::lock_guard<std::recursive_mutex> lock(controllerMutex);
//     if (currentDir && currentDir->getParent())
//     {
//         currentDir = currentDir->getParent().get();
//     }
// }

// PlaylistNode *MediaController::getCurrentDirectory()
// {
//     std::lock_guard<std::recursive_mutex> lock(controllerMutex);
//     return currentDir;
// }

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
    // SDL_Log("[MediaController]: current absPos: %ld, offset: %ld, relPos: %ld", absPos, offset, relPos);
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

    // [修改] 判空保护
    if (mediaService)
    {
        mediaService->setPosition(std::chrono::microseconds(pos_microsec));
    }
    lastSeekTime = std::chrono::steady_clock::now();
#ifdef DEBUG
    SDL_Log("[MediaController]: seek to %ld", pos_microsec);
#endif
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

// ==========================================
// 1. 定义权重常量 (你可以根据喜好调整)
// ==========================================
namespace SearchWeights
{
const int SCORE_TITLE = 100;   // 歌名最重要
const int SCORE_ARTIST = 80;   // 歌手次之
const int SCORE_ALBUM = 60;    // 专辑
const int SCORE_FILENAME = 40; // 文件名保底

// 匹配类型的倍率
const double MULTIPLIER_EXACT = 3.0;    // 完全一样 (e.g. 搜"Hello" 命中 "Hello")
const double MULTIPLIER_PREFIX = 1.5;   // 开头就是 (e.g. 搜"He" 命中 "Hello")
const double MULTIPLIER_CONTAINS = 1.0; // 中间包含 (e.g. 搜"el" 命中 "Hello")
} // namespace SearchWeights

// ==========================================
// 2. 辅助结构体：用于存储临时搜索结果和分数
// ==========================================
struct ScoredResult
{
    PlaylistNode *node;
    int score;

    // 用于排序：分数高的在前面
    bool operator>(const ScoredResult &other) const
    {
        return score > other.score;
    }
};

// ==========================================
// 3. 辅助函数：计算单个字段的匹配分数
// ==========================================
// 参数：text(源文本), queryLower(小写的搜索词), baseWeight(该字段的基础分)
static int calculateFieldScore(const std::string &text, const std::string &queryLower, int baseWeight)
{
    if (text.empty())
        return 0;

    // 为了匹配，需要将源文本转小写（注意：这里会有内存分配，追求极致性能可用 string_view + 自定义比较）
    std::string textLower = text;
    std::transform(textLower.begin(), textLower.end(), textLower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    size_t pos = textLower.find(queryLower);

    // 1. 如果没找到，0分
    if (pos == std::string::npos)
    {
        return 0;
    }

    // 2. 计算匹配质量
    double multiplier = SearchWeights::MULTIPLIER_CONTAINS;

    if (textLower == queryLower)
    {
        // 完全匹配：分数最高
        multiplier = SearchWeights::MULTIPLIER_EXACT;
    }
    else if (pos == 0)
    {
        // 前缀匹配：分数较高 (比如搜 "周"，"周杰伦" 排在 "小周" 前面)
        multiplier = SearchWeights::MULTIPLIER_PREFIX;
    }

    return static_cast<int>(baseWeight * multiplier);
}

// ==========================================
// 4. 核心搜索逻辑实现
// ==========================================

// 声明递归函数 (修改了签名，传入 vector<ScoredResult>)
static void searchRecursiveWithScore(PlaylistNode *scope, const std::string &queryLower, std::vector<ScoredResult> &results);

std::vector<PlaylistNode *> MediaController::searchSongs(const std::string &query)
{
    std::vector<PlaylistNode *> finalResults;
    if (query.empty())
    {
        return finalResults;
    }

    std::lock_guard<std::recursive_mutex> lock(controllerMutex);

    // 1. 确定搜索根节点 (保持原有逻辑)
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
        return finalResults;

    // 2. 预处理 query 为小写
    std::string queryLower = query;
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    // 3. 收集带分数的结果
    std::vector<ScoredResult> scoredResults;
    searchRecursiveWithScore(searchRoot, queryLower, scoredResults);

    // 4. 执行加权排序 (分数从高到低)
    std::sort(scoredResults.begin(), scoredResults.end(), std::greater<ScoredResult>());

    // 5. 提取 Node 指针到最终列表
    finalResults.reserve(scoredResults.size());
    for (const auto &res : scoredResults)
    {
        finalResults.push_back(res.node);
    }

    return finalResults;
}

// 递归函数的具体实现
static void searchRecursiveWithScore(PlaylistNode *scope, const std::string &queryLower, std::vector<ScoredResult> &results)
{
    if (!scope)
        return;

    const auto &children = scope->getChildren();
    for (const auto &child : children)
    {
        if (child->isDir())
        {
            // 如果是文件夹，继续递归
            searchRecursiveWithScore(child.get(), queryLower, results);
        }
        else
        {
            // 是文件，开始多字段打分
            const MetaData &meta = child->getMetaData();
            int totalScore = 0;

            // A. 匹配标题
            totalScore += calculateFieldScore(meta.getTitle(), queryLower, SearchWeights::SCORE_TITLE);

            // B. 匹配歌手
            totalScore += calculateFieldScore(meta.getArtist(), queryLower, SearchWeights::SCORE_ARTIST);

            // C. 匹配专辑
            totalScore += calculateFieldScore(meta.getAlbum(), queryLower, SearchWeights::SCORE_ALBUM);

            // D. 匹配文件名 (如果没有元数据，通常文件名很重要)
            // 获取文件名 (例如 "song.mp3")
            std::string filename = fs::path(child->getPath()).filename().string();
            totalScore += calculateFieldScore(filename, queryLower, SearchWeights::SCORE_FILENAME);

            // 如果总分大于0，说明至少有一个字段命中了
            if (totalScore > 0)
            {
                results.push_back({child.get(), totalScore});
            }
        }
    }
}

void MediaController::setMixingParameters(int sampleRate, AVSampleFormat smapleFormat)
{
    AudioParams params;
    params.sampleRate = sampleRate;
    params.sampleFormat = smapleFormat;
    params.ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    params.channels = 2;
    player->setMixingParameters(params);
}

void MediaController::setOUTPUTMode(outputMod mode)
{
    player->setOutputMode(mode);
}

outputMod MediaController::getOUTPUTMode()
{
    return player->getOutputMode();
}

AudioParams MediaController::getMixingParameters()
{
    return player->getMixingParameters();
}

AudioParams MediaController::getDeviceParameters()
{
    return player->getDeviceParameters();
}