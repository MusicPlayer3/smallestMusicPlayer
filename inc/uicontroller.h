#ifndef UICONTROLLER_H
#define UICONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QVariant>         // 用于通用数据类型，虽然推荐使用具体的类型
#include <QDir>             // 用于路径操作
#include <QCoreApplication> // 用于获取程序路径
#include <QStandardPaths>   // 用于获取跨平台默认目录
#include <qcontainerfwd.h>
#include <qtypes.h>
#include <QDateTime>

// 引入 MediaController 的头文件，以便访问其单例
#include "MediaController.hpp"

class UIController : public QObject
{
    Q_OBJECT // 核心宏：启用元对象系统，允许 QML 交互

    // 可爱的QT属性
    // ----1. 扫描音乐文件----
    // 1. 默认路径 (READ ONLY)
    Q_PROPERTY(QString defaultMusicPath READ defaultMusicPath CONSTANT);

    // 2. 扫描状态 (READ + NOTIFY)
    Q_PROPERTY(bool isScanning READ isScanning NOTIFY isScanningChanged FINAL);

    // 3. 专辑的封面Source 绝对路径版本
    Q_PROPERTY(QString coverArtSource READ coverArtSource NOTIFY coverArtSourceChanged FINAL);

    // 4. 歌曲详细信息
    Q_PROPERTY(QString songTitle READ songTitle NOTIFY songTitleChanged FINAL);
    Q_PROPERTY(QString artistName READ artistName NOTIFY artistNameChanged FINAL);
    Q_PROPERTY(QString albumName READ albumName NOTIFY albumNameChanged FINAL);
    Q_PROPERTY(QString currentPosText READ currentPosText NOTIFY currentPosTextChanged FINAL);
    Q_PROPERTY(QString remainingTimeText READ remainingTimeText NOTIFY remainingTimeTextChanged FINAL);

    // 5.进度条相关的信息
    Q_PROPERTY(qint64 totalDurationMicrosec READ totalDurationMicrosec NOTIFY totalDurationMicrosecChanged FINAL);
    Q_PROPERTY(qint64 currentPosMicrosec READ currentPosMicrosec NOTIFY currentPosMicrosecChanged FINAL);
    Q_PROPERTY(bool isSeeking READ isSeeking WRITE setIsSeeking NOTIFY isSeekingChanged FINAL); // 标记用户是否正在拖动进度条

    // 6.我的背景渐变颜色
    Q_PROPERTY(QString gradientColor1 READ gradientColor1 NOTIFY gradientColorsChanged FINAL);
    Q_PROPERTY(QString gradientColor2 READ gradientColor2 NOTIFY gradientColorsChanged FINAL);
    Q_PROPERTY(QString gradientColor3 READ gradientColor3 NOTIFY gradientColorsChanged FINAL);

    // 7.播放状态
    Q_PROPERTY(bool isPlaying READ getIsPlaying NOTIFY isPlayingChanged FINAL);

    // 8.音量
    Q_PROPERTY(double volume READ getVolume NOTIFY volumeChanged FINAL);

    // 9.乱序播放状态
    Q_PROPERTY(bool isShuffle READ isShuffle WRITE setShuffle NOTIFY isShuffleChanged FINAL);

public:
    // 构造函数：初始化时获取 MediaController 单例
    explicit UIController(QObject *parent = nullptr);

    // ----1. 扫描音乐文件----
    // Q_INVOKABLE: 接收 QML 选择的路径，开始扫描
    Q_INVOKABLE void startMediaScan(const QString &path);

    // READ Functions
    QString defaultMusicPath() const;
    bool isScanning() const;

    // Getter
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
    bool isSeeking() const { return m_isSeeking; }
    void setIsSeeking(bool newIsSeeking); // QML 调用的 Setter

signals:
    // ----1. 扫描音乐文件----
    // NOTIFY Signal (Q_PROPERTY 必需)
    void isScanningChanged(bool isScanning);

    // 扫描完成信号 (可选，但更清晰)
    void scanCompleted();

    // 更改音乐封面的信号
    void coverArtSourceChanged();

    // 更改音乐信息的信号
    void songTitleChanged();
    void artistNameChanged();
    void albumNameChanged();

    // 更改音乐时间的型号
    void currentPosTextChanged();
    void remainingTimeTextChanged();

    // 进度条
    void totalDurationMicrosecChanged();
    void currentPosMicrosecChanged();

    // 统一用一个信号通知所有颜色更新
    void gradientColorsChanged();

    // 播放状态
    void isPlayingChanged();

    // 音量
    void volumeChanged();

    // 乱序播放状态改变信号
    void isShuffleChanged();

    void isSeekingChanged();

public slots:
    // 核心：轮询槽，用于在不修改 MediaController 的前提下，获取后端状态
    void updateStateFromController(); // 这个是100ms的

    void updateVolumeState(); // 这个是500ms的

    // 播放状态
    Q_INVOKABLE void playpluse();
    Q_INVOKABLE void next();
    Q_INVOKABLE void prev();

    // 进度条改变方法
    Q_INVOKABLE void seek(qint64 pos_microsec);
    // 音量条改变方法
    Q_INVOKABLE void setVolume(double volume);

    // 乱序播放状态 Setter（用于QML写入）
    void setShuffle(bool newShuffle);

private:
    // 核心：保存 MediaController 单例的引用
    MediaController &m_mediaController;
    QTimer m_stateTimer; // 高频率(100ms)状态轮询定时器

    // 缓存 C++ 状态----1. 扫描音乐文件----
    QString m_defaultPath;
    bool m_isScanning = false;

    // 我们放在轮询里面执行的方法啊
    void checkAndUpdateCoverArt(PlaylistNode *currentNode); // 检测我们的歌曲是否变化,然后会做出更新图像的操作哦
    void checkAndUpdateScanState();                         // 新增：用于轮询 isScanCplt()
    void checkAndUpdateTimeState();                         // 新增：用于轮询 currentPos 和 remainingTime
    void updateGradientColors(const QString &imagePath);    // 新增：颜色提取逻辑
    void checkAndUpdatePlayState();                         // 新增：播放状态
    void checkAndUpdateVolumeState();                       // 新增：音量
    void checkAndUpdateShuffleState();                      // 新增：用于轮询 getShuffle()

    // 专辑封面图
    QString m_coverArtSource;                  // 存储处理后的图片路径 (file://...)
    PlaylistNode *m_lastPlayingNode = nullptr; // 用于比对指针是否变化

    // 音乐信息
    QString m_songTitle;
    QString m_artistName;
    QString m_albumName;

    // 音乐时间
    QString m_currentPosText = "00:00";
    QString m_remainingTimeText = "00:00";

    // 音乐进度条
    qint64 m_totalDurationMicrosec = 0;
    qint64 m_currentPosMicrosec = 0;

    // 背景渐变颜色
    QString m_gradientColor1 = "#7d5a5a";
    QString m_gradientColor2 = "#6b4a4a";
    QString m_gradientColor3 = "#5a3c3c";

    // 播放状态
    bool m_isPlaying = false;

    // 音量
    double m_volume = 1.0;
    QTimer m_volumeTimer;

     // 乱序播放状态缓存
    bool m_isShuffle = false;

    // 正在拖动
    bool m_isSeeking = false;

    qint64 m_lastSeekRequestTime = 0; // 记录上一次 UI 主动请求 Seek 的时间戳
};

#endif // UICONTROLLER_H
