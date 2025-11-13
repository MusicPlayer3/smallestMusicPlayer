#ifndef AUDIOPLAYER_HPP
#define AUDIOPLAYER_HPP

// 假设 Precompiled.h 包含了这些
// 如果没有，则需要包含 <string>, <mutex>, <condition_variable>, <thread>,
// <atomic>, <queue>, <memory> 以及 FFmpeg 和 SDL 的头文件。
#include "Precompiled.h"

// (优化) 使用 enum class 替换 C 风格 enum
enum class OutputMode
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

// (优化) 移除 typedef，直接使用 struct
struct AudioFrame
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

    // 禁用拷贝，因为 data 是唯一所有者
    AudioFrame(const AudioFrame &) = delete;
    AudioFrame &operator=(const AudioFrame &) = delete;
};

class AudioPlayer
{
private:
    // --- 路径和线程同步 ---
    mutable std::mutex path1Mutex;
    std::condition_variable path1CondVar;
    std::string currentPath = ""; // 当前播放歌曲的路径

    std::mutex preloadPathMutex;
    std::string preloadPath = ""; // 预加载歌曲的路径

    std::thread decodeThread; // 解码线程
    std::atomic<bool> quitFlag{false};

    // --- 状态 ---
    // (优化) 使用新的 enum class
    std::atomic<OutputMode> outputMode{OutputMode::OUTPUT_MIXING};
    std::atomic<PlayerState> playingState{PlayerState::STOPPED};
    std::atomic<int64_t> seekTarget{0};
    std::atomic<bool> hasPaused{true};
    std::atomic<bool> isFirstPlay{true};
    std::atomic<bool> hasPreloaded{false};

    // --- 音频队列和同步 ---
    std::mutex audioFrameQueueMutex;
    std::condition_variable audioFrameQueueCondVar;
    std::atomic<int> audioFrameQueueMaxSize{1024};
    // (优化) 使用智能指针管理 AudioFrame
    std::queue<std::unique_ptr<AudioFrame>> audioFrameQueue;

    // --- 音频回调状态 ---
#ifdef USE_SDL
    // (优化) 使用智能指针管理当前帧
    std::unique_ptr<AudioFrame> m_currentFrame = nullptr;
    int m_currentFramePos = 0;
#endif

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
    std::atomic<bool> isDeviceOpen{false};
    SDL_AudioSpec deviceSpec;
#ifdef USE_SDL
    SDL_AudioDeviceID m_audioDeviceID = 0;
#endif

    // === FFmpeg 资源封装 ===
    struct AudioStreamSource
    {
        AVFormatContext *pFormatCtx = nullptr;
        AVCodecParameters *pCodecParameters = nullptr;
        AVCodecContext *pCodecCtx = nullptr;
        const AVCodec *pCodec = nullptr;
        int audioStreamIndex = -1;
        SwrContext *swrCtx = nullptr;
        std::string path = ""; // 存储路径用于调试

        AudioStreamSource() = default;
        ~AudioStreamSource()
        {
            free();
        }

        // 禁止拷贝
        AudioStreamSource(const AudioStreamSource &) = delete;
        AudioStreamSource &operator=(const AudioStreamSource &) = delete;

        /**
         * @brief 释放此实例持有的所有 FFmpeg 资源
         */
        void free()
        {
            if (swrCtx)
                swr_free(&swrCtx);
            if (pCodecCtx)
                avcodec_free_context(&pCodecCtx);
            if (pFormatCtx)
                avformat_close_input(&pFormatCtx);
            pFormatCtx = nullptr;
            pCodecCtx = nullptr;
            swrCtx = nullptr;
            pCodecParameters = nullptr;
            pCodec = nullptr;
            audioStreamIndex = -1;
            path = "";
        }

        /**
         * @brief (重构) 初始化解码器
         * @param inputPath 音频文件路径
         * @param errorBuffer 外部错误缓冲区
         * @return true 成功, false 失败
         */
        bool initDecoder(const std::string &inputPath, char *errorBuffer);

        /**
         * @brief (重构) 打开重采样上下文
         * @param deviceParams 目标音频设备参数
         * @param volume 初始音量
         * @param errorBuffer 外部错误缓冲区
         * @return true 成功, false 失败
         */
        bool openSwrContext(const AudioParams &deviceParams, double volume, char *errorBuffer);
    };

    // (优化) 使用智能指针管理 FFmpeg 资源
    std::unique_ptr<AudioStreamSource> m_currentSource = nullptr; // 当前播放资源
    std::unique_ptr<AudioStreamSource> m_preloadSource = nullptr; // 预加载资源

    // --- 私有方法 ---
    void freeResources(); // 释放所有资源
    bool openAudioDevice();

#ifdef USE_SDL
    static void sdl2_audio_callback(void *userdata, Uint8 *stream, int len);
#endif

    // === 线程函数和辅助函数 ===
    void mainDecodeThread();

    /**
     * @brief (新增) 检查并触发预加载
     * @param currentPts 当前播放时间(秒)
     */
    void mainLoop_TriggerPreload(double currentPts);

    /**
     * @brief (新增) 自动计算音频队列目标大小
     * @param frame 解码后的 AVFrame (用于元数据)
     * @param out_bytes_per_sample 重采样后的每样本字节数
     */
    void mainLoop_CalculateQueueSize(AVFrame *frame, int out_bytes_per_sample);

public:
    AudioPlayer();
    ~AudioPlayer();

    static bool isValidAudio(const std::string &path);

    bool setPath1(const std::string &path);
    void setPreloadPath(const std::string &path);

    void play();                // 播放
    void pause();               // 暂停
    void seek(int64_t time);    // 跳转
    void setVolume(double vol); // 设置音量

    void setMixingParameters(const AudioParams &params);
    void setMixingParameters(int sampleRate, AVSampleFormat sampleFormat, uint64_t channelLayout, int channels);
    AudioParams getMixingParameters() const;

    // (优化) 使用新的 enum class
    void setOutputMode(OutputMode mode);

    bool isPlaying() const;
    bool isPaused() const;
    std::string getCurrentPath() const
    {
        std::lock_guard<std::mutex> lock(path1Mutex);
        return currentPath;
    }
    int64_t getNowPlayingTime() const;
    int64_t getAudioLength() const;
};
#endif // AUDIOPLAYER_HPP