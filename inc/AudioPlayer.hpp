#ifndef AUDIOPLAYER_HPP
#define AUDIOPLAYER_HPP

#include "Precompiled.h"

enum outputMod : std::uint8_t
{
    OUTPUT_DIRECT,
    OUTPUT_MIXING,
};

enum class PlayerState : std::uint8_t
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
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_FLT;
    AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    int channels = 2;
};

// 音频帧结构体
struct AudioFrame
{
    // 使用 unique_ptr 管理 av_malloc 分配的内存
    std::unique_ptr<uint8_t, void (*)(void *)> data;
    int64_t size;
    double pts;

    AudioFrame() : data(nullptr, av_free), size(0), pts(0.0)
    {
    }
};

class AudioPlayer
{
private:
    // --- 路径和线程同步 ---
    mutable std::mutex pathMutex;
    std::condition_variable pathCondVar;
    std::string currentPath = "";
    std::string preloadPath = "";

    std::thread decodeThread;
    std::atomic<bool> quitFlag{false};

    // --- 状态 ---
    mutable std::mutex stateMutex;
    std::condition_variable stateCondVar;
    std::atomic<outputMod> outputMode{OUTPUT_MIXING};
    // playingState 由 stateMutex 保护
    PlayerState playingState{PlayerState::STOPPED};
    PlayerState oldPlayingState{PlayerState::STOPPED};
    std::atomic<int64_t> seekTarget{0};
    bool isFirstPlay = true;
    std::atomic<bool> hasPreloaded{false};

    // --- 音频队列和同步 ---
    std::mutex audioFrameQueueMutex;
    std::atomic<size_t> audioFrameQueueMaxSize{128};
    std::queue<std::unique_ptr<AudioFrame>> audioFrameQueue;

    // --- 音频回调状态 ---
    std::unique_ptr<AudioFrame> m_currentFrame = nullptr;
    int m_currentFramePos = 0;

    // --- 统计和缓冲大小 ---
    std::atomic<int64_t> totalDecodedBytes{0};
    std::atomic<int64_t> totalDecodedFrames{0};
    std::atomic<bool> hasCalculatedQueueSize{false};

    // --- 播放信息 ---
    std::atomic<int64_t> nowPlayingTime = 0;
    std::atomic<int64_t> audioDuration = 0;
    double volume = 1.0;
    char errorBuffer[AV_ERROR_MAX_STRING_SIZE * 2] = {0};

    // --- 音频设备 ---
    AudioParams mixingParams;
    AudioParams deviceParams;
    SDL_AudioSpec deviceSpec;
    SDL_AudioDeviceID m_audioDeviceID = 0;

    // FFmpeg 资源封装
    struct AudioStreamSource
    {
        AVFormatContext *pFormatCtx = nullptr;
        AVCodecContext *pCodecCtx = nullptr;
        SwrContext *swrCtx = nullptr;
        int audioStreamIndex = -1;
        std::string path = "";

        AudioStreamSource() = default;
        ~AudioStreamSource()
        {
            free();
        }

        // 禁止拷贝，允许移动
        AudioStreamSource(const AudioStreamSource &) = delete;
        AudioStreamSource &operator=(const AudioStreamSource &) = delete;
        AudioStreamSource(AudioStreamSource &&) = default;
        AudioStreamSource &operator=(AudioStreamSource &&) = default;

        void free();
        bool initDecoder(const std::string &inputPath, char *errorBuffer);
        bool openSwrContext(const AudioParams &deviceParams, double volume, char *errorBuffer);
    };

    std::unique_ptr<AudioStreamSource> m_currentSource = nullptr;
    std::unique_ptr<AudioStreamSource> m_preloadSource = nullptr;

    // --- 私有方法 ---
    void freeResources();
    bool openAudioDevice();
    void closeAudioDevice();

    static void sdl2_audio_callback(void *userdata, Uint8 *stream, int len);

    // 解码线程函数
    void mainDecodeThread();
    bool setupDecodingSession(const std::string &path);
    void decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally);
    bool processFrame(AVFrame *frame);
    void triggerPreload(double currentPts);
    void calculateQueueSize(int out_bytes_per_sample);
    bool performSeamlessSwitch();
    void flushQueue();

    void applyFadeOutToLastFrame();

public:
    AudioPlayer();
    ~AudioPlayer();

    static bool isValidAudio(const std::string &path);

    static std::vector<int> buildAudioWaveform(const std::string &filepath,
                                               int barCount,
                                               int totalWidth,
                                               int &barWidth,
                                               int maxHeight);

    bool setPath(const std::string &path);
    void setPreloadPath(const std::string &path);

    void play();
    void pause();
    void seek(int64_t timeMicroseconds);
    void setVolume(double vol);

    void setMixingParameters(const AudioParams &params);
    AudioParams getMixingParameters() const;

    void setOutputMode(outputMod mode);

    bool isPlaying() const;
    const std::string getCurrentPath() const;
    int64_t getNowPlayingTime() const;
    int64_t getAudioDuration() const;

    // 获取当前播放位置（微秒），用于 MPRIS Position
    int64_t getCurrentPositionMicroseconds() const;

    // 获取总时长（毫秒），用于某些UI显示
    int64_t getDurationMillisecond() const;

    // 获取总时长（微秒），用于 MPRIS Metadata mpris:length
    int64_t getDurationMicroseconds() const;
};

#endif // AUDIOPLAYER_HPP