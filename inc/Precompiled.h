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

#include <taglib/tbytevector.h>

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

#include <QMainWindow>

#endif