#ifndef __PRECOMPILED_H__
#define __PRECOMPILED_H__

// =========================================================
// Windows 特有配置 (必须在任何标准库或 Qt 头文件之前处理)
// =========================================================
#ifdef _WIN32
    // 禁用 Windows 平台特有的 min() 和 max() 宏，防止与 std::min/max 冲突
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    
    // 减少 windows.h 引入的非必要头文件，加快编译速度
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif

    #include <windows.h>
    // 注意：如果有 winsock2.h 需求，必须在 windows.h 之前包含，
    // 但在这个项目中似乎由 Qt 或其他库间接处理了。
#else
    #include <iconv.h>
    #include <errno.h>
#endif

// ================= Standard C++ Libraries =================
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
// 确保 TagLib 在 Qt 之前包含，防止某些类型冲突
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
#include <taglib/unsynchronizedlyricsframe.h>
#ifdef TAGLIB_DSF_FILE_H
#include <taglib/dsffile.h>
#endif

namespace fs = std::filesystem;

// ================= FFmpeg Headers =================
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
}

#include <uchardet/uchardet.h>
#include "miniaudio.h"

// ================= SPDLog =================
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

constexpr std::string_view LOG_NAME = "globalLogger";

// ================= Qt Headers =================
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
#include <QColor>
#include <QFuture>

#endif // __PRECOMPILED_H__