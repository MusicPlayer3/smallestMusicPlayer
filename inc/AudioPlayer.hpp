#ifndef AUDIOPLAYER_HPP
#define AUDIOPLAYER_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <queue>
#include <thread>
#include <mutex>
#include <string>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

#include <string>

// #define USE_QT
#define USE_SDL

#ifdef USE_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_stdinc.h>
#include <SDL2/SDL_audio.h>
#endif

enum outputMod
{
    OUTPUT_DIRECT,
    OUTPUT_MIXING,
};

enum class PlayerState
{
    STOPPED,
    PLAYING,
    PAUSED,
    SEEKING,
};

// 重采样器参数
struct AudioParams
{
    int sampleRate = 96000;
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_FLTP;
    AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO; // 通道布局
    int channels = 2;
};

// 音频帧结构体
typedef struct AudioFrame
{
    uint8_t *data;
    int size;
    double pts;
    AudioFrame() :
        data(nullptr), size(0), pts(0.0)
    {
    }
    ~AudioFrame()
    {
        if (data)
            av_free(data);
    }
} AudioFrame;

class AudioPlayer
{
private:
    std::mutex path1Mutex;
    std::condition_variable path1CondVar;
    std::string currentPath = ""; // 当前播放歌曲的路径

    AVFormatContext *pFormatCtx1 = nullptr; // 当前播放歌曲的音频文件格式上下文

    bool hasPreloaded = false;

    AVCodecParameters *pCodecParameters1 = nullptr; // 当前播放歌曲的解码器参数

    AVCodecContext *pCodecCtx1 = nullptr; // 当前播放歌曲的解码器上下文

    const AVCodec *pCodec1 = nullptr; // 当前播放歌曲的解码器

    int audioStreamIndex1 = -1; // 当前播放歌曲的音频流索引

    std::atomic<int64_t> nowPlayingTime = 0; // 当前播放时间

    std::atomic<int64_t> audioDuration = 0; // 音频时长

    SwrContext *swrCtx = nullptr; // 重采样上下文

    std::thread decodeThread; // 解码线程

    std::atomic<bool> quitFlag{false};

    std::atomic<outputMod> outputMode{OUTPUT_MIXING};

    std::atomic<PlayerState> playingState{PlayerState::STOPPED};

    std::atomic<int64_t> seekTarget{0};

    std::mutex audioFrameQueueMutex;
    std::condition_variable audioFrameQueueCondVar;
    // 音频帧队列的最大长度
    std::atomic<int> audioFrameQueueMaxSize{1024};

    std::atomic<int64_t> totalDecodedBytes{0};
    std::atomic<int64_t> totalDecodedFrames{0};
    std::atomic<bool> hasCalculatedQueueSize{false};
    std::queue<AudioFrame *> audioFrameQueue;

    AudioParams mixingParams;

    bool initDecoder();

    bool openAudioDevice();

    void freeffmpegResources();

    double volume = 1.0; // 音量，范围0.0 - 1.0

    std::atomic<bool> hasPaused{true};

    char errorBuffer[AV_ERROR_MAX_STRING_SIZE * 2] = {0};

#ifdef USE_SDL
    static void sdl2_audio_callback(void *userdata, Uint8 *stream, int len);
    SDL_AudioDeviceID m_audioDeviceID = 0; // SDL音频设备ID

#endif

#ifdef USE_QT
#endif

    void mainDecodeThread();

public:
    AudioPlayer();
    static bool isValidAudio(const std::string &path);

    bool setPath1(const std::string &path);

    void play();                // 播放
    void pause();               // 暂停
    void seek(int64_t time);    // 跳转
    void setVolume(double vol); // 设置音量

    void setMixingParameters(const AudioParams &params); // 设置混音参数
    void setMixingParameters(int sampleRate, AVSampleFormat sampleFormat, uint64_t channelLayout, int channels);
    AudioParams getMixingParameters() const; // 获取混音参数

    void setOutputMode(outputMod mode);

    bool isPlaying() const;
    bool isPaused() const;
    int64_t getNowPlayingTime() const;
    int64_t getAudioLength() const;
};
#endif // AUDIOPLAYER_HPP
