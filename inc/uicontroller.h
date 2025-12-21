#ifndef UICONTROLLER_H
#define UICONTROLLER_H

#include "MediaController.hpp"

class UIController : public QObject
{
    Q_OBJECT

    // --- 核心属性 ---
    Q_PROPERTY(QString defaultMusicPath READ defaultMusicPath CONSTANT)
    Q_PROPERTY(bool isScanning READ isScanning NOTIFY isScanningChanged FINAL)

    // --- 播放信息 ---
    Q_PROPERTY(QString coverArtSource READ coverArtSource NOTIFY coverArtSourceChanged FINAL)
    Q_PROPERTY(QString songTitle READ songTitle NOTIFY songTitleChanged FINAL)
    Q_PROPERTY(QString artistName READ artistName NOTIFY artistNameChanged FINAL)
    Q_PROPERTY(QString albumName READ albumName NOTIFY albumNameChanged FINAL)

    // --- 时间与进度 ---
    Q_PROPERTY(QString currentPosText READ currentPosText NOTIFY currentPosTextChanged FINAL)
    Q_PROPERTY(QString remainingTimeText READ remainingTimeText NOTIFY remainingTimeTextChanged FINAL)
    Q_PROPERTY(qint64 totalDurationMicrosec READ totalDurationMicrosec NOTIFY totalDurationMicrosecChanged FINAL)
    Q_PROPERTY(qint64 currentPosMicrosec READ currentPosMicrosec NOTIFY currentPosMicrosecChanged FINAL)
    Q_PROPERTY(bool isSeeking READ isSeeking WRITE setIsSeeking NOTIFY isSeekingChanged FINAL)

    // --- 视觉效果 ---
    Q_PROPERTY(QString gradientColor1 READ gradientColor1 NOTIFY gradientColorsChanged FINAL)
    Q_PROPERTY(QString gradientColor2 READ gradientColor2 NOTIFY gradientColorsChanged FINAL)
    Q_PROPERTY(QString gradientColor3 READ gradientColor3 NOTIFY gradientColorsChanged FINAL)
    Q_PROPERTY(QVariantList waveformHeights READ waveformHeights NOTIFY waveformHeightsChanged FINAL)
    Q_PROPERTY(int waveformBarWidth READ waveformBarWidth NOTIFY waveformHeightsChanged FINAL)

    // --- 播放控制状态 ---
    Q_PROPERTY(bool isPlaying READ getIsPlaying NOTIFY isPlayingChanged FINAL)
    Q_PROPERTY(double volume READ getVolume NOTIFY volumeChanged FINAL)
    Q_PROPERTY(bool isShuffle READ isShuffle WRITE setShuffle NOTIFY isShuffleChanged FINAL)
    Q_PROPERTY(int repeatMode READ getRepeatMode NOTIFY repeatModeChanged FINAL)
    Q_PROPERTY(int outputMode READ outputMode WRITE setOutputMode NOTIFY outputModeChanged FINAL)

public:
    explicit UIController(QObject *parent = nullptr);
    ~UIController();

    Q_INVOKABLE void startMediaScan(const QString &path);

    // Getters
    QString defaultMusicPath() const;
    bool isScanning() const;
    QString coverArtSource() const;
    QString songTitle() const;
    QString artistName() const;
    QString albumName() const;
    QString currentPosText() const;
    QString remainingTimeText() const;
    qint64 totalDurationMicrosec() const;
    qint64 currentPosMicrosec() const;
    QString gradientColor1() const;
    QString gradientColor2() const;
    QString gradientColor3() const;
    bool getIsPlaying() const;
    double getVolume() const;
    bool isShuffle() const
    {
        return m_isShuffle;
    }
    bool isSeeking() const
    {
        return m_isSeeking;
    }
    void setIsSeeking(bool newIsSeeking);
    int getRepeatMode() const;
    QVariantList waveformHeights() const
    {
        return m_waveformHeights;
    }
    int waveformBarWidth() const
    {
        return m_waveformBarWidth;
    }
    int outputMode() const;

    // 混音与输出配置
    Q_INVOKABLE void applyMixingParams(int sampleRate, int formatIndex);
    Q_INVOKABLE QVariantMap getCurrentDeviceParams();
    void prepareForQuit();

signals:
    void isScanningChanged(bool isScanning);
    void scanCompleted();
    void coverArtSourceChanged();
    void songTitleChanged();
    void artistNameChanged();
    void albumNameChanged();
    void currentPosTextChanged();
    void remainingTimeTextChanged();
    void totalDurationMicrosecChanged();
    void currentPosMicrosecChanged();
    void gradientColorsChanged();
    void isPlayingChanged();
    void volumeChanged();
    void isShuffleChanged();
    void isSeekingChanged();
    void repeatModeChanged();
    void waveformHeightsChanged();
    void outputModeChanged();
    void mixingParamsApplied(int actualSampleRate, int actualFormatIndex);

public slots:
    void updateStateFromController(); // 主定时器回调
    void updateVolumeState();         // 音量定时器回调

    Q_INVOKABLE void playpluse();
    Q_INVOKABLE void next();
    Q_INVOKABLE void prev();
    Q_INVOKABLE void seek(qint64 pos_microsec);
    Q_INVOKABLE void setVolume(double volume);
    void setShuffle(bool newShuffle);
    Q_INVOKABLE void toggleRepeatMode();
    void setOutputMode(int mode);
    void onWaveformCalculationFinished();

private:
    MediaController &m_mediaController;
    QTimer m_stateTimer;
    QTimer m_volumeTimer;
    QString m_defaultPath;
    bool m_isScanning = false;

    // 状态更新辅助
    void checkAndUpdateCoverArt(PlaylistNode *currentNode);
    void checkAndUpdateScanState();
    void checkAndUpdateTimeState();
    void updateGradientColors(const QString &imagePath);
    void checkAndUpdatePlayState();
    void checkAndUpdateVolumeState();
    void checkAndUpdateShuffleState();
    void checkAndUpdateRepeatModeState();
    void checkAndUpdateOutputMode();
    void generateWaveformForNode(PlaylistNode *node);

    // 格式转换
    AVSampleFormat indexToAvFormat(int index);
    int avFormatToIndex(AVSampleFormat fmt);

    // 缓存变量 (用于比较变更)
    QString m_coverArtSource;
    PlaylistNode *m_lastPlayingNode = nullptr;
    QString m_songTitle;
    QString m_artistName;
    QString m_albumName;
    QString m_currentPosText = "00:00";
    QString m_remainingTimeText = "00:00";
    qint64 m_totalDurationMicrosec = 0;
    qint64 m_currentPosMicrosec = 0;
    QString m_gradientColor1 = "#232323";
    QString m_gradientColor2 = "#1a1a1a";
    QString m_gradientColor3 = "#121212";
    bool m_isPlaying = false;
    double m_volume = 1.0;
    bool m_isShuffle = false;
    bool m_isSeeking = false;
    qint64 m_lastSeekRequestTime = 0;
    int m_repeatMode = 0;
    int m_outputMode = 0;

    // 波形数据
    QVariantList m_waveformHeights;
    int m_waveformBarWidth = 4;

    // 异步波形生成结构
    struct AsyncWaveformResult
    {
        quint64 generationId;
        QString filePath;
        std::vector<int> heights;
        int barWidth;
    };
    QFutureWatcher<AsyncWaveformResult> m_waveformWatcher;
    quint64 m_currentWaveformGeneration = 0;

    bool m_hasLoadedInitialData = false;
};

#endif // UICONTROLLER_H