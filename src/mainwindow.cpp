#include "mainwindow.h"
#include "AudioPlayer.hpp"
#include "SDL_log.h"
#include "ui_mainwindow.h"
#include <QFileDialog>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    QString defaultPath;
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
        if (!player.setPath1(filename.toStdString()))
        {
            qWarning() << "无效的音频文件:" << filename;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 等待播放器准备好
        int64_t duration = player.getAudioLength();
        QString durationStr = QString::asprintf("%02ld:%02ld", duration / 60, duration % 60);

        ui->horizontalSlider->setMaximum(player.getAudioLength());
        ui->nowTime->setText("00:00");
        ui->remainingTime->setText(durationStr);
        uiThread = std::thread(&MainWindow::UIUpdateLoop, this);
        uiThread.detach();
    }
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_play_clicked()
{
    if (ui->play->text() == "播放")
    {
        ui->play->setText("暂停");
        player.pause();
    }
    else
    {
        ui->play->setText("播放");
        player.play();
    }
}

void MainWindow::on_front_clicked()
{
}

void MainWindow::UIUpdateLoop()
{
    while (true)
    {
        int64_t position = player.getNowPlayingTime();
        int64_t duration = player.getAudioLength();
        QString positionStr = QString::asprintf("%02ld:%02ld", position / 60, position % 60);
        QString remainingStr = QString::asprintf("%02ld:%02ld", (duration - position) / 60, (duration - position) % 60);

        ui->nowTime->setText(positionStr);
        ui->remainingTime->setText(remainingStr);
        // ui->horizontalSlider->setValue(position);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void MainWindow::on_back_clicked()
{
}

void MainWindow::on_horizontalSlider_valueChanged(int value)
{
    SDL_Log("value change! %d", value);
    player.seek(value);
}
