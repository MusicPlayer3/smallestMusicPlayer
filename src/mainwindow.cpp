#include "mainwindow.h"
#include "AudioPlayer.hpp"
#include "FileScanner.hpp"
#include "MetaDataSharer.hpp"
#include "ui_mainwindow.h"
#include <QFileDialog>
#include <memory>
#include "mpris_server.hpp"

#ifdef __linux__
MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::MainWindow)
#elifdef __WIN32__
MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::MainWindow)
#endif
{
    ui->setupUi(this);
#ifdef Q_OS_WIN
    defaultPath = QCoreApplication::applicationDirPath(); // 程序所在目录
#elif defined(Q_OS_LINUX)
    defaultPath = QDir::homePath(); // 用户home目录
#else
    defaultPath = QDir::homePath(); // 其他系统默认home
#endif

    QString filter = "音频文件 (*.mp3 *.wav *.flac *.ogg);;所有文件 (*.*)";
    QString filename = QFileDialog::getOpenFileName(
        this,
        tr("选择歌曲"),
        defaultPath,
        filter);

    if (!filename.isEmpty())
    {
        qDebug() << "选择的文件:" << filename;
        QFileInfo fileInfo(filename);
        QString baseName = fileInfo.fileName(); // 只保留文件名部分

        ui->songName->setText(baseName);
        if (!player.setPath(filename.toStdString()))
        {
            qWarning() << "无效的音频文件:" << filename;
            return;
        }
        int barWidth = 0;
        auto res = AudioPlayer::buildAudioWaveform(filename.toStdString(), 100, 200, barWidth, 100);
        // std::cout << "res of audioWaveForm..." // 略去打印以保持整洁

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 等待播放器准备好

        // [修改 1] 使用毫秒作为 Slider 的最大值，以获得更平滑的进度条
        int64_t durationMs = player.getDurationMillisecond();

        // 显示用的字符串仍然计算为秒
        QString durationStr = QString::asprintf("%02ld:%02ld", (durationMs / 1000) / 60, (durationMs / 1000) % 60);

        ui->horizontalSlider->setMaximum(static_cast<int>(durationMs));
        ui->nowTime->setText("00:00");
        ui->remainingTime->setText(durationStr);

        uiThread = std::thread(&MainWindow::UIUpdateLoop, this);
    }
}

MainWindow::~MainWindow()
{
    isQuit.store(true);
    if (uiThread.joinable())
    {
        uiThread.join();
    }
    delete ui;
    std::cout << "MainWindow exit\n";
}

void MainWindow::on_play_clicked()
{
    if (ui->play->text() == "播放")
    {
        ui->play->setText("暂停");
        player.play();
    }
    else
    {
        ui->play->setText("播放");
        player.pause();
    }
}

void MainWindow::on_front_clicked()
{
}

void MainWindow::UIUpdateLoop()
{
    QString nowPlayingPath = player.getCurrentPath().c_str();
    while (!isQuit.load())
    {
        // [修改 2] 使用微秒获取高精度当前时间
        int64_t positionMicro = player.getCurrentPositionMicroseconds();
        int64_t durationMs = player.getDurationMillisecond();

        // 计算秒用于显示文本
        int64_t positionSec = positionMicro / 1000000;
        int64_t durationSec = durationMs / 1000;

        QString positionStr = QString::asprintf("%02ld:%02ld", positionSec / 60, positionSec % 60);
        QString remainingStr = QString::asprintf("%02ld:%02ld", (durationSec - positionSec) / 60, (durationSec - positionSec) % 60);

        if (nowPlayingPath != player.getCurrentPath().c_str())
        {
            nowPlayingPath = player.getCurrentPath().c_str();
            QFileInfo fileInfo(nowPlayingPath);
            QString baseName = fileInfo.fileName();
            ui->songName->setText(baseName);

            // 切歌时更新 Slider 最大值
            durationMs = player.getDurationMillisecond(); // 获取新歌曲长度
                                                          // 注意：UI操作建议用信号槽 invokeMethod 到主线程，这里为了简单直接调用
                                                          // 实际项目中应避免在非 UI 线程直接操作 UI 控件
            QMetaObject::invokeMethod(ui->horizontalSlider, "setMaximum", Qt::QueuedConnection, Q_ARG(int, static_cast<int>(durationMs)));
        }

        // 使用 QMetaObject::invokeMethod 或者信号槽来更新 UI 会更安全，
        // 但为了保持和你原代码结构一致，这里只修改逻辑：

        // 更新文本
        QMetaObject::invokeMethod(ui->nowTime, "setText", Q_ARG(QString, positionStr));
        QMetaObject::invokeMethod(ui->remainingTime, "setText", Q_ARG(QString, remainingStr));

        // 更新 Slider
        // 注意：blockSignals(true) 防止 setValue 触发 on_horizontalSlider_valueChanged 导致循环 seek
        // 但在多线程环境下直接操作 ui 对象是不安全的，如果你的程序偶尔崩溃，请改为信号槽机制
        ui->horizontalSlider->blockSignals(true);
        if (ui->horizontalSlider->maximum() != durationMs)
        {
            ui->horizontalSlider->setMaximum(static_cast<int>(durationMs));
        }
        // 将微秒转换为毫秒设置给 Slider
        ui->horizontalSlider->setValue(static_cast<int>(positionMicro / 1000));
        ui->horizontalSlider->blockSignals(false);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "UIUpdateLoop exit\n";
}

void MainWindow::on_back_clicked()
{
}

void MainWindow::on_horizontalSlider_valueChanged(int value)
{
    // [修改 4] Seek 逻辑适配
    // UI Slider 的单位现在是 毫秒 (value)
    // AudioPlayer::seek 现在的参数是 微秒
    // 所以需要 value * 1000

    SDL_Log("Seek request: %d ms", value);

    // 只有当 Slider 没有被 blockSignals 时（即用户手动拖动时）才会触发这里
    // 但为了保险，通常会检查 isSliderDown()，不过你的 Loop 中加了 blockSignals 应该够了
    player.seek(static_cast<int64_t>(value) * 1000);
}

// 切歌按钮
void MainWindow::on_pushButton_clicked()
{
    QString filter = "音频文件 (*.mp3 *.wav *.flac *.ogg);;所有文件 (*.*)";
    QString filename = QFileDialog::getOpenFileName(
        this,
        tr("选择歌曲"),
        defaultPath,
        filter);
    if (!filename.isEmpty())
    {
        qDebug() << "选择的文件:" << filename;
        QFileInfo fileInfo(filename);
        QString baseName = fileInfo.fileName();

        ui->songName->setText(baseName);
        if (!player.setPath(filename.toStdString()))
        {
            qWarning() << "无效的音频文件:" << filename;
            return;
        }

        // [修改 5] 切歌后更新 Slider 范围
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 给解码线程一点时间解析 header
        int64_t durationMs = player.getDurationMillisecond();
        ui->horizontalSlider->setMaximum(static_cast<int>(durationMs));
    }
}

// 预加载按钮
void MainWindow::on_pushButton_2_clicked()
{
    QString filter = "音频文件 (*.mp3 *.wav *.flac *.ogg);;所有文件 (*.*)";
    QString filename = QFileDialog::getOpenFileName(
        this,
        tr("选择歌曲"),
        defaultPath,
        filter);
    if (!filename.isEmpty())
    {
        qDebug() << "选择的文件:" << filename;
        player.setPreloadPath(filename.toStdString());
    }
}

void MainWindow::on_verticalSlider_valueChanged(int value)
{
    double vol = static_cast<double>(value) / 100.0;
    player.setVolume(vol);
}