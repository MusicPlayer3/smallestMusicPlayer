#ifndef __PRECOMPILED_H__
#define __PRECOMPILED_H__

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

#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>
#include <taglib/tbytevector.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <vector>

#define USE_SDL

#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_stdinc.h>

#endif