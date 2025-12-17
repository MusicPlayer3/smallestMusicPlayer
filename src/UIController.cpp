#include "uicontroller.h"
#include "FileScanner.hpp"
#include "MediaController.hpp"
#include "PlaylistNode.hpp"
#include "AudioPlayer.hpp"

UIController::UIController(QObject *parent) :
    QObject(parent), m_mediaController(MediaController::getInstance())
{
#ifdef Q_OS_WIN
    m_defaultPath = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (m_defaultPath.isEmpty())
        m_defaultPath = QCoreApplication::applicationDirPath();
#elif defined(Q_OS_LINUX)
    m_defaultPath = QDir::homePath();
#else
    m_defaultPath = QDir::homePath();
#endif

    m_stateTimer.setInterval(100);
    connect(&m_stateTimer, &QTimer::timeout, this, &UIController::updateStateFromController);
    m_stateTimer.start();

    m_volumeTimer.setInterval(500);
    connect(&m_volumeTimer, &QTimer::timeout, this, &UIController::updateVolumeState);
    m_volumeTimer.start();

    m_coverArtSource = "";

    // 连接异步监听器
    connect(&m_waveformWatcher, &QFutureWatcher<AsyncWaveformResult>::finished,
            this, &UIController::onWaveformCalculationFinished);
    m_currentWaveformGeneration = 0;

    // 初始化 OutputMode
    m_outputMode = static_cast<int>(m_mediaController.getOUTPUTMode());

    // 初始化默认的深色背景
    m_gradientColor1 = "#232323";
    m_gradientColor2 = "#1a1a1a";
    m_gradientColor3 = "#121212";
}

// [新增] 析构清理
UIController::~UIController()
{
    if (m_waveformWatcher.isRunning())
    {
        m_waveformWatcher.cancel();
        m_waveformWatcher.waitForFinished();
    }
}

void UIController::startMediaScan(const QString &path)
{
    QDir dir(path);
    if (!dir.exists())
    {
        qWarning() << "Scan aborted: Directory does not exist:" << path;
        return;
    }
    m_mediaController.setRootPath(path.toStdString());
    m_mediaController.startScan();
    if (!m_isScanning)
    {
        m_isScanning = true;
        emit isScanningChanged(true);
    }
}

QString formatTime(qint64 microsecs)
{
    if (microsecs < 0)
        microsecs = 0;
    qint64 secs = microsecs / 1000000;
    qint64 minutes = secs / 60;
    qint64 seconds = secs % 60;
    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

// 计算两个颜色的差异 (欧氏距离近似)
double colorDistance(const QColor &c1, const QColor &c2)
{
    long rmean = ((long)c1.red() + (long)c2.red()) / 2;
    long r = (long)c1.red() - (long)c2.red();
    long g = (long)c1.green() - (long)c2.green();
    long b = (long)c1.blue() - (long)c2.blue();
    return std::sqrt((((512 + rmean) * r * r) >> 8) + 4 * g * g + (((767 - rmean) * b * b) >> 8));
}

void UIController::updateGradientColors(const QString &imagePath)
{
    QImage image(imagePath);

    // 默认备用色
    QList<QColor> palette = {QColor("#2d2d2d"), QColor("#353535"), QColor("#404040")};

    if (!image.isNull())
    {
        // 1. 缩小图片 (20x20) 以快速分析
        QImage small = image.scaled(20, 20, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        struct ColorScore
        {
            QColor color;
            double score;
        };
        std::vector<ColorScore> candidates;

        // 2. 遍历像素评分，寻找"活力色"作为基础
        for (int y = 0; y < small.height(); ++y)
        {
            for (int x = 0; x < small.width(); ++x)
            {
                QColor c = small.pixelColor(x, y);
                // 剔除过黑过白
                if (c.lightness() < 20 || c.lightness() > 240)
                    continue;

                // 评分：优先选择高饱和度颜色（这样能抓到封面的主色调）
                // 稍后我们会统一降低饱和度，但这里需要先选出有代表性的颜色
                double score = c.saturationF() * 2.0 + (1.0 - std::abs(c.lightnessF() - 0.5));
                candidates.push_back({c, score});
            }
        }

        // 3. 排序：分数高的在前
        std::sort(candidates.begin(), candidates.end(), [](const ColorScore &a, const ColorScore &b)
                  { return a.score > b.score; });

        // 4. 挑选差异较大的 3 个颜色
        palette.clear();
        for (const auto &item : candidates)
        {
            bool isDistinct = true;
            for (const auto &selected : palette)
            {
                if (colorDistance(item.color, selected) < 80.0)
                { // 稍微降低阈值以允许近似色
                    isDistinct = false;
                    break;
                }
            }
            if (isDistinct)
            {
                palette.append(item.color);
                if (palette.size() >= 3)
                    break;
            }
        }

        // 补齐不足的颜色
        while (palette.size() < 3)
        {
            if (!palette.isEmpty())
                palette.append(palette.last().darker(110));
            else
                palette.append(QColor("#2d2d2d"));
        }
    }

    // [关键修改] 5. 后处理：限制饱和度和亮度范围
    // 这一步确保了背景不会因为封面颜色过于鲜艳而显得刺眼或"花哨"
    for (auto &color : palette)
    {
        float h, s, l;
        color.getHslF(&h, &s, &l);

        // 限制饱和度最大为 0.5 (50%)
        // 这样既保留了色相(Hue)，又让颜色看起来更柔和、高级
        if (s > 0.5)
            s = 0.5;

        // 也可以稍微限制亮度，防止背景太亮影响白字阅读
        // if (l > 0.6) l = 0.6;

        color = QColor::fromHslF(h, s, l);
    }

    QString newColor1 = palette[0].name();
    QString newColor2 = palette[1].name();
    QString newColor3 = palette[2].name();

    if (m_gradientColor1 != newColor1 || m_gradientColor2 != newColor2 || m_gradientColor3 != newColor3)
    {
        m_gradientColor1 = newColor1;
        m_gradientColor2 = newColor2;
        m_gradientColor3 = newColor3;
        emit gradientColorsChanged();
    }
}

void UIController::setIsSeeking(bool newIsSeeking)
{
    if (m_isSeeking != newIsSeeking)
    {
        m_isSeeking = newIsSeeking;
        emit isSeekingChanged();
    }
}

void UIController::playpluse()
{
    m_mediaController.playpluse();
}
void UIController::next()
{
    m_mediaController.next();
}
void UIController::prev()
{
    m_mediaController.prev();
}

void UIController::seek(qint64 pos_microsec)
{
    m_lastSeekRequestTime = QDateTime::currentMSecsSinceEpoch();
    m_mediaController.seek(pos_microsec);

    if (m_currentPosMicrosec != pos_microsec)
    {
        m_currentPosMicrosec = pos_microsec;
        emit currentPosMicrosecChanged();

        QString newCurrentPosText = formatTime(m_currentPosMicrosec);
        if (m_currentPosText != newCurrentPosText)
        {
            m_currentPosText = newCurrentPosText;
            emit currentPosTextChanged();
        }

        qint64 remainingMicrosecs = std::max((qint64)0, m_totalDurationMicrosec - m_currentPosMicrosec);
        QString newRemainingTimeText = formatTime(remainingMicrosecs);
        if (m_remainingTimeText != newRemainingTimeText)
        {
            m_remainingTimeText = newRemainingTimeText;
            emit remainingTimeTextChanged();
        }
    }
}

void UIController::toggleRepeatMode()
{
    int newMode = (m_repeatMode + 1) % 3;
    m_mediaController.setRepeatMode(static_cast<RepeatMode>(newMode));
    if (m_repeatMode != newMode)
    {
        m_repeatMode = newMode;
        emit repeatModeChanged();
    }
}

void UIController::checkAndUpdateRepeatModeState()
{
    RepeatMode currentModeEnum = m_mediaController.getRepeatMode();
    int currentMode = static_cast<int>(currentModeEnum);
    if (m_repeatMode != currentMode)
    {
        m_repeatMode = currentMode;
        emit repeatModeChanged();
    }
}

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
    m_mediaController.setVolume(volume);
    m_volume = volume;
}

void UIController::setShuffle(bool newShuffle)
{
    m_mediaController.setShuffle(newShuffle);
    if (m_isShuffle != newShuffle)
    {
        m_isShuffle = newShuffle;
        emit isShuffleChanged();
    }
}

// [新增] Output Mode 相关实现
int UIController::outputMode() const
{
    return m_outputMode;
}

void UIController::setOutputMode(int mode)
{
    if (mode < 0 || mode > 1)
        return;

    outputMod newMode = (mode == 0) ? OUTPUT_DIRECT : OUTPUT_MIXING;
    m_mediaController.setOUTPUTMode(newMode);

    // 更新本地缓存
    if (m_outputMode != mode)
    {
        m_outputMode = mode;
        emit outputModeChanged();
    }
}

// 辅助函数：Index <-> AVSampleFormat
AVSampleFormat UIController::indexToAvFormat(int index)
{
    switch (index)
    {
    case 0: return AV_SAMPLE_FMT_S16;
    case 1: return AV_SAMPLE_FMT_S32;
    case 2: return AV_SAMPLE_FMT_FLT;
    case 3: return AV_SAMPLE_FMT_DBL;
    default: return AV_SAMPLE_FMT_FLT;
    }
}

int UIController::avFormatToIndex(AVSampleFormat fmt)
{
    // 简化匹配，忽略 planar 区别
    switch (fmt)
    {
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: return 0;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: return 1;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: return 2;
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP: return 3;
    default: return 3;
    }
}

void UIController::applyMixingParams(int sampleRate, int formatIndex)
{
    // 1. 设置参数
    AVSampleFormat fmt = indexToAvFormat(formatIndex);
    m_mediaController.setMixingParameters(sampleRate, fmt);

    // 2. [修改] 等待 500ms 后读取 (符合需求)
    QTimer::singleShot(500, this, [=, this]()
                       {
        AudioParams currentParams = m_mediaController.getDeviceParameters();
        
        int actualRate = currentParams.sampleRate;
        int actualFmtIndex = avFormatToIndex(currentParams.sampleFormat);
        
        emit mixingParamsApplied(actualRate, actualFmtIndex); });
}

QVariantMap UIController::getCurrentDeviceParams()
{
    AudioParams params = m_mediaController.getDeviceParameters();

    QVariantMap result;
    result["sampleRate"] = params.sampleRate;
    result["formatIndex"] = avFormatToIndex(params.sampleFormat);

    return result;
}

void UIController::checkAndUpdateOutputMode()
{
    int current = static_cast<int>(m_mediaController.getOUTPUTMode());
    if (m_outputMode != current)
    {
        m_outputMode = current;
        emit outputModeChanged();
    }
}


// [生成波形] 改为异步
void UIController::generateWaveformForNode(PlaylistNode *node)
{
    // 1. 立即清空当前波形，触发 UI 的“收缩”归零动画
    m_waveformHeights.clear();
    emit waveformHeightsChanged();

    m_currentWaveformGeneration++;
    quint64 thisRequestId = m_currentWaveformGeneration;

    if (!node && node->isDir())
    {
        return;
    }

    std::string stdFilePath = node->getMetaData().getFilePath();
    if (stdFilePath.empty())
    {
        return;
    }

    // 准备数据 (在主线程栈上)
    QString filePath = QString::fromStdString(stdFilePath);
    auto startTime = node->getMetaData().getOffset();
    auto endTime = node->getMetaData().getOffset() + node->getMetaData().getDuration(); // 注意：这里通常是 Start + Duration = End

    // 3. 在后台线程执行计算
    // 使用 QtConcurrent::run 运行 lambda
    QFuture<AsyncWaveformResult> future = QtConcurrent::run([=]()
                                                            {
        AsyncWaveformResult result;
        result.generationId = thisRequestId; // 携带 ID 回来
        result.filePath = filePath;
        
        int barCount = 70;
        int totalWidth = 320; 
        int calculatedBarWidth = 0;
        int maxHeight = 60; 

        // 这是一个耗时操作
        result.heights = AudioPlayer::buildAudioWaveform(
            filePath.toStdString(),
            barCount,
            totalWidth,
            calculatedBarWidth,
            maxHeight,
            startTime,
            endTime); // 传入正确的时间片段

        result.barWidth = calculatedBarWidth;
        return result; });

    // 4. 设置 Watcher 监听这个 Future
    m_waveformWatcher.setFuture(future);
}

// [新增] 异步回调
void UIController::onWaveformCalculationFinished()
{
    // 获取结果
    AsyncWaveformResult result = m_waveformWatcher.result();

    // [关键] 检查结果是否过时
    // 如果在计算过程中用户又切了一首歌，m_currentWaveformPath 会变成新歌的路径
    // 此时 result.filePath 是旧歌的路径，不匹配，则丢弃，防止 UI 闪烁
    if (result.generationId != m_currentWaveformGeneration)
    {
        // qDebug() << "Discarding expired waveform result. Got ID:" << result.generationId << " Expected:" << m_currentWaveformGeneration;
        return;
    }

    // 更新 UI 数据
    QVariantList newList;
    for (int h : result.heights)
    {
        newList.append(h);
    }

    m_waveformHeights = newList;
    m_waveformBarWidth = result.barWidth;

    // 触发 UI 的“展开”动画
    emit waveformHeightsChanged();
}

void UIController::checkAndUpdateCoverArt(PlaylistNode *currentNode)
{
    QString newCoverPath = "";
    QString newTitle = "";
    QString newArtist = "";
    QString newAlbum = "";

    if (currentNode != nullptr)
    {
        auto metadata = currentNode->getMetaData();
        std::string pathStr = metadata.getCoverPath();
        if (pathStr == "")
        {
            pathStr = FileScanner::extractCoverToTempFile(metadata);
            metadata.setCoverPath(pathStr);
            currentNode->setMetaData(metadata);
        }
        QString rawPath = QString::fromStdString(pathStr);
        if (!rawPath.isEmpty())
        {
            newCoverPath = QUrl::fromLocalFile(rawPath).toString();
        }

        auto &metaData = currentNode->getMetaData();
        newTitle = QString::fromStdString(metaData.getTitle());
        newArtist = QString::fromStdString(metaData.getArtist());
        newAlbum = QString::fromStdString(metaData.getAlbum());

        qint64 newDuration = m_mediaController.getDurationMicroseconds();
        if (m_totalDurationMicrosec != newDuration)
        {
            m_totalDurationMicrosec = newDuration;
            emit totalDurationMicrosecChanged();
        }
    }

    if (m_coverArtSource != newCoverPath)
    {
        m_coverArtSource = newCoverPath;
        emit coverArtSourceChanged();
        if (currentNode)
        {
            std::string pathStr = currentNode->getMetaData().getCoverPath();
            updateGradientColors(QString::fromStdString(pathStr));
        }
    }

    if (m_songTitle != newTitle)
    {
        m_songTitle = newTitle;
        emit songTitleChanged();
    }
    if (m_artistName != newArtist)
    {
        m_artistName = newArtist;
        emit artistNameChanged();
    }
    if (m_albumName != newAlbum)
    {
        m_albumName = newAlbum;
        emit albumNameChanged();
    }
}

bool m_hasLoadedInitialData = false;
void UIController::checkAndUpdateScanState()
{
    if (!m_isScanning)
        return;
    bool scanCplt = m_mediaController.isScanCplt();
    if (scanCplt && !m_hasLoadedInitialData)
    {
        emit scanCompleted();
        m_hasLoadedInitialData = true;
        m_isScanning = false;
        auto firstSong = m_mediaController.findFirstValidAudio(m_mediaController.getRootNode().get());
        m_mediaController.setNowPlayingSong(firstSong);
        m_mediaController.pause();
        // m_mediaController.play();
    }
}

void UIController::checkAndUpdateTimeState()
{
    if (m_isSeeking)
        return;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastSeekRequestTime < 300)
        return;

    qint64 activePos = m_mediaController.getCurrentPosMicroseconds();
    qint64 totalDuration = m_mediaController.getDurationMicroseconds();
    qint64 remainingMicrosecs = std::max((qint64)0, totalDuration - activePos);

    QString newCurrentPosText = formatTime(activePos);
    QString newRemainingTimeText = formatTime(remainingMicrosecs);

    if (m_currentPosText != newCurrentPosText)
    {
        m_currentPosText = newCurrentPosText;
        emit currentPosTextChanged();
    }
    if (m_remainingTimeText != newRemainingTimeText)
    {
        m_remainingTimeText = newRemainingTimeText;
        emit remainingTimeTextChanged();
    }
    if (m_currentPosMicrosec != activePos)
    {
        m_currentPosMicrosec = activePos;
        emit currentPosMicrosecChanged();
    }
}

void UIController::checkAndUpdatePlayState()
{
    bool currentIsPlaying = m_mediaController.getIsPlaying();
    if (m_isPlaying != currentIsPlaying)
    {
        m_isPlaying = currentIsPlaying;
        emit isPlayingChanged();
    }
}

void UIController::checkAndUpdateVolumeState()
{
    double currentVolume = m_mediaController.getVolume();
    if (std::abs(m_volume - currentVolume) > 0.001)
    {
        m_volume = currentVolume;
        emit volumeChanged();
    }
}

void UIController::checkAndUpdateShuffleState()
{
    bool currentShuffle = m_mediaController.getShuffle();
    if (m_isShuffle != currentShuffle)
    {
        m_isShuffle = currentShuffle;
        emit isShuffleChanged();
    }
}

void UIController::updateStateFromController()
{
    checkAndUpdateScanState();

    // [新增] 检查输出模式
    checkAndUpdateOutputMode();

    PlaylistNode *currentNode = m_mediaController.getCurrentPlayingNode();
    bool isSongChanged = false;

    if (currentNode != m_lastPlayingNode)
    {
        m_lastPlayingNode = currentNode;
        isSongChanged = true;

        m_currentPosMicrosec = 0;
        emit currentPosMicrosecChanged();

        QString zeroTime = "00:00";
        if (m_currentPosText != zeroTime)
        {
            m_currentPosText = zeroTime;
            emit currentPosTextChanged();
        }

        checkAndUpdateCoverArt(currentNode);
        generateWaveformForNode(currentNode);
    }

    checkAndUpdatePlayState();

    if (!isSongChanged)
    {
        checkAndUpdateTimeState();
    }
}

void UIController::updateVolumeState()
{
    checkAndUpdateVolumeState();
    checkAndUpdateShuffleState();
    checkAndUpdateRepeatModeState();
}