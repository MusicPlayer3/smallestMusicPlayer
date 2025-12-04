#include "uicontroller.h"
#include "MediaController.hpp" // 假设 MediaController.h 存在并定义了核心逻辑
#include <QDebug>
#include <QUrl>
#include <algorithm>
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
    m_stateTimer.setInterval(200);
    connect(&m_stateTimer, &QTimer::timeout, this, &UIController::updateStateFromController);
    m_stateTimer.start();

    // --- 2. 高频轮询超级机器! 请有需求的Connect一下喵 ---
    m_stateTimer.setInterval(100); // 100ms 检测一次，保证切歌封面更新及时
    connect(&m_stateTimer, &QTimer::timeout, this, &UIController::updateStateFromController);
    m_stateTimer.start();

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
        std::string pathStr = currentNode->getMetaData().getCoverPath();

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

void UIController::checkAndUpdateScanState() // 这里是一个测试函数,用于将第一首歌放到我的播放器里面捏,后面可能可以给ListView
{
    // 1. 只有在 UIController 认为当前正在扫描时，才轮询后端
    if (!m_isScanning)
    {
        return;
    }

    // 2. 调用 MediaController 后端方法轮询状态
    bool scanCplt = m_mediaController.isScanCplt();

    if (scanCplt)
    {
        // ** 扫描已完成！ **

        // 3. 更新 UIController 的缓存状态
        m_isScanning = false;

        // 通知 QML 任务完成，可以执行后续操作（例如，开始播放/跳转主页）
        // emit scanCompleted();

        qDebug() << "Media Scan Completed. Ready to Play or navigate.";

        m_mediaController.play();
    }
}

void UIController::checkAndUpdateTimeState() // 这里是轮询我的剩余时间
{
    // 1. 获取后端原始数据 (微秒)
    qint64 currentPos = m_mediaController.getCurrentPosMicroseconds();
    qint64 totalDuration = m_mediaController.getDurationMicroseconds();

    // 2. 计算剩余时间
    qint64 remainingMicrosecs = std::max((qint64)0, totalDuration - currentPos);

    // 3. 格式化为 UI 文本
    QString newCurrentPosText = formatTime(currentPos);
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

    // --- 这里是进度条的做法

    if (m_totalDurationMicrosec != totalDuration)
    {
        m_totalDurationMicrosec = totalDuration;
        emit totalDurationMicrosecChanged();
    }

    if (m_currentPosMicrosec != currentPos)
    {
        m_currentPosMicrosec = currentPos;
        emit currentPosMicrosecChanged();
    }
}

// 高频轮询槽实现 (核心状态同步)
void UIController::updateStateFromController()
{
    // 1. 扫描状态检测 (新增加的检测)
    checkAndUpdateScanState();

    // ** 切歌检测与封面更新逻辑 **
    // 1. 获取当前后端正在播放的节点指针
    PlaylistNode *currentNode = m_mediaController.getCurrentPlayingNode();

    // 2. 比对指针地址：如果地址变了，说明切歌了（或者从空变成了有歌）
    // 同时初始化操作也是在这一边
    if (currentNode != m_lastPlayingNode)
    {
        // 更新缓存指针
        m_lastPlayingNode = currentNode;
        checkAndUpdateCoverArt(currentNode);
    }

    // 3. 时间状态检测 (100ms 频率执行)
    checkAndUpdateTimeState();
}
