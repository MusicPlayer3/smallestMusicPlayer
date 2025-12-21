#ifndef AUDIOPLAYER_HPP
#define AUDIOPLAYER_HPP

#include "PCH.h"

// 输出模式：直接输出或混合输出
enum class OutputMode : std::uint8_t
{
    Direct, // 根据音频源格式直接输出 (独占模式或硬件透传)
    Mixing, // 统一重采样到指定格式输出 (软件混音)
};

// 播放器状态机
enum class PlayerState : std::uint8_t
{
    Stopped,
    Playing,
    Paused,
    Seeking,
};

// 音频参数：用于配置重采样器或设备
struct AudioParams
{
    int sampleRate = 96000;
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_S32;
    AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO; // 默认立体声
    int channels = 2;
};

// 音频帧：解码后的 PCM 数据单元
struct AudioFrame
{
    std::vector<uint8_t> data; // PCM 数据
    int64_t pts = 0;           // 呈现时间戳 (微秒)
};

// 波形数据结构 (用于内部计算)
struct BarData;

class AudioPlayer
{
public:
    AudioPlayer();
    ~AudioPlayer();

    // 禁止拷贝与赋值，防止资源管理混乱
    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    // --- 静态工具 ---

    // 检查文件是否为有效音频
    static bool isValidAudio(const std::string &path);

    /**
     * @brief 生成音频波形数据 (支持 SIMD 加速)
     * @param filepath 音频文件路径
     * @param barCount 柱状图数量
     * @param totalWidth 总宽度 (像素)
     * @param barWidth 输出的单柱宽度
     * @param maxHeight 最大高度
     * @param startTimeUS 起始时间 (微秒)
     * @param endTimeUS 结束时间 (微秒)
     */
    static std::vector<int> buildAudioWaveform(const std::string &filepath,
                                               int barCount,
                                               int totalWidth,
                                               int &barWidth,
                                               int maxHeight,
                                               int64_t startTimeUS,
                                               int64_t endTimeUS);

    // --- 控制接口 ---

    bool setPath(const std::string &path);
    void setPreloadPath(const std::string &path); // 设置下一首预加载路径
    void play();
    void pause();
    void seek(int64_t timeMicroseconds);
    void setVolume(double vol); // 0.0 - 1.0

    // --- 参数设置 ---

    void setMixingParameters(const AudioParams &params);
    AudioParams getMixingParameters() const;
    AudioParams getDeviceParameters() const;

    void setOutputMode(OutputMode mode);
    OutputMode getOutputMode() const
    {
        return outputMode.load();
    }

    // --- 状态查询 ---

    bool isPlaying() const;
    const std::string getCurrentPath() const;

    // 获取当前播放时间 (微秒)
    int64_t getNowPlayingTime() const;
    int64_t getCurrentPositionMicroseconds() const;

    // 获取总时长
    int64_t getAudioDuration() const;        // 秒
    int64_t getDurationMillisecond() const;  // 毫秒
    int64_t getDurationMicroseconds() const; // 微秒

private:
    // --- 内部类型 ---

    // FFmpeg 资源封装
    struct AudioStreamSource
    {
        AVFormatContext *pFormatCtx = nullptr;
        AVCodecContext *pCodecCtx = nullptr;
        SwrContext *swrCtx = nullptr;
        int audioStreamIndex = -1;
        std::string path;

        AudioStreamSource() = default;
        ~AudioStreamSource()
        {
            free();
        }

        void free();
        bool initDecoder(const std::string &inputPath);
        bool openSwrContext(const AudioParams &deviceParams);
    };

    // --- 成员变量 ---

    // 路径与线程同步
    mutable std::mutex pathMutex;
    std::condition_variable pathCondVar;
    std::string currentPath;
    std::string preloadPath;

    // 解码线程
    std::thread decodeThread;
    std::atomic<bool> quitFlag{false};

    // 播放状态控制
    mutable std::mutex stateMutex;
    std::condition_variable stateCondVar;
    std::atomic<OutputMode> outputMode{OutputMode::Mixing};
    PlayerState playingState{PlayerState::Stopped};
    PlayerState oldPlayingState{PlayerState::Stopped}; // 用于 Seek 后恢复状态

    std::atomic<int64_t> seekTarget{0};
    bool isFirstPlay = true;
    std::atomic<int64_t> m_decoderCursor{0}; // 无 PTS 时的软时钟

    // 预加载机制
    std::atomic<bool> hasPreloaded{false};
    std::unique_ptr<AudioStreamSource> m_currentSource;
    std::unique_ptr<AudioStreamSource> m_preloadSource;

    // 解码保护锁 (防止解码过程中重置上下文)
    std::mutex decodeMutex;

    // 音频帧队列 (生产者-消费者)
    std::mutex audioFrameQueueMutex;
    std::atomic<size_t> audioFrameQueueMaxSize{128};
    std::queue<std::shared_ptr<AudioFrame>> audioFrameQueue;

    // 当前播放帧游标
    std::shared_ptr<AudioFrame> m_currentFrame;
    size_t m_currentFramePos = 0;

    // 统计数据 (用于动态调整队列大小)
    std::atomic<int64_t> totalDecodedBytes{0};
    std::atomic<int64_t> totalDecodedFrames{0};
    std::atomic<bool> hasCalculatedQueueSize{false};

    // 播放信息
    std::atomic<int64_t> nowPlayingTime{0}; // 微秒
    std::atomic<int64_t> audioDuration{0};  // 微秒
    std::atomic<double> volume{1.0};

    // 参数配置
    AudioParams mixingParams;
    AudioParams deviceParams;

    // Miniaudio Context
    ma_context m_context;
    bool m_contextInited = false;
    ma_device m_device;
    bool m_deviceInited = false;

    // --- 私有辅助方法 ---

    // 获取元数据标题
    std::string getCurrentStreamTitle() const;

    // 资源管理
    void freeResources();
    bool openAudioDevice();
    void closeAudioDevice();
    void flushQueue();

    // 线程主逻辑
    void mainDecodeThread();
    bool setupDecodingSession(const std::string &path);
    void handleSeekRequest();
    bool waitForDecodeState();

    // 解码核心
    void decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally);
    bool processFrame(AVFrame *frame);
    void triggerPreload(double currentPts);
    void calculateQueueSize(int out_bytes_per_sample);
    bool performSeamlessSwitch();
    void applyFadeOutToLastFrame();

    // Miniaudio 数据回调 (静态)
    static void ma_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount);
};

#endif // AUDIOPLAYER_HPP