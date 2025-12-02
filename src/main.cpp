#include <QGuiApplication>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQuickControls2/QQuickStyle>
#include "musiclistmodel.h"
#include "uicontroller.h"
#include "CoverImageProvider.hpp"
#include <QtGlobal>

#if defined(Q_OS_WIN)
Q_DECL_EXPORT int qMain(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));

    // QGuiApplication app(argc, argv);
    QApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    QQmlApplicationEngine engine;

    // 注册 Image Provider
    // Provider ID: "covercache"
    engine.addImageProvider(QStringLiteral("covercache"), new CoverImageProvider);
    // 实例化我的控制器对象, 同时将 C++ 对象暴露给 QML 根上下文
    UIController playerController;
    engine.rootContext()->setContextProperty("playerController", &playerController);

    // 实例化模型并加载初始数据
    MusicListModel *musicModel = new MusicListModel(&app);
    musicModel->loadInitialData();
    engine.rootContext()->setContextProperty("musicListModel", musicModel);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []()
        { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("MusicPlayer", "Main");
    return app.exec();
}
