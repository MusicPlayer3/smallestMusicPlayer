#include "uicontroller.h"
#include "MediaController.hpp" // 假设 MediaController.h 存在并定义了核心逻辑
#include <QDebug>
#include "PlaylistNode.hpp"

UIController::UIController(QObject *parent) : QObject(parent), m_mediaController(MediaController::getInstance())
{
    // --- 1. 设置音乐默认路径 (模仿 QWidget 示例的跨平台逻辑) ---
    // Q_OS_WIN / Q_OS_LINUX 是 Qt 提供的宏定义，用于判断操作系统
#ifdef Q_OS_WIN
    // Windows: 默认使用用户的 "音乐" 文件夹
    m_defaultPath = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (m_defaultPath.isEmpty()) {
        m_defaultPath = QCoreApplication::applicationDirPath();
    }
#elif defined(Q_OS_LINUX)
  // Linux: 默认使用用户的 "Home" 目录 (或 MusicLocation)
    m_defaultPath = QDir::homePath();
#else
  // 其他系统：使用 Home 目录
    m_defaultPath = QDir::homePath();
#endif
    qDebug() << "Detected Default Music Path:" << m_defaultPath;

    // 初始化轮询定时器
    m_stateTimer.setInterval(200);
    connect(&m_stateTimer, &QTimer::timeout, this, &UIController::updateStateFromController);
    m_stateTimer.start();
    qDebug() << "UIController initialized and monitoring MediaController.";
}

// ====================================================================
// Q_INVOKABLE 实现：开始扫描
// ====================================================================
void UIController::startMediaScan(const QString& path)
{
    QDir dir(path);
    if (!dir.exists()) {
        qWarning() << "Scan aborted: Directory does not exist:" << path;
        return;
    }

    // 1. 调用 MediaController 设置路径和开始扫描
    // MediaController::setRootPath 接受 std::string
    m_mediaController.setRootPath(path.toStdString());
    m_mediaController.startScan();

    qDebug() << "MediaController scan started for:" << path;

    // 2. 更新状态并发出 NOTIFY 信号
    if (!m_isScanning) {
        m_isScanning = true;
        emit isScanningChanged(true);
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

// ====================================================================
// 轮询槽实现 (核心状态同步)
// ====================================================================
void UIController::updateStateFromController()
{
    // **********************************************
    // ** 此处目前为空。当您添加第一个 Q_PROPERTY 时，**
    // ** 我们将在此处实现检查逻辑并发出 NOTIFY 信号。 **
    // **********************************************
}
