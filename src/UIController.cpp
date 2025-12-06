#include "uicontroller.h"
#include "FileScanner.hpp"
#include "MediaController.hpp" // 假设 MediaController.h 存在并定义了核心逻辑
#include <QDebug>
#include <QUrl>
#include <algorithm>
#include <cmath> // 引入 abs
#include <qdebug.h>
#include <qtypes.h>
#include <string>
#include "PlaylistNode.hpp"
#include "ColorExtractor.hpp"

UIController::UIController(QObject *parent) :
    QObject(parent), m_mediaController(MediaController::getInstance())
{
    // --- 1. 设置音乐默认路径 (跨平台逻辑) ---
    // Q_OS_WIN / Q_OS_LINUX 是 Qt 提供的宏定义，用于判断操作系统
#ifdef Q_OS_WIN
    // Windows: 默认使用用户的 "音乐" 文件夹
    m_defaultPath = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (m_defaultPath.isEmpty())
    {
        m_defaultPath = QCoreApplication::applicationDirPath();
    }
#elif defined(Q_OS_LINUX)
    // Linux: 默认使用用户的 "Home" 目录 (或 MusicLocation)
    m_defaultPath = QDir::homePath();
#else
    // 其他系统：使用 Home 目录
    m_defaultPath = QDir::homePath();
#endif

    // 初始化轮询定时器
    // m_stateTimer.setInterval(200);
    // connect(&m_stateTimer, &QTimer::timeout, this, &UIController::updateStateFromController);
    // m_stateTimer.start();

    // --- 2. 高频轮询超级机器! 请有需求的Connect一下喵 ---
    m_stateTimer.setInterval(100); // 100ms 检测一次，保证切歌封面更新及时
    connect(&m_stateTimer, &QTimer::timeout, this, &UIController::updateStateFromController);
    m_stateTimer.start();

    // --- 3. 低频轮询超级机器! 请有需求的Connect一下喵 ---
    m_volumeTimer.setInterval(500);
    connect(&m_volumeTimer, &QTimer::timeout, this, &UIController::updateVolumeState);
    m_volumeTimer.start(); // 启动 500ms 定时器

    // 初始化封面为空或默认图
    m_coverArtSource = "";
}

// Q_INVOKABLE 实现：开始扫描
void UIController::startMediaScan(const QString &path)
{
    QDir dir(path);
    if (!dir.exists())
    {
        qWarning() << "Scan aborted: Directory does not exist:" << path;
        return;
    }

    // 1. 调用 MediaController 设置路径和开始扫描
    // MediaController::setRootPath 接受 std::string
    m_mediaController.setRootPath(path.toStdString());
    m_mediaController.startScan();

    // 2. 更新状态并发出 NOTIFY 信号
    // TODO: 这里未来肯定是有人要用的,特别点名ListView
    if (!m_isScanning)
    {
        m_isScanning = true;
        emit isScanningChanged(true);
    }
}

QString formatTime(qint64 microsecs)
{
    // 确保时间不是负数
    if (microsecs < 0)
        microsecs = 0;

    // 转换为秒
    qint64 secs = microsecs / 1000000;
    qint64 minutes = secs / 60;
    qint64 seconds = secs % 60;

    // 使用 QChar('0') 填充到两位 (例如 05:30)
    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

// 这里的话到时候倒是可以将这个
void UIController::updateGradientColors(const QString &imagePath)
{
    // 调用 ColorExtractor 的静态方法，传入封面路径
    QList<QColor> colors = ColorExtractor::getAdaptiveGradientColors(imagePath);

    // 确保结果有效且至少包含3个颜色
    if (colors.size() >= 3)
    {
        // QColor::name() 返回 #RRGGBB 格式的字符串
        QString newColor1 = colors[0].name();
        QString newColor2 = colors[1].name();
        QString newColor3 = colors[2].name();

        // 只有颜色发生变化时才更新属性并发送信号
        if (m_gradientColor1 != newColor1 || m_gradientColor2 != newColor2 || m_gradientColor3 != newColor3)
        {
            m_gradientColor1 = newColor1;
            m_gradientColor2 = newColor2;
            m_gradientColor3 = newColor3;

            emit gradientColorsChanged(); // 通知 QML 更新界面
        }
    }
}

void UIController::setIsSeeking(bool newIsSeeking)
{
    if (m_isSeeking != newIsSeeking)
    {
        m_isSeeking = newIsSeeking;
        emit isSeekingChanged();
        qDebug() << "UI Seeking state set to:" << newIsSeeking;

        // 关键逻辑修改：
        // 原有代码在这里强制调用 checkAndUpdateTimeState()。
        // 但在暂停状态下，后端处理 seek 需要时间，强制同步会读取到旧时间，导致进度条回弹。
        // 所以这里移除强制同步，让定时器在稍后自然轮询，或者依赖 seek() 中的乐观更新。
    }
}

void UIController::playpluse()
{
    // 调用 MediaController 的播放/暂停切换
    m_mediaController.playpluse();
}

void UIController::next()
{
    // 调用 MediaController 的下一首
    m_mediaController.next();
}

void UIController::prev()
{
    // 调用 MediaController 的上一首
    m_mediaController.prev();
}

void UIController::seek(qint64 pos_microsec)
{
    // 1. 记录当前时间戳 (毫秒)
    // 这是“短期自旋锁”的开始
    m_lastSeekRequestTime = QDateTime::currentMSecsSinceEpoch();

    // 2. 发送指令给后端
    m_mediaController.seek(pos_microsec);

    // 3. 乐观更新 (Optimistic Update)
    // 立即让 UI 响应，提升跟手度
    if (m_currentPosMicrosec != pos_microsec)
    {
        m_currentPosMicrosec = pos_microsec;
        emit currentPosMicrosecChanged();

        // 立即更新显示文本
        QString newCurrentPosText = formatTime(m_currentPosMicrosec);
        if (m_currentPosText != newCurrentPosText)
        {
            m_currentPosText = newCurrentPosText;
            emit currentPosTextChanged();
        }

        // 立即更新剩余时间
        qint64 remainingMicrosecs = std::max((qint64)0, m_totalDurationMicrosec - m_currentPosMicrosec);
        QString newRemainingTimeText = formatTime(remainingMicrosecs);
        if (m_remainingTimeText != newRemainingTimeText)
        {
            m_remainingTimeText = newRemainingTimeText;
            emit remainingTimeTextChanged();
        }
    }
}

void UIController::toggleRepeatMode() // 切换外部qml的播放模式
{
    // 1. 计算新的模式 (循环：0 -> 1 -> 2 -> 0)
    int newMode = (m_repeatMode + 1) % 3; // 0, 1, 2 循环

    // 2. 通知 MediaController 后端改变状态
    // 将 int 转换为 RepeatMode 枚举
    m_mediaController.setRepeatMode(static_cast<RepeatMode>(newMode));

    // 3. 立即更新本地缓存并通知 QML
    if (m_repeatMode != newMode)
    {
        m_repeatMode = newMode;
        emit repeatModeChanged();
    }
    qDebug() << "Repeat Mode Toggled to:" << m_repeatMode;
}

void UIController::checkAndUpdateRepeatModeState() // qml监听外部的播放模式
{
    // 获取后端 RepeatMode 状态
    RepeatMode currentModeEnum = m_mediaController.getRepeatMode();

    // 转换为 int
    int currentMode = static_cast<int>(currentModeEnum);

    if (m_repeatMode != currentMode)
    {
        m_repeatMode = currentMode;
        emit repeatModeChanged();
        qDebug() << "Repeat Mode synchronized from backend:" << m_repeatMode;
    }
}

// 这里是getter们
QString UIController::defaultMusicPath() const
{
    return m_defaultPath;
}

bool UIController::isScanning() const
{
    return m_isScanning;
}

QString UIController::coverArtSource() const
{
    return m_coverArtSource;
}

QString UIController::songTitle() const
{
    return m_songTitle;
}

QString UIController::artistName() const
{
    return m_artistName;
}

QString UIController::albumName() const
{
    return m_albumName;
}

QString UIController::currentPosText() const
{
    return m_currentPosText;
}

QString UIController::remainingTimeText() const
{
    return m_remainingTimeText;
}

qint64 UIController::totalDurationMicrosec() const
{
    return m_totalDurationMicrosec;
}

qint64 UIController::currentPosMicrosec() const
{
    return m_currentPosMicrosec;
}

QString UIController::gradientColor1() const
{
    return m_gradientColor1;
}
QString UIController::gradientColor2() const
{
    return m_gradientColor2;
}
QString UIController::gradientColor3() const
{
    return m_gradientColor3;
}

bool UIController::getIsPlaying() const
{
    return m_isPlaying;
}

double UIController::getVolume() const
{
    return m_volume;
}

int UIController::getRepeatMode() const
{
    return m_repeatMode;
}

void UIController::setVolume(double volume)
{
    // 1. 调用 MediaController 的设置音量
    m_mediaController.setVolume(volume);

    // 2. 更新本地缓存
    m_volume = volume;
}

void UIController::setShuffle(bool newShuffle)
{
    // 1. 调用后端 MediaController 的 setShuffle 接口
    m_mediaController.setShuffle(newShuffle);

    // 2. 立即更新缓存状态并通知前端
    if (m_isShuffle != newShuffle)
    {
        m_isShuffle = newShuffle;
        emit isShuffleChanged();
    }
    // 注意：即使这里立即更新了，轮询机制也会在外部操作时保证最终一致性。
}

// ---这里是我的轮询里面执行的一些方法集合
void UIController::checkAndUpdateCoverArt(PlaylistNode *currentNode)
{
    QString newCoverPath = "";
    QString newTitle = "";
    QString newArtist = "";
    QString newAlbum = "";

    // 安全检查：节点是否有效
    if (currentNode != nullptr)
    {
        auto metadata = currentNode->getMetaData();
        std::string pathStr = metadata.getCoverPath();
        if (pathStr == "")
        {
            pathStr = FileScanner::extractCoverToTempFile(metadata.getFilePath(), metadata.getAlbum());
            metadata.setCoverPath(pathStr);
            currentNode->setMetaData(metadata);
        }

        QString rawPath = QString::fromStdString(pathStr);

        // 路径处理：绝对路径转 QML URL
        if (!rawPath.isEmpty())
        {
            newCoverPath = QUrl::fromLocalFile(rawPath).toString();
        }

        // 这下面的都是专辑的信息
        auto &metaData = currentNode->getMetaData();
        newTitle = QString::fromStdString(metaData.getTitle());
        newArtist = QString::fromStdString(metaData.getArtist());
        newAlbum = QString::fromStdString(metaData.getAlbum());

        // --- 这里是进度条的做法
        qint64 newDuration = 0; // 新增变量
        newDuration = m_mediaController.getDurationMicroseconds();
        if (m_totalDurationMicrosec != newDuration)
        {
            m_totalDurationMicrosec = newDuration;
            emit totalDurationMicrosecChanged();
        }
    }
    // 5. 更新属性并通知 QML,这个优化很关键的哟
    // 只有当路径真的跟上次不一样时才发信号（虽然指针变了路径通常也会变）
    if (m_coverArtSource != newCoverPath)
    {
        m_coverArtSource = newCoverPath;
        emit coverArtSourceChanged();

        std::string pathStr = currentNode->getMetaData().getCoverPath();
        QString rawPath = QString::fromStdString(pathStr);
        updateGradientColors(rawPath);
    }

    // 4. 更新歌曲标题属性并发出信号
    if (m_songTitle != newTitle)
    {
        m_songTitle = newTitle;
        emit songTitleChanged(); // 信号恢复
        qDebug() << "Song Title Updated:" << m_songTitle;
    }

    // 5. 更新歌手名属性并发出信号
    if (m_artistName != newArtist)
    {
        m_artistName = newArtist;
        emit artistNameChanged(); // 信号恢复
        qDebug() << "Artist Name Updated:" << m_artistName;
    }

    // 6. 更新专辑名属性并发出信号
    if (m_albumName != newAlbum)
    {
        m_albumName = newAlbum;
        emit albumNameChanged(); // 信号恢复
        qDebug() << "Album Name Updated:" << m_albumName;
    }
}

bool m_hasLoadedInitialData = false;

void UIController::checkAndUpdateScanState() // 这里是一个测试函数,用于将第一首歌放到我的播放器里面捏,后面可能可以给ListView
{
    // 1. 只有在 UIController 认为当前正在扫描时，才轮询后端
    if (!m_isScanning)
    {
        return;
    }

    // 2. 调用 MediaController 后端方法轮询状态
    bool scanCplt = m_mediaController.isScanCplt();

    if (scanCplt && !m_hasLoadedInitialData)
    {
        emit scanCompleted();
        // ** 扫描已完成！ **
        m_hasLoadedInitialData = true;

        // 3. 更新 UIController 的缓存状态
        m_isScanning = false;

        // 通知 QML 任务完成，可以执行后续操作（例如，开始播放/跳转主页）
        // emit scanCompleted();

        qDebug() << "Media Scan Completed. Ready to Play or navigate.";

        m_mediaController.play();
    }
}

void UIController::checkAndUpdateTimeState()
{
    // 如果用户正在拖动，绝对不要更新，否则会跟用户的手打架
    if (m_isSeeking)
    {
        return;
    }

    // [新增逻辑] Seek 稳定期保护 (Grace Period)
    // 如果距离上一次 UI 主动 Seek 操作不到 300ms（根据音频引擎延迟调整，通常 200-400ms 足够），
    // 我们假设后端音频引擎还在处理 Seek 指令，尚未更新时间戳。
    // 此时直接跳过从后端读取数据，维持 seek() 中设置的乐观值，防止进度条“回弹”。
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastSeekRequestTime < 300)
    {
        return;
    }

    // --- 以下恢复正常的同步逻辑 ---

    // 1. 获取后端原始数据 (微秒)
    qint64 activePos = m_mediaController.getCurrentPosMicroseconds();
    qint64 totalDuration = m_mediaController.getDurationMicroseconds();

    // 2. 计算剩余时间
    qint64 remainingMicrosecs = std::max((qint64)0, totalDuration - activePos);

    // 3. 格式化为 UI 文本
    QString newCurrentPosText = formatTime(activePos);
    QString newRemainingTimeText = formatTime(remainingMicrosecs);

    // 4. 更新当前位置文本并通知 QML
    if (m_currentPosText != newCurrentPosText)
    {
        m_currentPosText = newCurrentPosText;
        emit currentPosTextChanged();
    }

    // 5. 更新剩余时间文本并通知 QML
    if (m_remainingTimeText != newRemainingTimeText)
    {
        m_remainingTimeText = newRemainingTimeText;
        emit remainingTimeTextChanged();
    }

    // 6. 更新数值属性
    if (m_currentPosMicrosec != activePos)
    {
        m_currentPosMicrosec = activePos;
        emit currentPosMicrosecChanged();
    }
}

void UIController::checkAndUpdatePlayState() // 这里是检测播放状态
{
    // 1. 获取后端 MediaController 的最新状态
    bool currentIsPlaying = m_mediaController.getIsPlaying();

    // 2. 状态发生变化时，更新缓存并发出信号
    if (m_isPlaying != currentIsPlaying)
    {
        m_isPlaying = currentIsPlaying;
        emit isPlayingChanged();
        qDebug() << "Playback state changed to:" << (m_isPlaying ? "Playing" : "Paused/Stopped");
    }
}

void UIController::checkAndUpdateVolumeState() // 这里是检测音量
{
    // 1. 获取后端 MediaController 的最新音量状态
    double currentVolume = m_mediaController.getVolume();

    // 2. 状态发生变化时，更新缓存并发出信号
    if (std::abs(m_volume - currentVolume) > 0.001)
    { // 使用浮点数安全比较
        m_volume = currentVolume;
        emit volumeChanged();
    }
}

void UIController::checkAndUpdateShuffleState()
{
    // 1. 获取后端 MediaController 的最新乱序状态
    bool currentShuffle = m_mediaController.getShuffle();

    // 2. 状态发生变化时，更新缓存并发出信号
    if (m_isShuffle != currentShuffle)
    {
        m_isShuffle = currentShuffle;
        emit isShuffleChanged();
    }
}

// 高频轮询槽实现 (核心状态同步)
void UIController::updateStateFromController()
{
    checkAndUpdateScanState();

    PlaylistNode *currentNode = m_mediaController.getCurrentPlayingNode();

    // 增加局部变量标记是否切歌
    bool isSongChanged = false;

    if (currentNode != m_lastPlayingNode)
    {
        m_lastPlayingNode = currentNode;
        isSongChanged = true; // 标记发生了切歌

        // 【关键修复】立即重置 UI 状态，不等待后端
        m_currentPosMicrosec = 0;
        emit currentPosMicrosecChanged(); // 通知 UI 归零

        // 立即重置时间文本，防止显示上一首歌的结束时间
        QString zeroTime = "00:00";
        if (m_currentPosText != zeroTime)
        {
            m_currentPosText = zeroTime;
            emit currentPosTextChanged();
        }

        checkAndUpdateCoverArt(currentNode);
    }

    checkAndUpdatePlayState();

    // 【关键修复】如果刚刚切歌，跳过本次时间同步
    // 给后端 100ms 的时间去完成重置，防止读取到上一首歌的残留时间戳
    if (!isSongChanged)
    {
        checkAndUpdateTimeState();
    }
}

// 低频轮询槽实现
void UIController::updateVolumeState()
{
    // 1. 获取后端 MediaController 的最新音量状态
    // (注意：这里我们仍然需要调用 MediaController::getVolume()，因为它可能被系统混音器或其它外部因素改变)
    checkAndUpdateVolumeState();

    checkAndUpdateShuffleState();

    checkAndUpdateRepeatModeState();
}