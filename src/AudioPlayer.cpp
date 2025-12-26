#include "AudioPlayer.hpp"
#include "SimpleThreadPool.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <future>
#include <deque>

// --- RAII Wrappers for FFmpeg ---
// 使用智能指针自动管理 FFmpeg 对象的生命周期，防止内存泄漏

struct AVPacketDeleter
{
    void operator()(AVPacket *p) const
    {
        av_packet_free(&p);
    }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

// 【修复 Build 错误】
// 线程池使用 std::bind 时可能会发生参数拷贝。
// std::unique_ptr 不可拷贝，导致 vector<unique_ptr> 拷贝失败。
// 对于需要传入线程池的批处理任务，使用 shared_ptr 替代 unique_ptr。
using AVPacketSharedPtr = std::shared_ptr<AVPacket>;

struct AVFrameDeleter
{
    void operator()(AVFrame *p) const
    {
        av_frame_free(&p);
    }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

// 帮助函数：创建智能指针
static AVPacketPtr make_av_packet()
{
    return AVPacketPtr(av_packet_alloc());
}
static AVFramePtr make_av_frame()
{
    return AVFramePtr(av_frame_alloc());
}

// 帮助函数：创建共享指针 (带自定义删除器)
static AVPacketSharedPtr make_shared_av_packet()
{
    return AVPacketSharedPtr(av_packet_alloc(), [](AVPacket *p)
                             { av_packet_free(&p); });
}

// --- Helper Functions ---

static std::string my_av_strerror(int errnum)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    if (av_strerror(errnum, errbuf, sizeof(errbuf)) == 0)
    {
        return std::string(errbuf);
    }
    return "Unknown error (" + std::to_string(errnum) + ")";
}

namespace
{
constexpr double PRELOAD_TRIGGER_SECONDS_BEFORE_END = 10.0;
constexpr double AUDIO_BUFFER_DURATION_SECONDS = 0.4; // 400ms 缓冲
constexpr int MIN_AUDIO_QUEUE_SIZE = 4;
} // namespace

// 格式转换工具
static ma_format toMaFormat(AVSampleFormat ffmpegFormat)
{
    switch (ffmpegFormat)
    {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P: return ma_format_u8;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: return ma_format_s16;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: return ma_format_s32;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: return ma_format_f32;
    default: return ma_format_f32; // 默认 float
    }
}

static AVSampleFormat toAVSampleFormat(ma_format maFormat)
{
    switch (maFormat)
    {
    case ma_format_u8: return AV_SAMPLE_FMT_U8;
    case ma_format_s16: return AV_SAMPLE_FMT_S16;
    case ma_format_s32: return AV_SAMPLE_FMT_S32;
    case ma_format_f32: return AV_SAMPLE_FMT_FLT;
    default: return AV_SAMPLE_FMT_NONE;
    }
}

static AVChannelLayout toAVChannelLayout(const uint8_t &channels)
{
    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    return layout;
}

// --- AudioStreamSource Implementation ---

void AudioPlayer::AudioStreamSource::free()
{
    if (swrCtx)
    {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }
    if (pCodecCtx)
    {
        avcodec_free_context(&pCodecCtx);
        pCodecCtx = nullptr;
    }
    if (pFormatCtx)
    {
        avformat_close_input(&pFormatCtx);
        pFormatCtx = nullptr;
    }
    audioStreamIndex = -1;
    path.clear();
}

bool AudioPlayer::AudioStreamSource::initDecoder(const std::string &inputPath)
{
    if (inputPath.empty())
        return false;
    path = inputPath;

    if (avformat_open_input(&pFormatCtx, path.c_str(), nullptr, nullptr) != 0)
    {
        spdlog::error("Cannot open audio file: {}", path);
        return false;
    }
    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
    {
        free();
        return false;
    }

    audioStreamIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0)
    {
        free();
        return false;
    }

    AVCodecParameters *pCodecParameters = pFormatCtx->streams[audioStreamIndex]->codecpar;
    const AVCodec *pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if (!pCodec)
    {
        free();
        return false;
    }

    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_parameters_to_context(pCodecCtx, pCodecParameters) < 0)
    {
        free();
        return false;
    }

    // 启用多线程解码 (如果支持)
    pCodecCtx->thread_count = 0; // 0 表示自动检测

    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        free();
        return false;
    }
    return true;
}

bool AudioPlayer::AudioStreamSource::openSwrContext(const AudioParams &deviceParams)
{
    if (!pCodecCtx)
        return false;

    if (swrCtx)
    {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }

    // FFmpeg 新版 API 推荐使用 av_opt_set 配合 swr_alloc
    swrCtx = swr_alloc();
    if (!swrCtx)
        return false;

    av_opt_set_chlayout(swrCtx, "in_chlayout", &(pCodecCtx->ch_layout), 0);
    av_opt_set_int(swrCtx, "in_sample_rate", pCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", pCodecCtx->sample_fmt, 0);

    av_opt_set_chlayout(swrCtx, "out_chlayout", &(deviceParams.ch_layout), 0);
    av_opt_set_int(swrCtx, "out_sample_rate", deviceParams.sampleRate, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", deviceParams.sampleFormat, 0);

    int ret = swr_init(swrCtx);
    if (ret < 0)
    {
        spdlog::error("swr_init failed: {}", my_av_strerror(ret));
        swr_free(&swrCtx);
        return false;
    }
    return true;
}

std::string AudioPlayer::getCurrentStreamTitle() const
{
    if (!m_currentSource || !m_currentSource->pFormatCtx)
    {
        return "AppMusicPlayer";
    }

    AVDictionary *metadata = m_currentSource->pFormatCtx->metadata;
    AVDictionaryEntry *artist = av_dict_get(metadata, "artist", nullptr, 0);
    AVDictionaryEntry *title = av_dict_get(metadata, "title", nullptr, 0);

    if (artist && title)
    {
        return std::string(artist->value) + " - " + std::string(title->value);
    }
    else if (title)
    {
        return std::string(title->value);
    }
    else
    {
        size_t lastSlash = currentPath.find_last_of("/\\");
        if (lastSlash != std::string::npos)
            return currentPath.substr(lastSlash + 1);
        return "AppMusicPlayer";
    }
}

// --- AudioPlayer Implementation ---

AudioPlayer::AudioPlayer()
{
    av_channel_layout_default(&mixingParams.ch_layout, 2);
    // 启动后台解码线程
    decodeThread = std::thread(&AudioPlayer::mainDecodeThread, this);
}

AudioPlayer::~AudioPlayer()
{
    spdlog::info("AudioPlayer: Destructing...");

    quitFlag.store(true);
    pathCondVar.notify_all();
    stateCondVar.notify_all();
    if (decodeThread.joinable())
    {
        decodeThread.join();
    }

    if (m_deviceInited)
    {
        isStopping.store(true);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        ma_device_stop(&m_device);
        ma_device_uninit(&m_device);
        m_deviceInited = false;
    }

    if (m_contextInited)
    {
        ma_context_uninit(&m_context);
        m_contextInited = false;
    }

    freeResources();
    spdlog::info("AudioPlayer: Destruction complete.");
}

void AudioPlayer::setCallbacks(const PlayerCallbacks &callbacks)
{
    m_callbacks = callbacks;
}

void AudioPlayer::freeResources()
{
    // 如果不是 Mixing 模式，清理设备
    if (outputMode.load() != OutputMode::Mixing)
    {
        closeAudioDevice();
    }
    m_currentSource.reset();
    m_preloadSource.reset();
    flushQueue();

    totalDecodedBytes = 0;
    totalDecodedFrames = 0;
    hasCalculatedQueueSize = false;
    hasPreloaded = false;
}

void AudioPlayer::flushQueue()
{
    std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
    // 使用 swap 清空队列，释放 shared_ptr 引用
    std::queue<std::shared_ptr<AudioFrame>> empty;
    std::swap(audioFrameQueue, empty);

    m_currentFrame.reset();
    m_currentFramePos = 0;
}

bool AudioPlayer::isValidAudio(const std::string &path)
{
    // 简单快速检查，不解码
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0)
        return false;

    // 查找音频流
    bool found = (avformat_find_stream_info(fmt, nullptr) >= 0 && av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0);
    avformat_close_input(&fmt);
    return found;
}

bool AudioPlayer::setPath(const std::string &path)
{
    if (!isValidAudio(path))
    {
        spdlog::warn("Invalid audio: {}", path);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = path;
        preloadPath.clear();
        hasPreloaded = false;
        m_preloadSource.reset();
        m_decoderCursor.store(0);
    }

    // 更新状态
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (playingState != PlayerState::Stopped && playingState != PlayerState::Seeking)
        {
            oldPlayingState = playingState;
        }
        else if (playingState == PlayerState::Stopped)
        {
            oldPlayingState = PlayerState::Playing;
        }
        playingState = PlayerState::Stopped;

        // [Callback]
        if (m_callbacks.onStateChanged)
        {
            m_callbacks.onStateChanged(PlayerState::Stopped);
        }
    }

    pathCondVar.notify_one();
    stateCondVar.notify_one();
    return true;
}

void AudioPlayer::setPreloadPath(const std::string &path)
{
    if (!isValidAudio(path))
        return;

    std::lock_guard<std::mutex> lock(pathMutex);
    if (path != preloadPath)
    {
        preloadPath = path;
        // 只有在 Mixing 模式下，我们才能无缝切换并重用输出格式
        if (outputMode.load() == OutputMode::Mixing)
        {
            hasPreloaded = false;
            m_preloadSource.reset();
        }
    }
}

void AudioPlayer::play()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (playingState != PlayerState::Playing)
    {
        isFirstPlay = false;
        playingState = PlayerState::Playing;
        if (m_deviceInited)
        {
            ma_device_start(&m_device);
        }
        stateCondVar.notify_one();

        // [Callback]
        if (m_callbacks.onStateChanged)
        {
            m_callbacks.onStateChanged(PlayerState::Playing);
        }
    }
}

void AudioPlayer::pause()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (playingState != PlayerState::Paused)
    {
        playingState = PlayerState::Paused;
        if (m_deviceInited)
        {
            ma_device_stop(&m_device);
        }
        stateCondVar.notify_one();

        // [Callback]
        if (m_callbacks.onStateChanged)
        {
            m_callbacks.onStateChanged(PlayerState::Paused);
        }
    }
}

void AudioPlayer::seek(int64_t timeMicroseconds)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    seekTarget.store(timeMicroseconds);

    if (playingState != PlayerState::Seeking)
    {
        oldPlayingState = playingState;
    }
    playingState = PlayerState::Seeking;

    // 暂停设备防止爆音或数据错乱
    if (m_deviceInited)
    {
        ma_device_stop(&m_device);
    }
    // 唤醒解码线程处理 Seek
    stateCondVar.notify_one();
}

void AudioPlayer::setVolume(double vol)
{
    volume.store(std::max(0.0, std::min(vol, 1.0)));
    if (m_deviceInited)
    {
        ma_device_set_master_volume(&m_device, (float)volume.load());
    }
}

void AudioPlayer::setMixingParameters(const AudioParams &params)
{
    if (outputMode.load() != OutputMode::Mixing)
        return;

    std::lock_guard<std::mutex> stateLock(stateMutex);
    std::lock_guard<std::mutex> decodeLock(decodeMutex); // 暂停解码活动

    bool wasPlaying = (playingState == PlayerState::Playing);

    if (m_deviceInited)
        ma_device_stop(&m_device);

    flushQueue();
    mixingParams = params;

    // 重建音频设备
    closeAudioDevice();
    if (openAudioDevice())
    {
        if (m_currentSource)
        {
            // 更新 Resampler
            m_currentSource->openSwrContext(deviceParams);
        }
        m_preloadSource.reset(); // 清除预加载，因为格式已变
        hasPreloaded.store(false);

        if (wasPlaying && m_deviceInited)
        {
            ma_device_start(&m_device);
        }
    }
}

void AudioPlayer::setOutputMode(OutputMode mode)
{
    std::lock_guard<std::mutex> stateLock(stateMutex);
    if (outputMode.load() == mode)
        return;

    std::lock_guard<std::mutex> decodeLock(decodeMutex);

    bool wasPlaying = (playingState == PlayerState::Playing);
    if (m_deviceInited)
        ma_device_stop(&m_device);
    flushQueue();

    outputMode.store(mode);
    closeAudioDevice();

    // 尝试重新打开设备
    // Direct: 只有有源时才打开; Mixing: 立即打开
    bool tryOpen = (mode == OutputMode::Mixing) || (m_currentSource != nullptr);

    if (tryOpen && openAudioDevice())
    {
        if (m_currentSource)
        {
            m_currentSource->openSwrContext(deviceParams);
        }
        m_preloadSource.reset();
        hasPreloaded.store(false);

        if (wasPlaying && m_deviceInited)
        {
            ma_device_start(&m_device);
        }
    }
}

AudioParams AudioPlayer::getMixingParameters() const
{
    return mixingParams;
}
AudioParams AudioPlayer::getDeviceParameters() const
{
    return deviceParams;
}

// --- Device Management ---

void AudioPlayer::closeAudioDevice()
{
    if (m_deviceInited)
    {
        ma_device_uninit(&m_device);
        m_deviceInited = false;
    }
    // Context 保留以便重用，析构时清理
}

bool AudioPlayer::openAudioDevice()
{
    ma_format targetFormat;
    ma_uint32 targetChannels;
    ma_uint32 targetSampleRate;
    std::string targetAppName;

    if (outputMode.load() == OutputMode::Mixing)
    {
        targetFormat = toMaFormat(mixingParams.sampleFormat);
        targetChannels = mixingParams.channels;
        targetSampleRate = mixingParams.sampleRate;
        targetAppName = "AppMusicPlayer";
    }
    else // Direct Mode
    {
        if (!m_currentSource || !m_currentSource->pCodecCtx)
            return false;

        targetFormat = toMaFormat(m_currentSource->pCodecCtx->sample_fmt);
        targetChannels = m_currentSource->pCodecCtx->ch_layout.nb_channels;
        targetSampleRate = m_currentSource->pCodecCtx->sample_rate;
        targetAppName = getCurrentStreamTitle();
    }

    // 检查是否无需重建
    if (m_deviceInited)
    {
        if (outputMode.load() == OutputMode::Mixing && m_device.playback.format == targetFormat && m_device.playback.channels == targetChannels && m_device.sampleRate == targetSampleRate)
        {
            return true;
        }
        closeAudioDevice();
    }

    // 重建 Context 以更新 AppName (针对 PulseAudio 等)
    if (m_contextInited)
    {
        ma_context_uninit(&m_context);
        m_contextInited = false;
    }

    ma_context_config ctxConfig = ma_context_config_init();
#ifdef __linux__
    ctxConfig.pulse.pApplicationName = targetAppName.c_str();
#endif
    if (ma_context_init(NULL, 0, &ctxConfig, &m_context) != MA_SUCCESS)
    {
        spdlog::error("Failed to init ma_context");
        return false;
    }
    m_contextInited = true;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.dataCallback = ma_data_callback;
    config.pUserData = this;
    config.playback.format = targetFormat;
    config.playback.channels = targetChannels;
    config.sampleRate = targetSampleRate;

    if (ma_device_init(&m_context, &config, &m_device) != MA_SUCCESS)
    {
        std::cerr << "Failed to open playback device." << std::endl;
        return false;
    }

    m_deviceInited = true;
    deviceParams.sampleRate = m_device.sampleRate;
    deviceParams.sampleFormat = toAVSampleFormat(m_device.playback.format);
    deviceParams.ch_layout = toAVChannelLayout(m_device.playback.channels);
    deviceParams.channels = m_device.playback.channels;

    ma_device_set_master_volume(&m_device, (float)volume.load());
    return true;
}

// --- Miniaudio Callback ---
void AudioPlayer::ma_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(pDevice->pUserData);
    if (!player)
        return;

    int bytesPerSample = av_get_bytes_per_sample(player->deviceParams.sampleFormat);
    int channels = player->deviceParams.channels;
    int totalBytesNeeded = frameCount * channels * bytesPerSample;

    uint8_t *outPtr = static_cast<uint8_t *>(pOutput);
    int bytesWritten = 0;

    // 关键区域：持有锁直到数据填充完毕
    std::lock_guard<std::mutex> lock(player->audioFrameQueueMutex);

    while (bytesWritten < totalBytesNeeded)
    {
        if (!player->m_currentFrame)
        {
            if (player->audioFrameQueue.empty())
            {
                player->stateCondVar.notify_one(); // 通知解码线程产粮
                break;                             // 队列空，填充静音
            }

            player->m_currentFrame = player->audioFrameQueue.front();
            player->audioFrameQueue.pop();
            player->m_currentFramePos = 0;

            if (player->m_currentFrame)
            {
                player->nowPlayingTime.store(player->m_currentFrame->pts);
            }
            player->stateCondVar.notify_one();
        }

        if (!player->m_currentFrame)
            break;

        // 计算当前更精确的时间戳 (插值)
        if (player->deviceParams.sampleRate > 0)
        {
            int64_t bytesPlayed = player->m_currentFramePos;
            int64_t offsetUS = (bytesPlayed * 1000000) / (channels * bytesPerSample * player->deviceParams.sampleRate);
            int64_t currentUS = player->m_currentFrame->pts + offsetUS;
            player->nowPlayingTime.store(currentUS);

            // [Callback] 限频触发
            int64_t now = std::chrono::steady_clock::now().time_since_epoch().count(); // 纳秒
            int64_t last = player->lastCallbackTime.load(std::memory_order_relaxed);
            // 100ms = 100,000,000 ns
            if (now - last > 100000000)
            {
                player->lastCallbackTime.store(now, std::memory_order_relaxed);
                if (player->m_callbacks.onPositionChanged)
                {
                    // 注意：这里是在音频线程，回调必须非常快且线程安全
                    player->m_callbacks.onPositionChanged(currentUS);
                }
            }
        }

        size_t frameSize = player->m_currentFrame->data.size();
        int frameRemaining = static_cast<int>(frameSize - player->m_currentFramePos);
        int bytesNeeded = totalBytesNeeded - bytesWritten;
        int copySize = std::min(frameRemaining, bytesNeeded);

        if (player->isStopping.load())
        {
            // 填充静音数据
            memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
            break;
        }
        else if (copySize > 0)
        {
            memcpy(outPtr + bytesWritten,
                   player->m_currentFrame->data.data() + player->m_currentFramePos,
                   copySize);
            bytesWritten += copySize;
            player->m_currentFramePos += copySize;
        }

        if (player->m_currentFramePos >= frameSize)
        {
            player->m_currentFrame.reset();
        }
    }

    if (bytesWritten < totalBytesNeeded)
    {
        memset(outPtr + bytesWritten, 0, totalBytesNeeded - bytesWritten);
    }
}

// --- Decode Thread Logic ---

void AudioPlayer::mainDecodeThread()
{
    // RAII Packet
    AVPacketPtr packet = make_av_packet();

    while (!quitFlag.load())
    {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(pathMutex);
            pathCondVar.wait(lock, [this]
                             { return !currentPath.empty() || quitFlag.load(); });
            if (quitFlag.load())
                break;
            path = currentPath;
        }

        if (setupDecodingSession(path))
        {
            // 恢复播放状态
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (playingState == PlayerState::Stopped)
                {
                    playingState = isFirstPlay ? PlayerState::Paused : oldPlayingState;
                }

                if (m_deviceInited)
                {
                    if (playingState == PlayerState::Playing)
                        ma_device_start(&m_device);
                    else
                        ma_device_stop(&m_device);
                }
            }

            bool isSongLoopActive = true;
            bool playbackFinishedNaturally = false;

            while (isSongLoopActive)
            {
                if (!waitForDecodeState())
                {
                    isSongLoopActive = false; // 被停止或退出
                    continue;
                }

                if (playingState == PlayerState::Seeking)
                {
                    handleSeekRequest();
                    continue;
                }

                // 核心解码区，使用 RAII Packet
                {
                    std::lock_guard<std::mutex> decodeLock(decodeMutex);
                    decodeAndProcessPacket(packet.get(), isSongLoopActive, playbackFinishedNaturally);
                }
            }

            freeResources();
            if (playbackFinishedNaturally)
            {
                std::lock_guard<std::mutex> lock(pathMutex);
                currentPath.clear();
                preloadPath.clear();
            }
        }
        else
        {
            freeResources();
            std::lock_guard<std::mutex> lock(pathMutex);
            currentPath.clear();
        }
    }
}

bool AudioPlayer::waitForDecodeState()
{
    std::unique_lock<std::mutex> lock(stateMutex);
    stateCondVar.wait(lock, [this]
                      {
                          if (quitFlag.load() || playingState == PlayerState::Stopped || playingState == PlayerState::Seeking)
                              return true;
                          if (playingState == PlayerState::Playing)
                          {
                              std::lock_guard<std::mutex> qLock(audioFrameQueueMutex);
                              return audioFrameQueue.size() < audioFrameQueueMaxSize.load();
                          }
                          return false; // Paused 状态下保持等待
                      });

    return !(quitFlag.load() || playingState == PlayerState::Stopped);
}

void AudioPlayer::handleSeekRequest()
{
    flushQueue();
    std::lock_guard<std::mutex> decodeLock(decodeMutex);

    if (!m_currentSource)
        return;

    int streamIdx = m_currentSource->audioStreamIndex;
    if (streamIdx < 0)
        return;

    AVRational tb = m_currentSource->pFormatCtx->streams[streamIdx]->time_base;
    int64_t target = seekTarget.load();
    int64_t stream_ts = av_rescale_q(target, AV_TIME_BASE_Q, tb);

    av_seek_frame(m_currentSource->pFormatCtx, streamIdx, stream_ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_currentSource->pCodecCtx);

    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        playingState = oldPlayingState;
        if (playingState == PlayerState::Playing && m_deviceInited)
        {
            ma_device_start(&m_device);
        }
    }
    m_decoderCursor.store(target);
    nowPlayingTime.store(target);
}

void AudioPlayer::decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally)
{
    int ret = av_read_frame(m_currentSource->pFormatCtx, packet);
    if (ret < 0)
    {
        if (ret == AVERROR_EOF)
        {
            // --- 歌曲切换逻辑 ---
            if (outputMode.load() == OutputMode::Mixing)
            {
                if (performSeamlessSwitch())
                    return;
            }
            else if (outputMode.load() == OutputMode::Direct)
            {
                std::string nextPath;
                {
                    std::lock_guard<std::mutex> lock(pathMutex);
                    nextPath = preloadPath;
                }
                if (!nextPath.empty())
                {
                    {
                        std::lock_guard<std::mutex> lock(pathMutex);
                        currentPath = nextPath;
                        preloadPath.clear();
                    }
                    playbackFinishedNaturally = false; // 触发外层重新 setupSession
                    isSongLoopActive = false;
                    return;
                }
            }

            playbackFinishedNaturally = true;
            isSongLoopActive = false;
            if (m_callbacks.onFileComplete)
            {
                m_callbacks.onFileComplete();
            }
        }
        else
        {
            spdlog::error("Read error: {}", my_av_strerror(ret));
            isSongLoopActive = false;
        }
        return;
    }

    if (packet->stream_index == m_currentSource->audioStreamIndex)
    {
        if (avcodec_send_packet(m_currentSource->pCodecCtx, packet) >= 0)
        {
            // 使用 RAII Frame
            AVFramePtr frame = make_av_frame();
            while (avcodec_receive_frame(m_currentSource->pCodecCtx, frame.get()) >= 0)
            {
                if (!processFrame(frame.get()))
                {
                    isSongLoopActive = false;
                    break;
                }
            }
        }
    }
    av_packet_unref(packet);
}

bool AudioPlayer::processFrame(AVFrame *frame)
{
    if (!frame || !m_currentSource)
        return false;

    double frameDurationSec = (double)frame->nb_samples / frame->sample_rate;
    int64_t frameDurationMicro = static_cast<int64_t>(frameDurationSec * 1000000);
    int64_t ptsMicro = 0;

    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
    {
        double ptsSec = frame->best_effort_timestamp * av_q2d(m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base);
        ptsMicro = static_cast<int64_t>(ptsSec * 1000000);
        m_decoderCursor.store(ptsMicro + frameDurationMicro);
    }
    else
    {
        ptsMicro = m_decoderCursor.load();
        m_decoderCursor.fetch_add(frameDurationMicro);
    }

    triggerPreload(static_cast<double>(ptsMicro) / 1000000.0);

    // 重采样计算
    int64_t delay = swr_get_delay(m_currentSource->swrCtx, frame->sample_rate);
    int64_t out_samples = av_rescale_rnd(delay + frame->nb_samples, deviceParams.sampleRate, frame->sample_rate, AV_ROUND_UP);

    auto audioFrame = std::make_shared<AudioFrame>();
    audioFrame->pts = ptsMicro;

    int out_bytes_per_sample = av_get_bytes_per_sample(deviceParams.sampleFormat);
    int out_channels = deviceParams.channels;
    int buffer_size = static_cast<int>(out_samples * out_channels * out_bytes_per_sample);

    audioFrame->data.resize(buffer_size);
    uint8_t *out_ptr = audioFrame->data.data();

    int64_t converted = swr_convert(m_currentSource->swrCtx, &out_ptr, static_cast<int>(out_samples),
                                    (const uint8_t **)frame->data, frame->nb_samples);

    if (converted < 0)
        return false;

    int real_size = static_cast<int>(converted * out_channels * out_bytes_per_sample);
    audioFrame->data.resize(real_size);

    totalDecodedBytes += real_size;
    totalDecodedFrames++;
    calculateQueueSize(out_bytes_per_sample);

    {
        std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
        audioFrameQueue.push(audioFrame);
    }
    return true;
}

bool AudioPlayer::setupDecodingSession(const std::string &path)
{
    m_currentSource = std::make_unique<AudioStreamSource>();
    if (!m_currentSource->initDecoder(path))
        return false;

    if (!openAudioDevice())
        return false;

    // Direct 模式下 deviceParams 会更新，Mixing 模式下使用预设
    if (!m_currentSource->openSwrContext(deviceParams))
        return false;

    audioDuration.store(m_currentSource->pFormatCtx->duration);
    return true;
}

void AudioPlayer::triggerPreload(double currentPts)
{
    if (outputMode.load() != OutputMode::Mixing || hasPreloaded.load() || currentPts < 0)
        return;

    std::string pPath;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        pPath = preloadPath;
    }
    if (pPath.empty())
        return;

    double dur = audioDuration.load() / (double)AV_TIME_BASE;
    if (dur > 0 && (dur - currentPts) < PRELOAD_TRIGGER_SECONDS_BEFORE_END)
    {
        auto src = std::make_unique<AudioStreamSource>();
        if (src->initDecoder(pPath) && src->openSwrContext(deviceParams))
        {
            m_preloadSource = std::move(src);
            hasPreloaded.store(true);
            spdlog::debug("Preloading: {}", pPath);
        }
        else
        {
            std::lock_guard<std::mutex> lock(pathMutex);
            preloadPath.clear();
        }
    }
}

void AudioPlayer::calculateQueueSize(int out_bytes_per_sample)
{
    if (hasCalculatedQueueSize.load())
        return;
    if (totalDecodedFrames.load() < 10)
        return;

    int64_t bytes = totalDecodedBytes.load();
    int64_t frames = totalDecodedFrames.load();
    int avgBytes = (frames > 0) ? static_cast<int>(bytes / frames) : 0;
    if (avgBytes == 0)
        return;

    int bytesPerSec = deviceParams.sampleRate * deviceParams.channels * out_bytes_per_sample;
    int targetBytes = static_cast<int>(bytesPerSec * AUDIO_BUFFER_DURATION_SECONDS);
    size_t maxF = std::max(MIN_AUDIO_QUEUE_SIZE, targetBytes / avgBytes);

    audioFrameQueueMaxSize.store(maxF);
    hasCalculatedQueueSize.store(true);
}

bool AudioPlayer::performSeamlessSwitch()
{
    if (outputMode.load() != OutputMode::Mixing || !hasPreloaded.load() || !m_preloadSource)
        return false;

    applyFadeOutToLastFrame();

    // 交换 Source
    m_currentSource = std::move(m_preloadSource);

    std::string newPath;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = m_currentSource->path;
        newPath = currentPath; // 记录新路径用于回调
        preloadPath.clear();
    }

    audioDuration.store(m_currentSource->pFormatCtx->duration);
    hasPreloaded.store(false);
    nowPlayingTime.store(0);
    m_decoderCursor.store(0);

    totalDecodedBytes = 0;
    totalDecodedFrames = 0;
    hasCalculatedQueueSize = false;

    // [新增] 关键：通知上层路径已变更 (在解锁后调用以防死锁)
    if (m_callbacks.onPathChanged)
    {
        m_callbacks.onPathChanged(newPath);
    }

    return true;
}

void AudioPlayer::applyFadeOutToLastFrame()
{
    std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
    if (audioFrameQueue.empty())
        return;

    auto frame = audioFrameQueue.back();
    if (!frame || frame->data.empty())
        return;

    // 简单淡出实现 (仅支持 Float)
    if (deviceParams.sampleFormat == AV_SAMPLE_FMT_FLT)
    {
        float *samples = reinterpret_cast<float *>(frame->data.data());
        size_t count = frame->data.size() / sizeof(float);
        for (size_t i = 0; i < count; ++i)
        {
            float gain = 1.0f - (static_cast<float>(i) / count);
            samples[i] *= gain;
        }
    }
}

// --- Getters ---

bool AudioPlayer::isPlaying() const
{
    std::lock_guard<std::mutex> lock(stateMutex);
    return playingState == PlayerState::Playing;
}
const std::string AudioPlayer::getCurrentPath() const
{
    std::lock_guard<std::mutex> lock(pathMutex);
    return currentPath;
}
int64_t AudioPlayer::getNowPlayingTime() const
{
    return nowPlayingTime.load() / 1000000;
}
int64_t AudioPlayer::getCurrentPositionMicroseconds() const
{
    return nowPlayingTime.load();
}
int64_t AudioPlayer::getAudioDuration() const
{
    return audioDuration.load() / AV_TIME_BASE;
}
int64_t AudioPlayer::getDurationMillisecond() const
{
    return audioDuration.load() / 1000;
}
int64_t AudioPlayer::getDurationMicroseconds() const
{
    return audioDuration.load();
}

// =========================================================
//  STATIC WAVEFORM GENERATION
//  Refactored for maintainability while keeping SIMD perf
// =========================================================

namespace WaveformUtils
{

struct ChunkResult
{
    float sumSquares;
    int actualCount;
};

// --- SIMD Kernels (Internal) ---
// 使用匿名命名空间保持内部链接，防止符号污染

#include <immintrin.h> // Make sure this is available

static ChunkResult computeSumSquaresAVX2_Float(const float *data, int range_count, int step, int decimation)
{
    float sum = 0.0f;
    int processed = 0;
    int i = 0;
    if (step == 1)
    {
        int jump = 8 * decimation;
        __m256 sum_vec = _mm256_setzero_ps();
        for (; i <= range_count - 8; i += jump)
        {
            __m256 vals = _mm256_loadu_ps(data + i);
            sum_vec = _mm256_fmadd_ps(vals, vals, sum_vec);
            processed += 8;
        }
        float temp[8];
        _mm256_storeu_ps(temp, sum_vec);
        for (int k = 0; k < 8; ++k)
            sum += temp[k];
    }
    int scalar_stride = step * decimation;
    for (; i < range_count; i += scalar_stride)
    {
        float val = data[i];
        sum += val * val;
        processed++;
    }
    return {sum, processed};
}

static ChunkResult computeSumSquaresAVX2_S16(const int16_t *data, int range_count, int step, int decimation)
{
    float sum = 0.0f;
    int processed = 0;
    int i = 0;
    const float SCALE_S16 = 1.0f / 32768.0f;
    if (step == 1)
    {
        int jump = 8 * decimation;
        __m256 sum_vec = _mm256_setzero_ps();
        __m256 factor = _mm256_set1_ps(SCALE_S16);
        for (; i <= range_count - 8; i += jump)
        {
            __m128i vals_i16 = _mm_loadu_si128((const __m128i *)(data + i));
            __m256i vals_i32 = _mm256_cvtepi16_epi32(vals_i16);
            __m256 vals_f = _mm256_cvtepi32_ps(vals_i32);
            vals_f = _mm256_mul_ps(vals_f, factor);
            sum_vec = _mm256_fmadd_ps(vals_f, vals_f, sum_vec);
            processed += 8;
        }
        float temp[8];
        _mm256_storeu_ps(temp, sum_vec);
        for (int k = 0; k < 8; ++k)
            sum += temp[k];
    }
    int scalar_stride = step * decimation;
    for (; i < range_count; i += scalar_stride)
    {
        float val = data[i] * SCALE_S16;
        sum += val * val;
        processed++;
    }
    return {sum, processed};
}

static ChunkResult computeSumSquaresAVX2_S32(const int32_t *data, int range_count, int step, int decimation)
{
    float sum = 0.0f;
    int processed = 0;
    int i = 0;
    const float SCALE_S32 = 1.0f / 2147483648.0f;
    if (step == 1)
    {
        int jump = 8 * decimation;
        __m256 sum_vec = _mm256_setzero_ps();
        __m256 factor = _mm256_set1_ps(SCALE_S32);
        for (; i <= range_count - 8; i += jump)
        {
            __m256i vals_i32 = _mm256_loadu_si256((const __m256i *)(data + i));
            __m256 vals_f = _mm256_cvtepi32_ps(vals_i32);
            vals_f = _mm256_mul_ps(vals_f, factor);
            sum_vec = _mm256_fmadd_ps(vals_f, vals_f, sum_vec);
            processed += 8;
        }
        float temp[8];
        _mm256_storeu_ps(temp, sum_vec);
        for (int k = 0; k < 8; ++k)
            sum += temp[k];
    }
    const double SCALE_S32_DBL = 1.0 / 2147483648.0;
    int scalar_stride = step * decimation;
    for (; i < range_count; i += scalar_stride)
    {
        float val = static_cast<float>(data[i] * SCALE_S32_DBL);
        sum += val * val;
        processed++;
    }
    return {sum, processed};
}

static ChunkResult computeSumSquares_U8(const uint8_t *data, int range_count, int step, int decimation)
{
    float sum = 0.0f;
    int processed = 0;
    const float SCALE_U8 = 1.0f / 128.0f;
    int stride = step * decimation;
    for (int i = 0; i < range_count; i += stride)
    {
        float val = (static_cast<float>(data[i]) - 128.0f) * SCALE_U8;
        sum += val * val;
        processed++;
    }
    return {sum, processed};
}

} // namespace WaveformUtils

// 波形数据容器
struct BarData
{
    double sumSquares = 0.0;
    int actualCount = 0;
};

// --- Strategy A (Parallel Seek for FLAC/MP3) ---
static std::vector<BarData> processAudioChunk_StrategyA(
    std::string filepath,
    int streamIdx,
    int64_t absoluteStartSample,
    int64_t absoluteEndSample,
    int64_t globalStartSample,
    double samplesPerBar,
    int totalBars)
{
    std::vector<BarData> localBars(totalBars);
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filepath.c_str(), nullptr, nullptr) < 0)
        return {};

    // 使用 unique_ptr 管理 context
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext *)> fmtGuard(fmt, [](AVFormatContext *f)
                                                                           { avformat_close_input(&f); });

    if (streamIdx >= (int)fmt->nb_streams)
        return {};

    AVCodecParameters *param = fmt->streams[streamIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(param->codec_id);
    auto ctx = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)>(
        avcodec_alloc_context3(codec), [](AVCodecContext *c)
        { avcodec_free_context(&c); });

    avcodec_parameters_to_context(ctx.get(), param);
    ctx->thread_count = 1;

    if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
        return {};

    int decimation = (ctx->sample_rate > 48000) ? (ctx->sample_rate / 32000) : 1;
    if (decimation < 1)
        decimation = 1;

    int64_t seekTimestamp = av_rescale_q(absoluteStartSample, (AVRational){1, ctx->sample_rate}, fmt->streams[streamIdx]->time_base);
    av_seek_frame(fmt, streamIdx, seekTimestamp, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx.get());

    auto pkt = make_av_packet();
    auto frame = make_av_frame();
    int64_t currentGlobalSample = -1;

    while (av_read_frame(fmt, pkt.get()) >= 0)
    {
        if (pkt->stream_index == streamIdx)
        {
            if (avcodec_send_packet(ctx.get(), pkt.get()) >= 0)
            {
                while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0)
                {
                    if (frame->pts != AV_NOPTS_VALUE)
                    {
                        int64_t ptsSample = av_rescale_q(frame->pts, fmt->streams[streamIdx]->time_base, (AVRational){1, ctx->sample_rate});
                        if (currentGlobalSample == -1 || std::abs(ptsSample - currentGlobalSample) > 2000)
                        {
                            currentGlobalSample = ptsSample;
                        }
                    }
                    else if (currentGlobalSample == -1)
                        currentGlobalSample = 0;

                    if (currentGlobalSample >= absoluteEndSample)
                        return localBars;

                    int samples = frame->nb_samples;
                    int offset = 0;

                    if (currentGlobalSample < absoluteStartSample)
                    {
                        offset = static_cast<int>(absoluteStartSample - currentGlobalSample);
                        if (offset >= samples)
                        {
                            currentGlobalSample += samples;
                            continue;
                        }
                    }

                    int processSamples = samples - offset;
                    if (currentGlobalSample + samples > absoluteEndSample)
                    {
                        processSamples = static_cast<int>(absoluteEndSample - (currentGlobalSample + offset));
                    }
                    if (processSamples <= 0)
                        continue;

                    int channels = ctx->ch_layout.nb_channels;
                    int step = (!av_sample_fmt_is_planar(ctx->sample_fmt) && channels > 1) ? channels : 1;

                    void *rawData = frame->data[0];
                    int processedInFrame = 0;
                    int64_t frameBaseSample = currentGlobalSample + offset;
                    int64_t relativeSample = frameBaseSample - globalStartSample;
                    int curBarIdx = std::max(0, static_cast<int>(relativeSample / samplesPerBar));
                    double nextBarBoundaryRel = (curBarIdx + 1) * samplesPerBar;

                    while (processedInFrame < processSamples && curBarIdx < totalBars)
                    {
                        int64_t currentRelative = (frameBaseSample + processedInFrame) - globalStartSample;
                        int64_t needed = static_cast<int64_t>(nextBarBoundaryRel) - currentRelative;
                        if (needed <= 0)
                            needed = 1;
                        int count = std::min((int64_t)(processSamples - processedInFrame), needed);

                        WaveformUtils::ChunkResult res = {0.0f, 0};
                        int dataOffset = (offset + processedInFrame) * step;

                        using namespace WaveformUtils;
                        switch (ctx->sample_fmt)
                        {
                        case AV_SAMPLE_FMT_FLTP:
                        case AV_SAMPLE_FMT_FLT:
                            res = computeSumSquaresAVX2_Float((const float *)rawData + dataOffset, count, step, decimation);
                            break;
                        case AV_SAMPLE_FMT_S16P:
                        case AV_SAMPLE_FMT_S16:
                            res = computeSumSquaresAVX2_S16((const int16_t *)rawData + dataOffset, count, step, decimation);
                            break;
                        case AV_SAMPLE_FMT_S32P:
                        case AV_SAMPLE_FMT_S32:
                            res = computeSumSquaresAVX2_S32((const int32_t *)rawData + dataOffset, count, step, decimation);
                            break;
                        case AV_SAMPLE_FMT_U8P:
                        case AV_SAMPLE_FMT_U8:
                            res = computeSumSquares_U8((const uint8_t *)rawData + dataOffset, count, step, decimation);
                            break;
                        default: break;
                        }

                        localBars[curBarIdx].sumSquares += res.sumSquares;
                        localBars[curBarIdx].actualCount += res.actualCount;
                        processedInFrame += count;

                        if (((frameBaseSample + processedInFrame) - globalStartSample) >= nextBarBoundaryRel)
                        {
                            curBarIdx++;
                            nextBarBoundaryRel += samplesPerBar;
                        }
                    }
                    currentGlobalSample += samples;
                }
            }
        }
        av_packet_unref(pkt.get());
    }
    return localBars;
}

// --- Strategy B (Sequential Batch for M4A/MP4) ---
static std::vector<BarData> processPacketBatch_StrategyB(
    std::vector<AVPacketSharedPtr> packets,
    AVCodecParameters *codecPar,
    AVRational timeBase,
    int64_t globalStartSample,
    double samplesPerBar,
    int totalBars)
{
    std::vector<BarData> localBars(totalBars);
    if (packets.empty())
        return localBars;

    const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
    auto ctx = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)>(
        avcodec_alloc_context3(codec), [](AVCodecContext *c)
        { avcodec_free_context(&c); });

    avcodec_parameters_to_context(ctx.get(), codecPar);
    ctx->thread_count = 1;

    if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
        return localBars;

    int decimation = (ctx->sample_rate > 48000) ? (ctx->sample_rate / 32000) : 1;
    if (decimation < 1)
        decimation = 1;
    auto frame = make_av_frame();

    for (const auto &pkt : packets)
    {
        if (avcodec_send_packet(ctx.get(), pkt.get()) >= 0)
        {
            while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0)
            {
                if (frame->pts == AV_NOPTS_VALUE)
                    continue;

                int64_t ptsSample = av_rescale_q(frame->pts, timeBase, (AVRational){1, ctx->sample_rate});
                int samples = frame->nb_samples;
                int channels = ctx->ch_layout.nb_channels;
                int step = (!av_sample_fmt_is_planar(ctx->sample_fmt) && channels > 1) ? channels : 1;
                void *rawData = frame->data[0];

                int processedInFrame = 0;
                int64_t frameBaseSample = ptsSample;
                int64_t relativeSample = frameBaseSample - globalStartSample;
                int curBarIdx = static_cast<int>(relativeSample / samplesPerBar);

                if (curBarIdx < 0 && (relativeSample + samples) < 0)
                    continue;
                if (curBarIdx < 0)
                    curBarIdx = 0;

                double nextBarBoundaryRel = (curBarIdx + 1) * samplesPerBar;

                while (processedInFrame < samples && curBarIdx < totalBars)
                {
                    int64_t currentRel = (frameBaseSample + processedInFrame) - globalStartSample;
                    if (currentRel < 0)
                    {
                        processedInFrame++;
                        continue;
                    }

                    int64_t needed = static_cast<int64_t>(nextBarBoundaryRel) - currentRel;
                    if (needed <= 0)
                        needed = 1;
                    int count = std::min((int64_t)(samples - processedInFrame), needed);

                    WaveformUtils::ChunkResult res = {0.0f, 0};
                    int dataOffset = processedInFrame * step;

                    using namespace WaveformUtils;
                    switch (ctx->sample_fmt)
                    {
                    case AV_SAMPLE_FMT_FLTP:
                    case AV_SAMPLE_FMT_FLT:
                        res = computeSumSquaresAVX2_Float((const float *)rawData + dataOffset, count, step, decimation);
                        break;
                    case AV_SAMPLE_FMT_S16P:
                    case AV_SAMPLE_FMT_S16:
                        res = computeSumSquaresAVX2_S16((const int16_t *)rawData + dataOffset, count, step, decimation);
                        break;
                    case AV_SAMPLE_FMT_S32P:
                    case AV_SAMPLE_FMT_S32:
                        res = computeSumSquaresAVX2_S32((const int32_t *)rawData + dataOffset, count, step, decimation);
                        break;
                    case AV_SAMPLE_FMT_U8P:
                    case AV_SAMPLE_FMT_U8:
                        res = computeSumSquares_U8((const uint8_t *)rawData + dataOffset, count, step, decimation);
                        break;
                    default: break;
                    }

                    localBars[curBarIdx].sumSquares += res.sumSquares;
                    localBars[curBarIdx].actualCount += res.actualCount;
                    processedInFrame += count;

                    if (((frameBaseSample + processedInFrame) - globalStartSample) >= nextBarBoundaryRel)
                    {
                        curBarIdx++;
                        nextBarBoundaryRel += samplesPerBar;
                    }
                }
            }
        }
    }
    return localBars;
}

// --- Main Waveform Builder ---
std::vector<int> AudioPlayer::buildAudioWaveform(
    const std::string &filepath,
    int barCount,
    int totalWidth,
    int &barWidth,
    int maxHeight,
    int64_t startTimeUS,
    int64_t endTimeUS)
{
    std::vector<int> barHeights(barCount, 0);
    if (barCount <= 0 || totalWidth <= 0)
        return barHeights;
    barWidth = std::max(1, (totalWidth / barCount) - 2);

    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filepath.c_str(), nullptr, nullptr) < 0)
        return barHeights;
    // RAII for format context
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext *)> fmtGuard(fmt, [](AVFormatContext *f)
                                                                           { avformat_close_input(&f); });

    if (avformat_find_stream_info(fmt, nullptr) < 0)
        return barHeights;

    int streamIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIdx < 0)
        return barHeights;

    int64_t fileDurationUS = fmt->duration * 1000000 / AV_TIME_BASE;
    if (endTimeUS <= 0 || endTimeUS > fileDurationUS)
        endTimeUS = fileDurationUS;
    if (startTimeUS < 0)
        startTimeUS = 0;
    if (startTimeUS >= endTimeUS)
        return barHeights;

    int sampleRate = fmt->streams[streamIdx]->codecpar->sample_rate;
    int64_t globalStartSample = av_rescale(startTimeUS, sampleRate, 1000000);
    int64_t globalEndSample = av_rescale(endTimeUS, sampleRate, 1000000);
    int64_t totalSegmentSamples = globalEndSample - globalStartSample;
    double samplesPerBar = std::max(1.0, (double)totalSegmentSamples / barCount);

    // 策略选择
    const char *formatName = fmt->iformat->name;
    bool useStrategyB = false;
    const char *complexContainers[] = {"mov", "mp4", "m4a", "3gp", "3g2", "mj2", "matroska", "webm"};
    for (const char *name : complexContainers)
    {
        if (strstr(formatName, name) != nullptr)
        {
            useStrategyB = true;
            break;
        }
    }

    std::vector<std::future<std::vector<BarData>>> futures;
    std::vector<BarData> finalBarsData(barCount);

    if (!useStrategyB) // Strategy A: Parallel Seek
    {
        // 提前关闭主线程的 handle，子线程会自己打开
        fmtGuard.reset();

        int threadCount = std::max(2, (int)std::thread::hardware_concurrency());
        if (endTimeUS - startTimeUS < 1000000)
            threadCount = 1;

        int64_t samplesPerThread = totalSegmentSamples / threadCount;
        for (int i = 0; i < threadCount; ++i)
        {
            int64_t tStart = globalStartSample + i * samplesPerThread;
            int64_t tEnd = (i == threadCount - 1) ? globalEndSample : (tStart + samplesPerThread);
            futures.push_back(SimpleThreadPool::instance().enqueue(
                processAudioChunk_StrategyA, filepath, streamIdx, tStart, tEnd, globalStartSample, samplesPerBar, barCount));
        }
    }
    else // Strategy B: Pipeline Reading
    {
        AVCodecParameters *codecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(codecPar, fmt->streams[streamIdx]->codecpar);
        AVRational timeBase = fmt->streams[streamIdx]->time_base;
        // RAII helper for codecPar
        std::unique_ptr<AVCodecParameters, void (*)(AVCodecParameters *)> parGuard(codecPar, [](AVCodecParameters *p)
                                                                                   { avcodec_parameters_free(&p); });

        std::vector<AVPacketSharedPtr> currentBatch;
        const int BATCH_SIZE = 250;
        currentBatch.reserve(BATCH_SIZE);

        auto tempPkt = make_av_packet();
        int64_t seekTarget = av_rescale_q(startTimeUS, (AVRational){1, 1000000}, timeBase);
        av_seek_frame(fmt, streamIdx, seekTarget, AVSEEK_FLAG_BACKWARD);
        int64_t endPTS = av_rescale_q(endTimeUS, (AVRational){1, 1000000}, timeBase);

        while (av_read_frame(fmt, tempPkt.get()) >= 0)
        {
            if (tempPkt->stream_index == streamIdx)
            {
                if (tempPkt->pts != AV_NOPTS_VALUE && tempPkt->pts > endPTS)
                    break;

                // Clone packet for the thread (使用 Shared Ptr)
                auto clone = make_shared_av_packet();
                av_packet_ref(clone.get(), tempPkt.get());
                currentBatch.push_back(clone); // shared_ptr 拷贝成本低

                if (currentBatch.size() >= BATCH_SIZE)
                {
                    futures.push_back(SimpleThreadPool::instance().enqueue(
                        processPacketBatch_StrategyB, currentBatch, codecPar, timeBase, globalStartSample, samplesPerBar, barCount));
                    currentBatch.clear();
                    currentBatch.reserve(BATCH_SIZE);
                }
            }
            av_packet_unref(tempPkt.get());
        }

        if (!currentBatch.empty())
        {
            futures.push_back(SimpleThreadPool::instance().enqueue(
                processPacketBatch_StrategyB, currentBatch, codecPar, timeBase, globalStartSample, samplesPerBar, barCount));
        }
        // fmtGuard 自动析构
    }

    // 汇总结果
    for (auto &fut : futures)
    {
        auto chunkResult = fut.get();
        for (int i = 0; i < barCount; ++i)
        {
            if (chunkResult[i].actualCount > 0)
            {
                finalBarsData[i].sumSquares += chunkResult[i].sumSquares;
                finalBarsData[i].actualCount += chunkResult[i].actualCount;
            }
        }
    }

    // 转换为视觉高度 (dB Scale)
    const float DB_CEILING = 0.0f;
    const float DB_FLOOR = -55.0f;
    const float DB_RANGE = DB_CEILING - DB_FLOOR;

    for (int i = 0; i < barCount; ++i)
    {
        float visualHeight = 0.0f;
        if (finalBarsData[i].actualCount > 0)
        {
            float rms = std::sqrt(finalBarsData[i].sumSquares / finalBarsData[i].actualCount);
            float db = (rms < 1e-9f) ? DB_FLOOR : 20.0f * std::log10(rms);

            db = std::clamp(db, DB_FLOOR, DB_CEILING);
            float normalized = (db - DB_FLOOR) / DB_RANGE;
            visualHeight = normalized * maxHeight;
        }
        barHeights[i] = std::max(2, static_cast<int>(visualHeight));
    }

    return barHeights;
}