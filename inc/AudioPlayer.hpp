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
    // --- 路径和线程同步 ---
    std::mutex path1Mutex;
    std::condition_variable path1CondVar;
    std::string currentPath = ""; // 当前播放歌曲的路径

    std::mutex preloadPathMutex;
    std::string preloadPath = ""; // 预加载歌曲的路径

    std::thread decodeThread; // 解码线程
    std::atomic<bool> quitFlag{false};

    // --- 状态 ---
    std::atomic<outputMod> outputMode{OUTPUT_MIXING};
    std::atomic<PlayerState> playingState{PlayerState::STOPPED};
    std::atomic<int64_t> seekTarget{0};
    std::atomic<bool> hasPaused{true};
    std::atomic<bool> isFirstPlay{true};   // (新增) 首次播放标志
    std::atomic<bool> hasPreloaded{false}; // (新增) 预加载完成标志

    // --- 音频队列和同步 ---
    std::mutex audioFrameQueueMutex;
    std::condition_variable audioFrameQueueCondVar;
    std::condition_variable decoderCondVar; // 解码器等待条件
    std::atomic<int> audioFrameQueueMaxSize{1024};
    std::queue<AudioFrame *> audioFrameQueue;

    // --- 音频回调状态 ---
#ifdef USE_SDL
    AudioFrame *m_currentFrame = nullptr;
    int m_currentFramePos = 0;
#endif

    // --- 统计和缓冲大小 ---
    std::atomic<int64_t> totalDecodedBytes{0};
    std::atomic<int64_t> totalDecodedFrames{0};
    std::atomic<bool> hasCalculatedQueueSize{false};

    // --- 播放信息 ---
    std::atomic<int64_t> nowPlayingTime = 0; // 当前播放时间 (秒)
    std::atomic<int64_t> audioDuration = 0;  // 音频时长 (AV_TIME_BASE)
    double volume = 1.0;                     // 音量
    char errorBuffer[AV_ERROR_MAX_STRING_SIZE * 2] = {0};

    // --- 音频设备 ---
    AudioParams mixingParams;
#ifdef USE_SDL
    SDL_AudioDeviceID m_audioDeviceID = 0; // SDL音频设备ID
#endif

    // --- 资源 1 (当前播放) ---
    AVFormatContext *pFormatCtx1 = nullptr;
    AVCodecParameters *pCodecParameters1 = nullptr;
    AVCodecContext *pCodecCtx1 = nullptr;
    const AVCodec *pCodec1 = nullptr;
    int audioStreamIndex1 = -1;
    SwrContext *swrCtx = nullptr; // 重采样上下文

    // --- 资源 2 (预加载) ---
    AVFormatContext *pFormatCtx2 = nullptr;
    AVCodecParameters *pCodecParameters2 = nullptr;
    AVCodecContext *pCodecCtx2 = nullptr;
    const AVCodec *pCodec2 = nullptr;
    int audioStreamIndex2 = -1;
    SwrContext *swrCtx2 = nullptr; // 预加载重采样上下文

    // --- 私有方法 ---
    bool initDecoder();
    bool openAudioDevice();

    // (新增) 预加载方法
    bool initDecoder2();
    bool openSwrContext2();

    // (修改) 资源释放函数
    void freeResources();  // 释放所有资源
    void freeResources1(); // 释放资源 1
    void freeResources2(); // 释放资源 2

#ifdef USE_SDL
    static void sdl2_audio_callback(void *userdata, Uint8 *stream, int len);
#endif

    void mainDecodeThread();

public:
    AudioPlayer();
    ~AudioPlayer();

    static bool isValidAudio(const std::string &path);

    bool setPath1(const std::string &path);
    // (新增) 设置预加载路径
    void setPreloadPath(const std::string &path);

    void play();                // 播放
    void pause();               // 暂停
    void seek(int64_t time);    // 跳转
    void setVolume(double vol); // 设置音量

    void setMixingParameters(const AudioParams &params);
    void setMixingParameters(int sampleRate, AVSampleFormat sampleFormat, uint64_t channelLayout, int channels);
    AudioParams getMixingParameters() const;

    void setOutputMode(outputMod mode);

    bool isPlaying() const;
    bool isPaused() const;
    int64_t getNowPlayingTime() const;
    int64_t getAudioLength() const;
};
#endif // AUDIOPLAYER_HPP