#include "uicontroller.h"
#include "DatabaseService.hpp"
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
#else
    m_defaultPath = QDir::homePath();
#endif

    // 状态轮询：100ms 更新一次 UI 状态
    m_stateTimer.setInterval(100);
    connect(&m_stateTimer, &QTimer::timeout, this, &UIController::updateStateFromController);
    m_stateTimer.start();

    // 音量轮询：500ms (降低频率)
    m_volumeTimer.setInterval(500);
    connect(&m_volumeTimer, &QTimer::timeout, this, &UIController::updateVolumeState);
    m_volumeTimer.start();

    connect(&m_waveformWatcher, &QFutureWatcher<AsyncWaveformResult>::finished,
            this, &UIController::onWaveformCalculationFinished);

    m_outputMode = static_cast<int>(m_mediaController.getOUTPUTMode());
}

UIController::~UIController()
{
    prepareForQuit();
}

void UIController::prepareForQuit()
{
    if (m_stateTimer.isActive())
        m_stateTimer.stop();
    if (m_volumeTimer.isActive())
        m_volumeTimer.stop();
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
        return;
    m_mediaController.setRootPath(path.toStdString());
    m_mediaController.startScan();
    if (!m_isScanning)
    {
        m_isScanning = true;
        emit isScanningChanged(true);
    }
}

void UIController::UpdateLastFolder()
{
    // TODO： 这里需要调用后端实例的方法，从数据库中调用最近播放的文件夹
    auto &db = DatabaseService::instance();
    if (db.isPopulated())
    {
        // 有数据，从数据库中获取最近播放的文件夹
        auto start = std::chrono::high_resolution_clock::now();
        auto rootNode = db.loadFullTree();
        auto end = std::chrono::high_resolution_clock::now();
        if (rootNode)
        {
            MediaController::getInstance().setRootNode(rootNode);
            emit scanCompleted();
            m_hasLoadedInitialData = true;
            m_isScanning = false;
            auto first = m_mediaController.findFirstValidAudio(m_mediaController.getRootNode().get());
            m_mediaController.setNowPlayingSong(first);
            m_mediaController.pause();
            spdlog::info("UIController: UpdateLastFolder called. Loaded data from database in {} ms.", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        }
    }
    else
    {
        // TODO:没有数据，唤出文件夹选择器
    }

    qDebug() << "UIController: UpdateLastFolder called. Ready for AudioPlayer logic.";
}

static QString formatTime(qint64 microsecs)
{
    if (microsecs < 0)
        microsecs = 0;
    qint64 secs = microsecs / 1000000;
    qint64 minutes = secs / 60;
    qint64 seconds = secs % 60;
    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

static double colorDistance(const QColor &c1, const QColor &c2)
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
    // 默认深色系背景
    QList<QColor> palette = {QColor("#232323"), QColor("#1a1a1a"), QColor("#121212")};

    if (!image.isNull())
    {
        // 缩小采样以提高性能 (20x20)
        QImage small = image.scaled(20, 20, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        struct ColorScore
        {
            QColor color;
            double score;
        };
        std::vector<ColorScore> candidates;
        candidates.reserve(400);

        for (int y = 0; y < small.height(); ++y)
        {
            for (int x = 0; x < small.width(); ++x)
            {
                QColor c = small.pixelColor(x, y);
                // 过滤极暗和极亮的颜色
                if (c.lightness() < 20 || c.lightness() > 240)
                    continue;

                // 评分算法：偏好高饱和度
                double score = c.saturationF() * 2.0 + (1.0 - std::abs(c.lightnessF() - 0.5));
                candidates.push_back({c, score});
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b)
                  { return a.score > b.score; });

        palette.clear();
        for (const auto &item : candidates)
        {
            bool isDistinct = true;
            for (const auto &selected : palette)
            {
                if (colorDistance(item.color, selected) < 80.0)
                {
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

        // 补足3色
        while (palette.size() < 3)
        {
            if (!palette.isEmpty())
                palette.append(palette.last().darker(110));
            else
                palette.append(QColor("#2d2d2d"));
        }
    }

    // 颜色归一化：限制饱和度，防止背景过于刺眼
    for (auto &color : palette)
    {
        float h, s, l;
        color.getHslF(&h, &s, &l);
        if (s > 0.4)
            s = 0.4; // 稍微降低饱和度上限
        if (l > 0.5)
            l = 0.5; // 限制亮度
        color = QColor::fromHslF(h, s, l);
    }

    QString c1 = palette.value(0).name();
    QString c2 = palette.value(1).name();
    QString c3 = palette.value(2).name();

    if (m_gradientColor1 != c1 || m_gradientColor2 != c2 || m_gradientColor3 != c3)
    {
        m_gradientColor1 = c1;
        m_gradientColor2 = c2;
        m_gradientColor3 = c3;
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

    // 立即更新 UI 状态，防止进度条跳回
    if (m_currentPosMicrosec != pos_microsec)
    {
        m_currentPosMicrosec = pos_microsec;
        emit currentPosMicrosecChanged();

        QString txt = formatTime(pos_microsec);
        if (m_currentPosText != txt)
        {
            m_currentPosText = txt;
            emit currentPosTextChanged();
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

// Getters implementation
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
int UIController::outputMode() const
{
    return m_outputMode;
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

void UIController::setOutputMode(int mode)
{
    if (mode < 0 || mode > 1)
        return;
    OutputMode newMode = (mode == 0) ? OutputMode::Direct : OutputMode::Mixing;
    m_mediaController.setOUTPUTMode(newMode);

    if (m_outputMode != mode)
    {
        m_outputMode = mode;
        emit outputModeChanged();
    }
}

AVSampleFormat UIController::indexToAvFormat(int index)
{
    switch (index)
    {
    case 0: return AV_SAMPLE_FMT_S16;
    case 1: return AV_SAMPLE_FMT_S32;
    case 2: return AV_SAMPLE_FMT_FLT;
    default: return AV_SAMPLE_FMT_FLT;
    }
}

int UIController::avFormatToIndex(AVSampleFormat fmt)
{
    switch (fmt)
    {
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: return 0;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: return 1;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: return 2;
    default: return 2; // Default to float
    }
}

void UIController::applyMixingParams(int sampleRate, int formatIndex)
{
    AVSampleFormat fmt = indexToAvFormat(formatIndex);
    m_mediaController.setMixingParameters(sampleRate, fmt);
    QTimer::singleShot(500, this, [=, this]()
                       {
        AudioParams p = m_mediaController.getDeviceParameters();
        emit mixingParamsApplied(p.sampleRate, avFormatToIndex(p.sampleFormat)); });
}

QVariantMap UIController::getCurrentDeviceParams()
{
    AudioParams p = m_mediaController.getDeviceParameters();
    QVariantMap map;
    map["sampleRate"] = p.sampleRate;
    map["formatIndex"] = avFormatToIndex(p.sampleFormat);
    return map;
}

void UIController::generateWaveformForNode(PlaylistNode *node)
{
    m_waveformHeights.clear();
    emit waveformHeightsChanged();

    m_currentWaveformGeneration++;
    quint64 genId = m_currentWaveformGeneration;

    if (!node || node->isDir())
        return;

    std::string pathStr = node->getMetaData().getFilePath();
    if (pathStr.empty())
        return;

    QString filePath = QString::fromStdString(pathStr);
    int64_t start = node->getMetaData().getOffset();
    int64_t end = start + node->getMetaData().getDuration();

    // 异步计算
    QFuture<AsyncWaveformResult> future = QtConcurrent::run([=]()
                                                            {
        AsyncWaveformResult res;
        res.generationId = genId;
        res.filePath = filePath;
        
        int barWidth = 0;
        // 生成 70 个柱子，宽 320px
        res.heights = AudioPlayer::buildAudioWaveform(
            filePath.toStdString(), 70, 320, barWidth, 60, start, end);
        res.barWidth = barWidth;
        return res; });

    m_waveformWatcher.setFuture(future);
}

void UIController::onWaveformCalculationFinished()
{
    AsyncWaveformResult res = m_waveformWatcher.result();
    if (res.generationId != m_currentWaveformGeneration)
        return;

    QVariantList list;
    for (int h : res.heights)
        list.append(h);

    m_waveformHeights = list;
    m_waveformBarWidth = res.barWidth;
    emit waveformHeightsChanged();
}

void UIController::checkAndUpdateCoverArt(PlaylistNode *currentNode)
{
    QString newCover = "", newTitle = "", newArtist = "", newAlbum = "";

    if (currentNode)
    {
        auto meta = currentNode->getMetaData();

        // 双重检查：虽然 MediaController 可能已经处理过，但 UI 刷新可能独立触发
        // 如果路径为空，再次尝试提取
        if (meta.getCoverPath().empty())
        {
            std::string path = FileScanner::extractCoverToTempFile(meta);
            if (!path.empty())
            {
                meta.setCoverPath(path);
                currentNode->setMetaData(meta); // 回写缓存
            }
        }

        // 处理封面路径
        QString rawPath = QString::fromStdString(meta.getCoverPath());
        if (!rawPath.isEmpty())
        {
            // 转换为 QML 兼容的 file:// URL
            // 这对于显示本地磁盘上的高清大图至关重要
            newCover = QUrl::fromLocalFile(rawPath).toString();
        }
        else
        {
            // 如果确实没有封面，可以使用默认占位符或保持为空
            newCover = "";
        }

        newTitle = QString::fromStdString(meta.getTitle());
        newArtist = QString::fromStdString(meta.getArtist());
        newAlbum = QString::fromStdString(meta.getAlbum());

        // 更新时长逻辑 (保持不变)
        qint64 dur = m_mediaController.getDurationMicroseconds();
        if (m_totalDurationMicrosec != dur)
        {
            m_totalDurationMicrosec = dur;
            emit totalDurationMicrosecChanged();
        }
    }

    // 仅在变动时发送信号
    if (m_coverArtSource != newCover)
    {
        m_coverArtSource = newCover;
        emit coverArtSourceChanged();

        // 更新背景模糊/渐变色
        if (currentNode)
        {
            updateGradientColors(QString::fromStdString(currentNode->getMetaData().getCoverPath()));
        }
    }

    // 更新文字信息
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

void UIController::checkAndUpdateScanState()
{
    if (!m_isScanning)
        return;
    if (m_mediaController.isScanCplt() && !m_hasLoadedInitialData)
    {
        emit scanCompleted();
        m_hasLoadedInitialData = true;
        m_isScanning = false;
        // 自动加载第一首歌但不播放
        auto first = m_mediaController.findFirstValidAudio(m_mediaController.getRootNode().get());
        m_mediaController.setNowPlayingSong(first);
        m_mediaController.pause();
    }
}

void UIController::checkAndUpdateTimeState()
{
    if (m_isSeeking)
        return;
    if (QDateTime::currentMSecsSinceEpoch() - m_lastSeekRequestTime < 300)
        return;

    qint64 pos = m_mediaController.getCurrentPosMicroseconds();
    qint64 dur = m_mediaController.getDurationMicroseconds();

    if (m_currentPosMicrosec != pos)
    {
        m_currentPosMicrosec = pos;
        emit currentPosMicrosecChanged();
    }

    QString tPos = formatTime(pos);
    if (m_currentPosText != tPos)
    {
        m_currentPosText = tPos;
        emit currentPosTextChanged();
    }

    QString tRem = formatTime(std::max((qint64)0, dur - pos));
    if (m_remainingTimeText != tRem)
    {
        m_remainingTimeText = tRem;
        emit remainingTimeTextChanged();
    }
}

void UIController::checkAndUpdatePlayState()
{
    bool playing = m_mediaController.getIsPlaying();
    if (m_isPlaying != playing)
    {
        m_isPlaying = playing;
        emit isPlayingChanged();
    }
}

void UIController::checkAndUpdateVolumeState()
{
    double vol = m_mediaController.getVolume();
    if (std::abs(m_volume - vol) > 0.001)
    {
        m_volume = vol;
        emit volumeChanged();
    }
}

void UIController::checkAndUpdateShuffleState()
{
    bool shuf = m_mediaController.getShuffle();
    if (m_isShuffle != shuf)
    {
        m_isShuffle = shuf;
        emit isShuffleChanged();
    }
}

void UIController::checkAndUpdateRepeatModeState()
{
    int mode = static_cast<int>(m_mediaController.getRepeatMode());
    if (m_repeatMode != mode)
    {
        m_repeatMode = mode;
        emit repeatModeChanged();
    }
}

void UIController::checkAndUpdateOutputMode()
{
    int mode = static_cast<int>(m_mediaController.getOUTPUTMode());
    if (m_outputMode != mode)
    {
        m_outputMode = mode;
        emit outputModeChanged();
    }
}

void UIController::updateStateFromController()
{
    checkAndUpdateScanState();
    checkAndUpdateOutputMode();

    PlaylistNode *curr = m_mediaController.getCurrentPlayingNode();
    bool songChanged = (curr != m_lastPlayingNode);

    if (songChanged)
    {
        m_lastPlayingNode = curr;
        m_currentPosMicrosec = 0;
        emit currentPosMicrosecChanged();

        m_currentPosText = "00:00";
        emit currentPosTextChanged();

        checkAndUpdateCoverArt(curr);
        generateWaveformForNode(curr);
    }

    checkAndUpdatePlayState();
    if (!songChanged)
        checkAndUpdateTimeState();
}

void UIController::updateVolumeState()
{
    checkAndUpdateVolumeState();
    checkAndUpdateShuffleState();
    checkAndUpdateRepeatModeState();
}