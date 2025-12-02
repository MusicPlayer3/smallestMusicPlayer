#ifndef UICONTROLLER_H
#define UICONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QVariant> // 用于通用数据类型，虽然推荐使用具体的类型
#include <QDir>             // 用于路径操作
#include <QCoreApplication> // 用于获取程序路径
#include <QStandardPaths>   // 用于获取跨平台默认目录

// 引入 MediaController 的头文件，以便访问其单例
#include "MediaController.hpp"

class UIController : public QObject
{
    Q_OBJECT // 核心宏：启用元对象系统，允许 QML 交互

    // ----1. 扫描音乐文件----
    // 1. 默认路径 (READ ONLY)
    Q_PROPERTY(QString defaultMusicPath READ defaultMusicPath CONSTANT)

    // 2. 扫描状态 (READ + NOTIFY)
    Q_PROPERTY(bool isScanning READ isScanning NOTIFY isScanningChanged FINAL)

public:
    // 构造函数：初始化时获取 MediaController 单例
    explicit UIController(QObject *parent = nullptr);

    // ----1. 扫描音乐文件----
    // Q_INVOKABLE: 接收 QML 选择的路径，开始扫描
    Q_INVOKABLE void startMediaScan(const QString& path);

    // READ Functions
    QString defaultMusicPath() const;
    bool isScanning() const;

signals:
    // ----1. 扫描音乐文件----
    // NOTIFY Signal (Q_PROPERTY 必需)
    void isScanningChanged(bool isScanning);

    // 扫描完成信号 (可选，但更清晰)
    void scanCompleted();


private slots:
    // 核心：轮询槽，用于在不修改 MediaController 的前提下，获取后端状态
    void updateStateFromController();

private:
    // 核心：保存 MediaController 单例的引用
    MediaController& m_mediaController;
    QTimer m_stateTimer;                // 状态轮询定时器

    // 缓存 C++ 状态----1. 扫描音乐文件----
    QString m_defaultPath;
    bool m_isScanning = false;
};

#endif // UICONTROLLER_H
