#include "mainwindow.h"
#include "AudioPlayer.hpp"
#include "MetaDataSharer.hpp"
#include "ui_mainwindow.h"
#include <QFileDialog>
#include <memory>
#include "mpris_server.hpp"

#ifdef __linux__
MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::MainWindow),sharer(player)
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
        std::cout << "res of audioWaveForm:\n";
        for (const auto &item : res)
        {
            std::cout << item << " ";
        }
        std::cout << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 等待播放器准备好
        int64_t duration = player.getAudioDuration();
        QString durationStr = QString::asprintf("%02ld:%02ld", duration / 60, duration % 60);

        ui->horizontalSlider->setMaximum(player.getAudioDuration());
        ui->nowTime->setText("00:00");
        ui->remainingTime->setText(durationStr);
        uiThread = std::thread(&MainWindow::UIUpdateLoop, this);
#ifdef __linux__
        
#endif
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
        int64_t position = player.getNowPlayingTime();
        int64_t duration = player.getAudioDuration();
        QString positionStr = QString::asprintf("%02ld:%02ld", position / 60, position % 60);
        QString remainingStr = QString::asprintf("%02ld:%02ld", (duration - position) / 60, (duration - position) % 60);
        if (nowPlayingPath != player.getCurrentPath().c_str())
        {
            nowPlayingPath = player.getCurrentPath().c_str();
            QFileInfo fileInfo(nowPlayingPath);
            QString baseName = fileInfo.fileName(); // 只保留文件名部分
            ui->songName->setText(baseName);
        }
        ui->nowTime->setText(positionStr);
        ui->remainingTime->setText(remainingStr);
        ui->horizontalSlider->blockSignals(true);
        ui->horizontalSlider->setMaximum(duration);
        ui->horizontalSlider->setValue(position);
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
    SDL_Log("value change! %d", value);
    player.seek(value);
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
        QString baseName = fileInfo.fileName(); // 只保留文件名部分

        ui->songName->setText(baseName);
        if (!player.setPath(filename.toStdString()))
        {
            qWarning() << "无效的音频文件:" << filename;
            return;
        }
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
