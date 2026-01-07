#include "uicontroller.h"
#include "DatabaseService.hpp"
#include "FileScanner.hpp"
#include "MediaController.hpp"
#include "PlaylistNode.hpp"
#include "AudioPlayer.hpp"
// --- Helper Structures and Functions (Moved out of member functions) ---

namespace {

// 将 ColorScore 移到这里，避免在函数内部定义导致的编译器解析错误
struct ColorScore
{
    QColor color;
    double score;
};

// 辅助函数：计算颜色距离
static double colorDistance(const QColor &c1, const QColor &c2)
{
    long rmean = ((long)c1.red() + (long)c2.red()) / 2;
    long r = (long)c1.red() - (long)c2.red();
    long g = (long)c1.green() - (long)c2.green();
    long b = (long)c1.blue() - (long)c2.blue();
    return std::sqrt((((512 + rmean) * r * r) >> 8) + 4 * g * g + (((767 - rmean) * b * b) >> 8));
}

// 辅助函数：格式化时间
static QString formatTime(qint64 microsecs)
{
    if (microsecs < 0)
        microsecs = 0;
    qint64 secs = microsecs / 1000000;
    qint64 minutes = secs / 60;
    qint64 seconds = secs % 60;
    return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

} // namespace

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

    // 注册监听
    m_mediaController.addListener(this);

    // 初始化一次状态
    m_volume = m_mediaController.getVolume();
    m_isShuffle = m_mediaController.getShuffle();
    m_repeatMode = static_cast<int>(m_mediaController.getRepeatMode());

    onTrackChanged(m_mediaController.getCurrentPlayingNode());

    connect(&m_waveformWatcher, &QFutureWatcher<AsyncWaveformResult>::finished,
            this, &UIController::onWaveformCalculationFinished);

    m_outputMode = static_cast<int>(m_mediaController.getOUTPUTMode());
}

UIController::~UIController()
{
    prepareForQuit();
    // 注销
    m_mediaController.removeListener(this);
}

void UIController::prepareForQuit()
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
            m_mediaController.prepareSong(first);
            spdlog::info("UIController: UpdateLastFolder called. Loaded data from database in {} ms.", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        }
    }
    else
    {
        // TODO:没有数据，唤出文件夹选择器
    }

    qDebug() << "UIController: UpdateLastFolder called. Ready for AudioPlayer logic.";
}

void UIController::updateGradientColors(const QString &imagePath)
{
    if (imagePath.isEmpty())
        return; 

    // 确保 QImage 对象正确构造
    QImage image(imagePath);
    
    // 默认背景色
    QList<QColor> palette;
    palette << QColor("#232323") << QColor("#1a1a1a") << QColor("#121212");

    if (!image.isNull())
    {
        QImage small = image.scaled(20, 20, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        std::vector<ColorScore> candidates;
        candidates.reserve(400);

        for (int y = 0; y < small.height(); ++y)
        {
            for (int x = 0; x < small.width(); ++x)
            {
                QColor c = small.pixelColor(x, y);
                if (c.lightness() < 20 || c.lightness() > 240)
                    continue;

                double score = (c.saturationF() * 2.0) + (1.0 - std::abs(c.lightnessF() - 0.5));
                candidates.push_back({c, score});
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const ColorScore &a, const ColorScore &b)
                  { return a.score > b.score; });

        palette.clear();
        for (const auto &item : candidates)
        {
            bool isDistinct = true;
            for (const QColor &selected : palette)
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

        while (palette.size() < 3)
        {
            if (!palette.isEmpty())
                palette.append(palette.last().darker(110));
            else
                palette.append(QColor("#2d2d2d"));
        }
    }

    // 重新调整亮度饱和度，避免 UI 文字看不清
    for (int i = 0; i < palette.size(); ++i)
    {
        float h, s, l;
        palette[i].getHslF(&h, &s, &l);
        if (s > 0.4)
            s = 0.4;
        if (l > 0.5)
            l = 0.5;
        palette[i] = QColor::fromHslF(h, s, l);
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

void UIController::onPlaybackStateChanged(bool isPlaying)
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
        if (m_isPlaying != isPlaying) {
            m_isPlaying = isPlaying;
            emit isPlayingChanged();
        } });
}

void UIController::onTrackChanged(PlaylistNode *newNode)
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
        PlaylistNode* curr = m_mediaController.getCurrentPlayingNode();
        if (curr != m_lastPlayingNode) {
            m_lastPlayingNode = curr;
            
            m_currentPosMicrosec = 0;
            emit currentPosMicrosecChanged();
            m_currentPosText = "00:00";
            emit currentPosTextChanged();

            checkAndUpdateCoverArt(curr);
            generateWaveformForNode(curr);
        } });
}

void UIController::onPositionChanged(int64_t microsec)
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
        if (m_isSeeking) return; 
        
        if (m_currentPosMicrosec != microsec) {
            m_currentPosMicrosec = microsec;
            emit currentPosMicrosecChanged();
        }

        QString tPos = formatTime(microsec);
        if (m_currentPosText != tPos) {
            m_currentPosText = tPos;
            emit currentPosTextChanged();
        }
        
        qint64 total = m_totalDurationMicrosec;
        QString tRem = formatTime(std::max((qint64)0, total - microsec));
        if (m_remainingTimeText != tRem) {
            m_remainingTimeText = tRem;
            emit remainingTimeTextChanged();
        } });
}

void UIController::onVolumeChanged(double volume)
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
        if (std::abs(m_volume - volume) > 0.001) {
            m_volume = volume;
            emit volumeChanged();
        } });
}

void UIController::onShuffleChanged(bool shuffle)
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
        if (m_isShuffle != shuffle) {
            m_isShuffle = shuffle;
            emit isShuffleChanged();
        } });
}

void UIController::onRepeatModeChanged(RepeatMode mode)
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
        int m = static_cast<int>(mode);
        if (m_repeatMode != m) {
            m_repeatMode = m;
            emit repeatModeChanged();
        } });
}

void UIController::onMetadataChanged(PlaylistNode *node)
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
        if (node == m_lastPlayingNode) {
            checkAndUpdateCoverArt(node); 
        } });
}

void UIController::onScanFinished()
{
    QMetaObject::invokeMethod(this, [=, this]()
                              {
                                  if (m_isScanning)
                                  {
                                      m_isScanning = false;
                                      emit isScanningChanged(false);
                                  }

                                  emit scanCompleted();

                                  m_hasLoadedInitialData = true;

                                  auto first = m_mediaController.findFirstValidAudio(m_mediaController.getRootNode().get());
                                  if (first)
                                  {
                                      m_mediaController.prepareSong(first);
                                      onTrackChanged(first);
                                  } });
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
    default: return 2;
    }
}

void UIController::applyMixingParams(int sampleRate, int formatIndex)
{
    AVSampleFormat fmt = indexToAvFormat(formatIndex);
    m_mediaController.setMixingParameters(sampleRate, fmt);
    QTimer::singleShot(500, this, [=, this]()
                       {
        AudioParams p = m_mediaController.getDeviceParameters();
        emit mixingParamsApplied(p.sample_rate, avFormatToIndex(p.fmt)); });
}

QVariantMap UIController::getCurrentDeviceParams()
{
    AudioParams p = m_mediaController.getDeviceParameters();
    QVariantMap map;
    map["sampleRate"] = p.sample_rate;
    map["formatIndex"] = avFormatToIndex(p.fmt);
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

    // Async computation
    QFuture<AsyncWaveformResult> future = QtConcurrent::run([=]()
                                                            {
        AsyncWaveformResult res;
        res.generationId = genId;
        res.filePath = filePath;
        
        int barWidth = 0;
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

        if (meta.getCoverPath().empty())
        {
            std::string path = FileScanner::extractCoverToTempFile(meta);
            if (!path.empty())
            {
                meta.setCoverPath(path);
                currentNode->setMetaData(meta); // write back
            }
        }

        QString rawPath = QString::fromStdString(meta.getCoverPath());
        if (!rawPath.isEmpty())
        {
            newCover = QUrl::fromLocalFile(rawPath).toString();
        }
        else
        {
            newCover = "";
        }

        newTitle = QString::fromStdString(meta.getTitle());
        newArtist = QString::fromStdString(meta.getArtist());
        newAlbum = QString::fromStdString(meta.getAlbum());

        qint64 dur = m_mediaController.getDurationMicroseconds();
        if (m_totalDurationMicrosec != dur)
        {
            m_totalDurationMicrosec = dur;
            emit totalDurationMicrosecChanged();
        }
    }

    if (m_coverArtSource != newCover)
    {
        m_coverArtSource = newCover;
        emit coverArtSourceChanged();

        if (currentNode)
        {
            updateGradientColors(QString::fromStdString(currentNode->getMetaData().getCoverPath()));
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

void UIController::checkAndUpdateScanState()
{
    if (!m_isScanning)
        return;
    if (m_mediaController.isScanCplt() && !m_hasLoadedInitialData)
    {
        emit scanCompleted();
        m_hasLoadedInitialData = true;
        m_isScanning = false;
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