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
#include <taglib/tag.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2frame.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/asffile.h>
#include <taglib/apefile.h>
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
#include <taglib/opusfile.h>
#include <taglib/wavpackfile.h>
#include <taglib/trueaudiofile.h>
#include <taglib/mpcfile.h>
#include <taglib/dsffile.h>
#include <taglib/apefile.h>
#include <taglib/apetag.h>
#include "unsynchronizedlyricsframe.h"
#ifdef TAGLIB_DSF_FILE_H
#include <taglib/dsffile.h>
#endif

// 操作系统原生字符处理头文件
#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#include <errno.h>
#endif

namespace fs = std::filesystem;

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

// uchardet
#include <uchardet.h>

// miniaudio header
#include "miniaudio.h"

// spdlog header
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

constexpr std::string_view LOG_NAME = "globalLogger";

// Qt Headers
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
#include <QtWidgets/QApplication>
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