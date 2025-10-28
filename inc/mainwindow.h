#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "AudioPlayer.hpp"
#include <QMainWindow>
#include <thread>

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_play_clicked();

    void on_front_clicked();

    void on_back_clicked();

    void on_horizontalSlider_valueChanged(int value);

    void on_pushButton_clicked();

    void on_pushButton_2_clicked();

private:
    Ui::MainWindow *ui;
    AudioPlayer player;

    QString defaultPath;

    std::thread uiThread;

    void UIUpdateLoop();
};
#endif // MAINWINDOW_H
