#ifndef __PRECOMPILED_H__
#define __PRECOMPILED_H__

// Standard C++ Libraries
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <stack>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <unordered_set>
#include <print>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <array>
#include <future>
#include <cmath>
#include <random>

// ================= TagLib Headers =================
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tfile.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/asffile.h>
#include <taglib/asftag.h>
#include <taglib/asfpicture.h>
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
#include <taglib/dsffile.h>

// ffmpeg headers
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

// stb single headers
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

// sdl2
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_stdinc.h>

// uchardet
#include <uchardet.h>

#include "miniaudio.h"

#define USE_QT_

#ifdef USE_QT_

#include <QQuickImageProvider>
#include <QImage>
#include <QSize>
#include <QDebug>
#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QUrl>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariant>
#include <QDir>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDateTime>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQuickControls2/QQuickStyle>
#include <QtGlobal>
#include <QCommandLineParser>
#include <QFileInfo>
#include <qdebug.h>
#include <qcolor.h>
#include <qimage.h>

#endif

#endif