#include "AudioPlayer.hpp"
#include "SimpleThreadPool.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <future>
#include <deque>

// --- RAII Wrappers for FFmpeg ---

struct AVPacketDeleter
{
    void operator()(AVPacket *p) const
    {
        av_packet_free(&p);
    }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
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
constexpr double AUDIO_BUFFER_DURATION_SECONDS = 0.4;
constexpr int MIN_AUDIO_QUEUE_SIZE = 4;
} // namespace

// 格式转换工具 (Miniaudio <-> FFmpeg)
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
    default: return ma_format_f32;
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

// --- AudioStreamSource Implementation ---

void AudioPlayer::AudioStreamSource::free()
{
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

    pCodecCtx->thread_count = 0;

    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        free();
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
    mixingParams.sample_rate = 96000;
    mixingParams.fmt = AV_SAMPLE_FMT_S32;
    // 混合器输出通常假设以 1/sample_rate 为时间基，但实际会被 deviceParams 覆盖
    mixingParams.time_base = {1, 96000};

    m_filterChain = std::make_unique<AudioFilterChain>();
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
    std::queue<std::shared_ptr<AudioFrame>> empty;
    std::swap(audioFrameQueue, empty);

    m_currentFrame.reset();
    m_currentFramePos = 0;

    if (m_filterChain)
    {
        m_filterChain->flush();
    }
}

bool AudioPlayer::isValidAudio(const std::string &path)
{
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0)
        return false;
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

    // 停止设备，防止回调读取脏数据
    if (m_deviceInited)
    {
        ma_device_stop(&m_device);
    }

    flushQueue();

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = path;
        preloadPath.clear();
        hasPreloaded = false;
        m_preloadSource.reset();
        m_decoderCursor.store(0);
        nowPlayingTime.store(0);

        m_endOfStreamReached.store(false);
        m_playbackFinishedCallbackFired.store(false);
        lastReportedPath = path;
    }

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

    if (m_deviceInited)
    {
        ma_device_stop(&m_device);
    }
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
    std::lock_guard<std::mutex> decodeLock(decodeMutex);

    bool wasPlaying = (playingState == PlayerState::Playing);

    if (m_deviceInited)
        ma_device_stop(&m_device);

    flushQueue();
    mixingParams = params;

    closeAudioDevice();
    if (openAudioDevice())
    {
        m_preloadSource.reset();
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

    bool tryOpen = (mode == OutputMode::Mixing) || (m_currentSource != nullptr);

    if (tryOpen && openAudioDevice())
    {
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
}

bool AudioPlayer::openAudioDevice()
{
    ma_format targetFormat;
    ma_uint32 targetChannels;
    ma_uint32 targetSampleRate;
    std::string targetAppName;

    if (outputMode.load() == OutputMode::Mixing)
    {
        targetFormat = toMaFormat(mixingParams.fmt);
        targetChannels = mixingParams.ch_layout.nb_channels;
        targetSampleRate = mixingParams.sample_rate;
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

    if (m_deviceInited)
    {
        if (outputMode.load() == OutputMode::Mixing && m_device.playback.format == targetFormat && m_device.playback.channels == targetChannels && m_device.sampleRate == targetSampleRate)
        {
            return true;
        }
        closeAudioDevice();
    }

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
    deviceParams.sample_rate = m_device.sampleRate;
    deviceParams.fmt = toAVSampleFormat(m_device.playback.format);
    av_channel_layout_default(&deviceParams.ch_layout, m_device.playback.channels);

    ma_device_set_master_volume(&m_device, (float)volume.load());
    return true;
}

// --- Miniaudio Callback ---
void AudioPlayer::ma_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(pDevice->pUserData);
    if (!player)
        return;

    int bytesPerSample = av_get_bytes_per_sample(player->deviceParams.fmt);
    int channels = player->deviceParams.ch_layout.nb_channels;
    int totalBytesNeeded = frameCount * channels * bytesPerSample;

    uint8_t *outPtr = static_cast<uint8_t *>(pOutput);
    int bytesWritten = 0;

    std::lock_guard<std::mutex> lock(player->audioFrameQueueMutex);

    while (bytesWritten < totalBytesNeeded)
    {
        if (!player->m_currentFrame)
        {
            if (player->audioFrameQueue.empty())
            {
                // 队列已空，检查是否播放完毕
                if (player->m_endOfStreamReached.load() && !player->m_playbackFinishedCallbackFired.load())
                {
                    player->m_playbackFinishedCallbackFired.store(true);
                    if (player->m_callbacks.onFileComplete)
                    {
                        player->m_callbacks.onFileComplete();
                    }
                }
                player->stateCondVar.notify_one();
                break;
            }

            player->m_currentFrame = player->audioFrameQueue.front();
            player->audioFrameQueue.pop();
            player->m_currentFramePos = 0;

            // 检测路径变更
            if (player->m_currentFrame && !player->m_currentFrame->sourcePath.empty())
            {
                if (player->m_currentFrame->sourcePath != player->lastReportedPath)
                {
                    player->lastReportedPath = player->m_currentFrame->sourcePath;
                    if (player->m_callbacks.onPathChanged)
                    {
                        player->m_callbacks.onPathChanged(player->lastReportedPath);
                    }
                }
            }
            player->stateCondVar.notify_one();
        }

        if (!player->m_currentFrame)
            break;

        // 计算时间戳
        if (player->deviceParams.sample_rate > 0)
        {
            int64_t bytesPlayed = player->m_currentFramePos;
            int64_t offsetUS = (bytesPlayed * 1000000) / (channels * bytesPerSample * player->deviceParams.sample_rate);
            int64_t currentUS = player->m_currentFrame->pts + offsetUS;
            player->nowPlayingTime.store(currentUS);

            int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
            int64_t last = player->lastCallbackTime.load(std::memory_order_relaxed);
            if (now - last > 100000000) // 100ms
            {
                player->lastCallbackTime.store(now, std::memory_order_relaxed);
                if (player->m_callbacks.onPositionChanged)
                {
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
                    isSongLoopActive = false;
                    continue;
                }

                if (playingState == PlayerState::Seeking)
                {
                    handleSeekRequest();
                    continue;
                }

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
                          return false; });

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

    if (m_filterChain)
    {
        m_filterChain->flush();
    }
}

// MP3 采样率查询表
// Version indices: 0=MPEG2.5, 1=Reserved, 2=MPEG2, 3=MPEG1
// Rate indices: 0, 1, 2 (3=Reserved)
static const int kMp3SampleRates[4][4] = {
    {11025, 12000, 8000, 0},  // 00: MPEG 2.5
    {0, 0, 0, 0},             // 01: Reserved
    {22050, 24000, 16000, 0}, // 10: MPEG 2
    {44100, 48000, 32000, 0}  // 11: MPEG 1
};

/**
 * @brief 解析 MP3 帧头并返回采样率
 * @return 返回采样率 (Hz)，如果非有效 MP3 帧头则返回 -1
 */
static int getMp3FrameSampleRate(const uint8_t *head, int size)
{
    // 最小 MP3 帧头为 4 字节
    if (size < 4)
        return -1;

    // 提取前 4 字节为 32 位整数 (Big Endian)
    uint32_t header = (static_cast<uint32_t>(head[0]) << 24) | (static_cast<uint32_t>(head[1]) << 16) | (static_cast<uint32_t>(head[2]) << 8) | static_cast<uint32_t>(head[3]);

    // 1. Sync Word 校验: 11 bits (0xFFE0....)
    if ((header & 0xFFE00000) != 0xFFE00000)
        return -1;

    // 2. Version Index (Bits 20,19)
    int ver_idx = (header >> 19) & 3;
    if (ver_idx == 1)
        return -1; // Reserved

    // 3. Layer Index (Bits 18,17)
    int layer_idx = (header >> 17) & 3;
    if (layer_idx == 0)
        return -1; // Reserved

    // 4. Bitrate Index (Bits 15-12)
    int bitrate_idx = (header >> 12) & 0xF;
    if (bitrate_idx == 0 || bitrate_idx == 15)
        return -1; // Free/Bad

    // 5. Sample Rate Index (Bits 11,10)
    int srate_idx = (header >> 10) & 3;
    if (srate_idx == 3)
        return -1; // Reserved

    return kMp3SampleRates[ver_idx][srate_idx];
}

void AudioPlayer::decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally)
{
    int ret = av_read_frame(m_currentSource->pFormatCtx, packet);
    if (ret < 0)
    {
        if (ret == AVERROR_EOF)
        {
            avcodec_send_packet(m_currentSource->pCodecCtx, nullptr);
            AVFramePtr frame = make_av_frame();
            while (avcodec_receive_frame(m_currentSource->pCodecCtx, frame.get()) >= 0)
            {
                processFrame(frame.get());
            }

            if (outputMode.load() == OutputMode::Mixing && performSeamlessSwitch())
            {
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
                    playbackFinishedNaturally = false;
                    isSongLoopActive = false;
                    return;
                }
            }

            m_endOfStreamReached.store(true);
            playbackFinishedNaturally = true;
            isSongLoopActive = false;
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
        bool isValidPacket = true;

        // 仅针对 MP3 格式启用“严格审查模式”
        if (m_currentSource->pCodecCtx->codec_id == AV_CODEC_ID_MP3)
        {
            // 获取当前音频流的预期采样率 (例如 44100)
            int expectedSampleRate = m_currentSource->pCodecCtx->sample_rate;

            // 解析包头中的实际采样率
            int packetSampleRate = getMp3FrameSampleRate(packet->data, packet->size);

            if (packetSampleRate == -1)
            {
                // 连基础帧头结构都不对，肯定是垃圾
                isValidPacket = false;
            }
            else if (packetSampleRate != expectedSampleRate)
            {
                // 帧头结构虽然合法，但采样率突变 (例如 44100 -> 12000)
                // 判定为伪装成音频的元数据/乱码
                isValidPacket = false;
                spdlog::warn("Dropped consistent-check failed MP3 packet: {} Hz vs Exp {} Hz", packetSampleRate, expectedSampleRate);
            }
        }

        // 如果是 FLAC, WAV, AAC 等其他格式，默认信任 Demuxer，isValidPacket 保持 true

        if (isValidPacket)
        {
            if (avcodec_send_packet(m_currentSource->pCodecCtx, packet) >= 0)
            {
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
    }
    av_packet_unref(packet);
}

// =========================================================
// 【核心修改】使用 AudioFilterChain 替代 SwrContext
// =========================================================
bool AudioPlayer::processFrame(AVFrame *frame)
{
    if (!frame || !m_currentSource || !m_filterChain)
        return false;

    // 1. 准备 Input Params
    AudioParams inputParams;
    inputParams.sample_rate = frame->sample_rate;
    inputParams.fmt = static_cast<AVSampleFormat>(frame->format);

    if (frame->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&inputParams.ch_layout, &frame->ch_layout);
    else
        av_channel_layout_default(&inputParams.ch_layout, frame->ch_layout.nb_channels);

    // 【关键】从 Stream 中获取真实的时间基并传递给 AudioParams
    // 这样 AudioFilterChain 就会知道输入数据的 PTS 单位是什么
    int streamIdx = m_currentSource->audioStreamIndex;
    if (streamIdx >= 0)
    {
        inputParams.time_base = m_currentSource->pFormatCtx->streams[streamIdx]->time_base;
    }
    else
    {
        // Fallback: 如果没有流信息，通常 frame->time_base 也是不准的，只能猜
        inputParams.time_base = {1, frame->sample_rate};
    }

    // 2. 初始化滤镜链
    // 此时 inputParams 包含了正确的时间基，init 会检测是否需要重建
    int ret = m_filterChain->init(inputParams, deviceParams, "");
    if (ret < 0)
    {
        spdlog::error("Failed to init filter chain: {}", my_av_strerror(ret));
        return false;
    }

    // 3. 将帧推入滤镜链
    AVFrame *cloneFrame = av_frame_clone(frame);
    if (!cloneFrame)
        return false;

    // 【关键】不再需要手动 av_rescale_q
    // 因为 abuffer 滤镜已经通过 time_base 参数知道这个 PTS 的含义了，
    // FFmpeg 内部会自动处理时间基转换。

    // 更新 UI 显示用的软时钟逻辑 (保持不变，用于 triggerPreload)
    double frameDurationSec = (double)frame->nb_samples / frame->sample_rate;
    int64_t frameDurationMicro = static_cast<int64_t>(frameDurationSec * 1000000);
    int64_t ptsMicro = 0;

    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
    {
        double ptsSec = frame->best_effort_timestamp * av_q2d(m_currentSource->pFormatCtx->streams[streamIdx]->time_base);
        ptsMicro = static_cast<int64_t>(ptsSec * 1000000);
        m_decoderCursor.store(ptsMicro + frameDurationMicro);
    }
    else
    {
        ptsMicro = m_decoderCursor.load();
        m_decoderCursor.fetch_add(frameDurationMicro);
    }
    triggerPreload(static_cast<double>(ptsMicro) / 1000000.0);

    ret = m_filterChain->push_frame(cloneFrame);
    if (ret < 0)
    {
        av_frame_free(&cloneFrame);
        return false;
    }

    return pullProcessedFramesFromGraph();
}

bool AudioPlayer::pullProcessedFramesFromGraph()
{
    AVFramePtr outFrame = make_av_frame();

    // 【关键】获取滤镜链实际输出的时间基
    // 滤镜链经过 abuffer -> aformat -> abuffersink 后，
    // 时间基可能会变 (通常 aformat 会将其重置为 1/sample_rate)
    AVRational outTB = m_filterChain->get_output_time_base();

    while (true)
    {
        int ret = m_filterChain->pop_frame(outFrame.get());

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return false;

        auto audioFrame = std::make_shared<AudioFrame>();

        // 将滤镜输出的 PTS 转换为微秒 (用于 AudioFrame 和 Miniaudio 回调)
        if (outFrame->pts != AV_NOPTS_VALUE && outTB.den > 0)
        {
            // 使用 av_rescale_q 将输出时间基转换为 1/1,000,000 (微秒)
            audioFrame->pts = av_rescale_q(outFrame->pts, outTB, {1, 1000000});
        }
        else
        {
            audioFrame->pts = m_decoderCursor.load(); // Fallback
        }
        audioFrame->sourcePath = m_currentSource->path;

        // ... (数据拷贝部分保持不变，假设输出为 Packed 格式) ...
        int bytesPerSample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(outFrame->format));
        int channels = outFrame->ch_layout.nb_channels;
        int samples = outFrame->nb_samples;
        int bufferSize = av_samples_get_buffer_size(nullptr, channels, samples, static_cast<AVSampleFormat>(outFrame->format), 1);

        if (bufferSize > 0)
        {
            audioFrame->data.resize(bufferSize);
            // 简单拷贝 data[0]，确保 deviceParams 是 Packed 格式
            memcpy(audioFrame->data.data(), outFrame->data[0], bufferSize);

            totalDecodedBytes += bufferSize;
            totalDecodedFrames++;
            calculateQueueSize(bytesPerSample);

            {
                std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
                audioFrameQueue.push(audioFrame);
            }
        }
        av_frame_unref(outFrame.get());
    }
    return true;
}

bool AudioPlayer::setupDecodingSession(const std::string &path)
{
    m_endOfStreamReached.store(false);
    m_playbackFinishedCallbackFired.store(false);
    m_currentSource = std::make_unique<AudioStreamSource>();
    if (!m_currentSource->initDecoder(path))
    {
        return false;
    }

    if (!openAudioDevice())
    {
        return false;
    }

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
        if (src->initDecoder(pPath))
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

    int bytesPerSec = deviceParams.sample_rate * deviceParams.ch_layout.nb_channels * out_bytes_per_sample;
    int targetBytes = static_cast<int>(bytesPerSec * AUDIO_BUFFER_DURATION_SECONDS);
    size_t maxF = std::max(MIN_AUDIO_QUEUE_SIZE, targetBytes / avgBytes);

    audioFrameQueueMaxSize.store(maxF);
    hasCalculatedQueueSize.store(true);
}

bool AudioPlayer::performSeamlessSwitch()
{
    if (outputMode.load() != OutputMode::Mixing || !hasPreloaded.load() || !m_preloadSource)
        return false;

    m_currentSource = std::move(m_preloadSource);

    std::string newPath;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = m_currentSource->path;
        newPath = currentPath;
        preloadPath.clear();
    }

    audioDuration.store(m_currentSource->pFormatCtx->duration);
    hasPreloaded.store(false);
    nowPlayingTime.store(0);
    m_decoderCursor.store(0);

    totalDecodedBytes = 0;
    totalDecodedFrames = 0;
    hasCalculatedQueueSize = false;
    return true;
}

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
            // ID3v1 标签固定为 128 字节，且位于文件绝对末尾。
            // FFmpeg 有时会将这部分数据误读进最后一个 Packet 中。
            if (pkt->size >= 128)
            {
                // 获取 packet 数据末尾的最后 128 字节的指针
                const uint8_t *tail = pkt->data + pkt->size - 128;

                // 检查 Magic Bytes: 'T', 'A', 'G'
                if (tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G')
                {
                    // 确认这是 ID3v1 标签！
                    // 直接减小 packet 的 size，逻辑上“切除”这部分数据。
                    // 注意：这不会导致内存泄漏，av_packet_unref 依然会释放整个 buffer。
                    pkt->size -= 128;

                    // 可选：打印日志确认
                    spdlog::info("Detected and stripped ID3v1 tag from last packet.");
                    // printf("Detected and stripped ID3v1 tag from last packet.\n");
                }
            }
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