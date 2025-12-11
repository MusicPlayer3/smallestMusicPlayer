#ifndef AUDIOPLAYER_HPP
#define AUDIOPLAYER_HPP

#include "Precompiled.h"
#include "miniaudio.h"
#include <vector>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

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
    AVSampleFormat sampleFormat = AV_SAMPLE_FMT_S32;
    AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    int channels = 2;
};

// 音频帧结构体 - 简化内存管理
struct AudioFrame
{
    std::vector<uint8_t> data; // 使用 vector 自动管理内存
    int64_t pts = 0;           // 微秒单位
};

class AudioPlayer
{
public:
    AudioPlayer();
    ~AudioPlayer();

    // 禁止拷贝
    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    // 静态工具
    static bool isValidAudio(const std::string &path);
    /**
     * @brief 生成歌曲波形
     */
    static std::vector<int> buildAudioWaveform(const std::string &filepath,
                                               int barCount,
                                               int totalWidth,
                                               int &barWidth,
                                               int maxHeight,
                                               int64_t startTimeUS,
                                               int64_t endTimeUS);

    // 控制接口
    bool setPath(const std::string &path);
    void setPreloadPath(const std::string &path);
    void play();
    void pause();
    void seek(int64_t timeMicroseconds);
    void setVolume(double vol);

    // 参数设置
    void setMixingParameters(const AudioParams &params);
    AudioParams getMixingParameters() const;
    AudioParams getDeviceParameters() const;
    void setOutputMode(outputMod mode);
    outputMod getOutputMode() const
    {
        return outputMode;
    }

    // 状态查询
    bool isPlaying() const;
    const std::string getCurrentPath() const;
    int64_t getNowPlayingTime() const;              // 微秒
    int64_t getCurrentPositionMicroseconds() const; // 微秒
    int64_t getAudioDuration() const;               // 秒
    int64_t getDurationMillisecond() const;         // 毫秒
    int64_t getDurationMicroseconds() const;        // 微秒

private:
    // --- 内部类 ---
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
        bool initDecoder(const std::string &inputPath, char *errorBuffer);
        bool openSwrContext(const AudioParams &deviceParams, double volume, char *errorBuffer);
    };

    // --- 成员变量 ---

    // 路径与线程控制
    mutable std::mutex pathMutex;
    std::condition_variable pathCondVar;
    std::string currentPath = "";
    std::string preloadPath = "";

    std::thread decodeThread;
    std::atomic<bool> quitFlag{false};

    // 播放状态
    mutable std::mutex stateMutex;
    std::condition_variable stateCondVar;
    std::atomic<outputMod> outputMode{OUTPUT_MIXING};
    PlayerState playingState{PlayerState::STOPPED};
    PlayerState oldPlayingState{PlayerState::STOPPED};
    std::atomic<int64_t> seekTarget{0};
    bool isFirstPlay = true;
    // 解码器时间游标 (微秒)，用于在没有 PTS 时推算时间
    std::atomic<int64_t> m_decoderCursor{0};

    // 预加载控制
    std::atomic<bool> hasPreloaded{false};
    std::unique_ptr<AudioStreamSource> m_currentSource;
    std::unique_ptr<AudioStreamSource> m_preloadSource;

    // 资源保护锁 (新增：防止参数重置时解码线程运行)
    std::mutex decodeMutex;

    // 音频队列 (生产者-消费者)
    std::mutex audioFrameQueueMutex; // 保护 queue 和 currentFrame
    std::atomic<size_t> audioFrameQueueMaxSize{128};
    std::queue<std::shared_ptr<AudioFrame>> audioFrameQueue;

    // 当前正在回调中播放的帧
    std::shared_ptr<AudioFrame> m_currentFrame;
    size_t m_currentFramePos = 0;

    // 统计
    std::atomic<int64_t> totalDecodedBytes{0};
    std::atomic<int64_t> totalDecodedFrames{0};
    std::atomic<bool> hasCalculatedQueueSize{false};

    // 播放信息
    std::atomic<int64_t> nowPlayingTime{0}; // 微秒
    std::atomic<int64_t> audioDuration{0};  // 微秒
    std::atomic<double> volume{1.0};
    char errorBuffer[AV_ERROR_MAX_STRING_SIZE * 2] = {0};

    // Miniaudio 设备
    AudioParams mixingParams;
    AudioParams deviceParams;

    // --- Miniaudio Context ---
    ma_context m_context;
    bool m_contextInited = false;

    ma_device m_device;
    bool m_deviceInited = false;

    // --- 辅助方法 ---
    // 用于从当前 AVFormatContext 提取 "Artist - Title" 字符串
    std::string getCurrentStreamTitle() const;

    // --- 私有方法 ---
    void freeResources();
    bool openAudioDevice();
    void closeAudioDevice();
    void flushQueue();

    // 核心线程逻辑
    void mainDecodeThread();
    bool setupDecodingSession(const std::string &path);
    void handleSeekRequest();
    bool waitForDecodeState();

    // 解码逻辑
    void decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally);
    bool processFrame(AVFrame *frame);
    void triggerPreload(double currentPts);
    void calculateQueueSize(int out_bytes_per_sample);
    bool performSeamlessSwitch();
    void applyFadeOutToLastFrame();

    // Miniaudio 回调
    static void ma_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount);
};

#endif // AUDIOPLAYER_HPP