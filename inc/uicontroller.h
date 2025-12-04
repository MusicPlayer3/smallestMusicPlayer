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

// 引入 MediaController 的头文件，以便访问其单例
#include "MediaController.hpp"

class UIController : public QObject
{
    Q_OBJECT // 核心宏：启用元对象系统，允许 QML 交互

    // 可爱的QT属性
    // ----1. 扫描音乐文件----
    // 1. 默认路径 (READ ONLY)
    Q_PROPERTY(QString defaultMusicPath READ defaultMusicPath CONSTANT)

        // 2. 扫描状态 (READ + NOTIFY)
        Q_PROPERTY(bool isScanning READ isScanning NOTIFY isScanningChanged FINAL)

        // 3. 专辑的封面Source 绝对路径版本
        Q_PROPERTY(QString coverArtSource READ coverArtSource NOTIFY coverArtSourceChanged FINAL)

        // 4. 歌曲详细信息
        Q_PROPERTY(QString songTitle READ songTitle NOTIFY songTitleChanged FINAL)
            Q_PROPERTY(QString artistName READ artistName NOTIFY artistNameChanged FINAL)
                Q_PROPERTY(QString albumName READ albumName NOTIFY albumNameChanged FINAL)
                    Q_PROPERTY(QString currentPosText READ currentPosText NOTIFY currentPosTextChanged FINAL)
                        Q_PROPERTY(QString remainingTimeText READ remainingTimeText NOTIFY remainingTimeTextChanged FINAL)

        // 5.进度条相关的信息
        Q_PROPERTY(qint64 totalDurationMicrosec READ totalDurationMicrosec NOTIFY totalDurationMicrosecChanged FINAL)
            Q_PROPERTY(qint64 currentPosMicrosec READ currentPosMicrosec NOTIFY currentPosMicrosecChanged FINAL)


public :
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

    void totalDurationMicrosecChanged();
    void currentPosMicrosecChanged();

private slots:
    // 核心：轮询槽，用于在不修改 MediaController 的前提下，获取后端状态
    void updateStateFromController();

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
};

#endif // UICONTROLLER_H
