#ifndef __PRECOMPILED_H__
#define __PRECOMPILED_H__

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <queue>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <stack>

#include <taglib/tbytevector.h>
#include <taglib/tag.h>
#include <taglib/fileref.h>
#include <taglib/tstring.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

#define USE_SDL

#include <SDL2/SDL.h>
#include <SDL2/SDL_stdinc.h>
#include <SDL2/SDL_audio.h>

#ifndef FILESCANNER_TEST // 测试 FileScanner 时不需要 Qt
#include <QMainWindow>
#endif

#endif