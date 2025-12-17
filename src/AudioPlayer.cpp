#include "AudioPlayer.hpp"
#include "SimpleThreadPool.hpp"

// --- Helper Functions ---

static std::string my_av_strerror(int errnum)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(errbuf, sizeof(errbuf), errnum);
    return std::string(errbuf);
}

namespace
{
constexpr double PRELOAD_TRIGGER_SECONDS_BEFORE_END = 10.0;
constexpr double AUDIO_BUFFER_DURATION_SECONDS = 0.4; // 400ms
constexpr int MIN_AUDIO_QUEUE_SIZE = 4;
} // namespace

// 将 FFmpeg 格式转换为 Miniaudio 格式
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

// 将 Miniaudio 格式转换回 FFmpeg 格式 (用于反向校验)
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

// --- AudioStreamSource ---

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

std::string AudioPlayer::getCurrentStreamTitle() const
{
    if (!m_currentSource || !m_currentSource->pFormatCtx)
    {
        return "AppMusicPlayer"; // 默认名称
    }

    AVDictionary *metadata = m_currentSource->pFormatCtx->metadata;
    AVDictionaryEntry *artist = av_dict_get(metadata, "artist", nullptr, 0);
    AVDictionaryEntry *title = av_dict_get(metadata, "title", nullptr, 0);

    std::string displayStr;
    if (artist && title)
    {
        displayStr = std::string(artist->value) + " - " + std::string(title->value);
    }
    else if (title)
    {
        displayStr = std::string(title->value);
    }
    else
    {
        // 如果没有元数据，回退到文件名
        size_t lastSlash = currentPath.find_last_of("/\\");
        if (lastSlash != std::string::npos)
            displayStr = currentPath.substr(lastSlash + 1);
        else
            displayStr = "AppMusicPlayer";
    }
    return displayStr;
}

bool AudioPlayer::AudioStreamSource::initDecoder(const std::string &inputPath, char *errorBuffer)
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

    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        free();
        return false;
    }
    return true;
}

bool AudioPlayer::AudioStreamSource::openSwrContext(const AudioParams &deviceParams, double volume, char *errorBuffer)
{
    if (!pCodecCtx)
        return false;

    if (swrCtx)
    {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }

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

// --- AudioPlayer ---

AudioPlayer::AudioPlayer()
{
    av_channel_layout_default(&mixingParams.ch_layout, 2);
    decodeThread = std::thread(&AudioPlayer::mainDecodeThread, this);
}

AudioPlayer::~AudioPlayer()
{
    quitFlag.store(true);
    pathCondVar.notify_one();
    stateCondVar.notify_one();

    if (decodeThread.joinable())
    {
        decodeThread.join();
    }

    freeResources();
    closeAudioDevice();
    if (m_contextInited)
    {
        ma_context_uninit(&m_context);
        m_contextInited = false;
    }
}

void AudioPlayer::freeResources()
{
    if (outputMode.load() != OUTPUT_MIXING)
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
    // 清空队列
    std::queue<std::shared_ptr<AudioFrame>> empty;
    std::swap(audioFrameQueue, empty);

    // 重置当前帧
    m_currentFrame.reset();
    m_currentFramePos = 0;
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

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = path;
        preloadPath.clear();
        hasPreloaded = false;
        m_preloadSource.reset();
        m_decoderCursor.store(0);
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (playingState == PlayerState::SEEKING)
        {
            // 保持
        }
        else if (playingState != PlayerState::STOPPED)
        {
            oldPlayingState = playingState;
        }
        else
        {
            oldPlayingState = PlayerState::PLAYING;
        }
        playingState = PlayerState::STOPPED;
    }

    pathCondVar.notify_one();
    stateCondVar.notify_one();
    return true;
}

void AudioPlayer::setPreloadPath(const std::string &path)
{
    if (!isValidAudio(path))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(pathMutex);
    if (path != preloadPath)
    {
        preloadPath = path;
        // 只有在 Mixing 模式下我们才会在当前歌曲播放时去预加载资源
        // Direct 模式下只是存储路径，等当前歌曲播完后再处理
        if (outputMode.load() == OUTPUT_MIXING)
        {
            hasPreloaded = false;
            m_preloadSource.reset();
        }
    }
}

void AudioPlayer::play()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (playingState != PlayerState::PLAYING)
    {
        isFirstPlay = false;
        playingState = PlayerState::PLAYING;
        if (m_deviceInited)
        {
            ma_device_start(&m_device);
        }
        stateCondVar.notify_one();
    }
}

void AudioPlayer::pause()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (playingState != PlayerState::PAUSED)
    {
        playingState = PlayerState::PAUSED;
        if (m_deviceInited)
        {
            ma_device_stop(&m_device);
        }
        stateCondVar.notify_one();
    }
}

void AudioPlayer::seek(int64_t timeMicroseconds)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    seekTarget.store(timeMicroseconds);

    if (playingState != PlayerState::SEEKING)
    {
        oldPlayingState = playingState;
    }
    playingState = PlayerState::SEEKING;

    // 暂停设备防止爆音
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

// --- 功能补全：参数设置与模式切换 ---

void AudioPlayer::setMixingParameters(const AudioParams &params)
{
    if (outputMode.load() != OUTPUT_MIXING)
    {
        return;
    }

    // 获取状态锁，暂停播放逻辑
    std::lock_guard<std::mutex> stateLock(stateMutex);
    // 获取解码锁，确保解码线程此刻没有在进行 swr_convert 或使用 deviceParams
    std::lock_guard<std::mutex> decodeLock(decodeMutex);

    bool wasPlaying = (playingState == PlayerState::PLAYING);

    // 1. 停止设备
    if (m_deviceInited)
    {
        ma_device_stop(&m_device);
    }

    // 2. 清空队列 (旧格式的数据已失效)
    flushQueue();

    // 3. 重新配置
    mixingParams = params;

    // 关闭旧设备并重新打开 (openAudioDevice 会使用新的 mixingParams)
    closeAudioDevice();
    if (openAudioDevice())
    {
        // 4. 重新初始化重采样器
        if (m_currentSource)
        {
            // 使用更新后的 deviceParams
            m_currentSource->openSwrContext(deviceParams, 1.0, errorBuffer);
        }

        // 注意：m_preloadSource 的 swr 也是旧的，直接重置预加载状态让其稍后重新加载
        m_preloadSource.reset();
        hasPreloaded.store(false);

        // 5. 恢复播放
        if (wasPlaying && m_deviceInited)
        {
            ma_device_start(&m_device);
        }
    }
}

void AudioPlayer::setOutputMode(outputMod mode)
{
    std::lock_guard<std::mutex> stateLock(stateMutex);
    if (outputMode.load() == mode)
        return;

    std::lock_guard<std::mutex> decodeLock(decodeMutex);

    bool wasPlaying = (playingState == PlayerState::PLAYING);

    // 1. 停止设备
    if (m_deviceInited)
    {
        ma_device_stop(&m_device);
    }

    // 2. 清空队列
    flushQueue();

    // 3. 切换模式
    outputMode.store(mode);

    // 4. 重置设备
    // 在 Direct 模式下，设备参数依赖 currentSource；在 Mixing 模式下依赖 mixingParams
    closeAudioDevice();

    // 只有当有当前源或处于Mixing模式时才能立即打开设备，否则等待播放时打开
    bool tryOpen = (mode == OUTPUT_MIXING) || (m_currentSource != nullptr);

    if (tryOpen && openAudioDevice())
    {
        // 5. 重新初始化重采样器
        if (m_currentSource)
        {
            m_currentSource->openSwrContext(deviceParams, 1.0, errorBuffer);
        }

        // 清理预加载资源 (模式切换导致预加载的资源格式可能不匹配)
        m_preloadSource.reset();
        hasPreloaded.store(false);

        // 6. 恢复播放
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

// --- Audio Device Management (Miniaudio) ---

void AudioPlayer::closeAudioDevice()
{
    if (m_deviceInited)
    {
        ma_device_uninit(&m_device);
        m_deviceInited = false;
    }

    // Device 依赖 Context，所以 Device 关了之后可以关 Context，或者在析构时关。
    // 为了支持动态改名，建议在 openAudioDevice 里按需重建，
    // 这里可以不做操作，或者也清理掉。
    // 如果为了性能（Context 初始化涉及加载 DLL），可以保留 Context 直到析构。
    // 但为了改名，我们在 openAudioDevice 里已经做了清理逻辑。
}

bool AudioPlayer::openAudioDevice()
{
    // 1. 计算期望的目标参数
    ma_format targetFormat;
    ma_uint32 targetChannels;
    ma_uint32 targetSampleRate;
    std::string targetAppName;

    if (outputMode.load() == OUTPUT_MIXING)
    {
        targetFormat = toMaFormat(mixingParams.sampleFormat);
        targetChannels = mixingParams.channels;
        targetSampleRate = mixingParams.sampleRate;
        targetAppName = "AppMusicPlayer";
    }
    else
    {
        if (!m_currentSource || !m_currentSource->pCodecCtx)
            return false;

        targetFormat = toMaFormat(m_currentSource->pCodecCtx->sample_fmt);
        targetChannels = m_currentSource->pCodecCtx->ch_layout.nb_channels;
        targetSampleRate = m_currentSource->pCodecCtx->sample_rate;

        targetAppName = getCurrentStreamTitle();
    }

    // --- 优化检查逻辑 ---
    if (m_deviceInited)
    {
        if (outputMode.load() == OUTPUT_MIXING && m_device.playback.format == targetFormat && m_device.playback.channels == targetChannels && m_device.sampleRate == targetSampleRate)
        {
            return true;
        }
        closeAudioDevice();
    }

    // 2. 初始化 Context (为了设置 App Name)
    if (m_contextInited)
    {
        ma_context_uninit(&m_context);
        m_contextInited = false;
    }

    ma_context_config ctxConfig = ma_context_config_init();

    // [修复 1] 针对不同后端设置应用名称
    // miniaudio 的 config 结构体中，应用名称通常是在特定后端的子结构里
    // 尤其是 Linux 上的 PulseAudio 需要这个字段来显示名称
#ifdef __linux__
    // 确保你的 miniaudio 版本包含 pulse 定义，通常默认包含
    ctxConfig.pulse.pApplicationName = targetAppName.c_str();
#endif
    // Windows (WASAPI) 通常使用窗口标题或可执行文件名，miniaudio 暂无通用的 pApplicationName 字段

    // 初始化 context
    if (ma_context_init(NULL, 0, &ctxConfig, &m_context) != MA_SUCCESS)
    {
        spdlog::error("[AudioPlayer:569] Failed to initialize audio context");
        return false;
    }
    m_contextInited = true;

    // 3. 配置新设备
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.dataCallback = ma_data_callback;
    config.pUserData = this;

    // 应用参数
    config.playback.format = targetFormat;
    config.playback.channels = targetChannels;
    config.sampleRate = targetSampleRate;

    // 4. 初始化设备
    if (ma_device_init(&m_context, &config, &m_device) != MA_SUCCESS)
    {
        std::cerr << "Failed to open playback device." << std::endl;
        return false;
    }

    m_deviceInited = true;

    // 回填实际参数
    deviceParams.sampleRate = m_device.sampleRate;
    deviceParams.sampleFormat = toAVSampleFormat(m_device.playback.format);
    deviceParams.ch_layout = toAVChannelLayout(m_device.playback.channels);
    deviceParams.channels = m_device.playback.channels;

    ma_device_set_master_volume(&m_device, (float)volume.load());

    return true;
}

// --- Critical: Miniaudio Callback ---
void AudioPlayer::ma_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(pDevice->pUserData);
    if (!player)
        return;

    // 计算需要的总字节数
    int bytesPerSample = av_get_bytes_per_sample(player->deviceParams.sampleFormat);
    int channels = player->deviceParams.channels;
    int totalBytesNeeded = frameCount * channels * bytesPerSample;

    uint8_t *outPtr = static_cast<uint8_t *>(pOutput);
    int bytesWritten = 0;

    // 必须全程持有锁
    std::lock_guard<std::mutex> lock(player->audioFrameQueueMutex);

    while (bytesWritten < totalBytesNeeded)
    {
        // 1. 获取数据帧
        if (!player->m_currentFrame)
        {
            if (player->audioFrameQueue.empty())
            {
                // 队列为空，通知解码线程
                player->stateCondVar.notify_one();
                break; // 退出循环，填充静音
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
        {
            break;
        }

        // 实时更新 nowPlayingTime (平滑插值)
        // 基础时间 (帧开始时间) + 偏移时间 (已播放字节对应的时间)
        // 计算公式：偏移字节 / (通道数 * 采样深度) / 采样率 * 1000000
        if (player->deviceParams.sampleRate > 0)
        {
            int64_t bytesAlreadyPlayedInFrame = player->m_currentFramePos;
            int64_t offsetMicroseconds = (bytesAlreadyPlayedInFrame * 1000000) / (channels * bytesPerSample * player->deviceParams.sampleRate);

            player->nowPlayingTime.store(player->m_currentFrame->pts + offsetMicroseconds);
        }

        // 2. 拷贝数据
        size_t frameSize = player->m_currentFrame->data.size();
        int frameRemaining = static_cast<int>(frameSize - player->m_currentFramePos);
        int bytesNeeded = totalBytesNeeded - bytesWritten;
        int copySize = std::min(frameRemaining, bytesNeeded);

        if (copySize > 0)
        {
            memcpy(outPtr + bytesWritten,
                   player->m_currentFrame->data.data() + player->m_currentFramePos,
                   copySize);

            bytesWritten += copySize;
            player->m_currentFramePos += copySize;
        }

        // 3. 帧用尽
        if (player->m_currentFramePos >= frameSize)
        {
            player->m_currentFrame.reset();
        }
    }

    // 4. 如果数据不足，填充静音 (Miniaudio 要求填满 buffer)
    if (bytesWritten < totalBytesNeeded)
    {
        memset(outPtr + bytesWritten, 0, totalBytesNeeded - bytesWritten);
    }
}

// --- Decoder Thread & Logic ---

void AudioPlayer::mainDecodeThread()
{
    AVPacket *packet = av_packet_alloc();

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
            // 初始状态恢复
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (playingState == PlayerState::STOPPED)
                {
                    playingState = isFirstPlay ? PlayerState::PAUSED : oldPlayingState;
                }

                if (m_deviceInited)
                {
                    if (playingState == PlayerState::PLAYING)
                    {
                        ma_device_start(&m_device);
                    }
                    else
                    {
                        ma_device_stop(&m_device);
                    }
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

                if (playingState == PlayerState::SEEKING)
                {
                    handleSeekRequest();
                    continue;
                }

                // 加锁进行解码，防止在参数重设时进行操作
                {
                    std::lock_guard<std::mutex> decodeLock(decodeMutex);
                    decodeAndProcessPacket(packet, isSongLoopActive, playbackFinishedNaturally);
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
    av_packet_free(&packet);
}

bool AudioPlayer::waitForDecodeState()
{
    std::unique_lock<std::mutex> lock(stateMutex);
    stateCondVar.wait(lock, [this]
                      {
                          if (quitFlag.load() || playingState == PlayerState::STOPPED || playingState == PlayerState::SEEKING)
                              return true;
                          if (playingState == PlayerState::PLAYING)
                          {
                              std::lock_guard<std::mutex> qLock(audioFrameQueueMutex);
                              return audioFrameQueue.size() < audioFrameQueueMaxSize.load();
                          }
                          return false; // Paused -> wait
                      });

    return !(quitFlag.load() || playingState == PlayerState::STOPPED);
}

void AudioPlayer::handleSeekRequest()
{
    flushQueue();

    // seek 时也要保护，虽然 seekTarget 由 stateMutex 保护，但操作 ffmpeg 上下文最好也互斥
    std::lock_guard<std::mutex> decodeLock(decodeMutex);

    if (!m_currentSource)
    {
        return;
    }

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
        if (playingState == PlayerState::PLAYING && m_deviceInited)
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
            // Bug Fix 3: Logic for switching songs

            // 情况 A: Mixing 模式且有预加载源 -> 无缝切换
            if (outputMode.load() == OUTPUT_MIXING)
            {
                if (performSeamlessSwitch())
                    return;
            }
            else if (outputMode.load() == OUTPUT_DIRECT)
            {
                // 情况 B: Direct 模式，检查是否有 preloadPath
                // Direct 模式下不能做无缝DSP混音，必须重新打开设备。
                // 这里的策略是：将 preloadPath 移至 currentPath，然后跳出当前 loop，
                // 但设置 playbackFinishedNaturally = false，这样外层循环不会清空 currentPath，
                // 而是再次调用 setupDecodingSession，从而根据新文件重新初始化设备。

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
                    // 关键：不设为 naturally finished，也不清空 currentPath
                    playbackFinishedNaturally = false;
                    isSongLoopActive = false; // 退出内层循环
                    return;
                }
            }

            // 没有下一首，正常结束
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
        if (avcodec_send_packet(m_currentSource->pCodecCtx, packet) >= 0)
        {
            AVFrame *frame = av_frame_alloc();
            while (avcodec_receive_frame(m_currentSource->pCodecCtx, frame) >= 0)
            {
                if (!processFrame(frame))
                {
                    isSongLoopActive = false;
                    break; // Error during processing
                }
            }
            av_frame_free(&frame);
        }
    }
    av_packet_unref(packet);
}

bool AudioPlayer::processFrame(AVFrame *frame)
{
    if (!frame || !m_currentSource)
        return false;

    // 计算当前帧的时长 (秒)
    double frameDurationSec = (double)frame->nb_samples / frame->sample_rate;
    int64_t frameDurationMicro = static_cast<int64_t>(frameDurationSec * 1000000);

    // 确定 PTS
    int64_t ptsMicro = 0;

    // 尝试从 FFmpeg 获取
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
    {
        double ptsSec = frame->best_effort_timestamp * av_q2d(m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base);
        ptsMicro = static_cast<int64_t>(ptsSec * 1000000);

        // 既然拿到了准确时间，同步更新我们的游标，供下一帧（如果丢失PTS）使用
        m_decoderCursor.store(ptsMicro + frameDurationMicro);
    }
    else
    {
        // FFmpeg 没给时间 (APE/WAV 常见情况)，使用我们要维护的游标
        // 绝对不要使用 nowPlayingTime.load() !
        ptsMicro = m_decoderCursor.load();

        // 累加游标
        m_decoderCursor.fetch_add(frameDurationMicro);
    }

    triggerPreload(static_cast<double>(ptsMicro) / 1000000.0);

    // 重采样
    int64_t delay = swr_get_delay(m_currentSource->swrCtx, frame->sample_rate);
    int64_t out_samples = av_rescale_rnd(delay + frame->nb_samples, deviceParams.sampleRate, frame->sample_rate, AV_ROUND_UP);

    // 准备输出 buffer
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

    // 调整实际大小
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
    if (!m_currentSource->initDecoder(path, errorBuffer))
        return false;

    // Direct 模式下这里会根据文件格式打开设备；Mixing 模式下会根据 mixingParams 打开
    if (!openAudioDevice())
    {
        return false;
    }

    // 音量设为 1.0 (由 miniaudio master volume 控制)
    if (!m_currentSource->openSwrContext(deviceParams, 1.0, errorBuffer))
        return false;

    audioDuration.store(m_currentSource->pFormatCtx->duration);
    return true;
}

void AudioPlayer::triggerPreload(double currentPts)
{
    // Direct 模式不支持无缝混音预加载，只支持路径预设，在EOF处处理
    if (outputMode.load() != OUTPUT_MIXING || hasPreloaded.load() || currentPts < 0)
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
        // 预加载必须使用当前设备的参数进行重采样初始化
        if (src->initDecoder(pPath, errorBuffer) && src->openSwrContext(deviceParams, 1.0, errorBuffer))
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
    int avgBytes = static_cast<int>(bytes / frames);
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
    if (outputMode.load() != OUTPUT_MIXING || !hasPreloaded.load() || !m_preloadSource)
        return false;

    applyFadeOutToLastFrame();

    m_currentSource = std::move(m_preloadSource);
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = m_currentSource->path;
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

void AudioPlayer::applyFadeOutToLastFrame()
{
    std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
    if (audioFrameQueue.empty())
        return;

    auto frame = audioFrameQueue.back();
    if (!frame || frame->data.empty())
        return;

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
    return playingState == PlayerState::PLAYING;
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

// --- Static: Waveform ---
// =========================================================
//  SIMD 计算内核
// =========================================================
struct ChunkResult
{
    float sumSquares;
    int actualCount;
};

// Float SIMD
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

// S16 SIMD
static ChunkResult computeSumSquaresAVX2_S16(const int16_t *data, int range_count, int step, int decimation)
{
    float sum = 0.0f;
    int processed = 0;
    int i = 0;
    // 归一化系数：确保 int16 最大值映射到 1.0f
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

// S32 SIMD
static ChunkResult computeSumSquaresAVX2_S32(const int32_t *data, int range_count, int step, int decimation)
{
    float sum = 0.0f;
    int processed = 0;
    int i = 0;
    // 归一化系数：确保 int32 最大值映射到 1.0f
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

// U8 SIMD
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

struct BarData
{
    double sumSquares = 0.0;
    int actualCount = 0;
};

struct SafePacket
{
    AVPacket *pkt;
    SafePacket()
    {
        pkt = av_packet_alloc();
    }
    ~SafePacket()
    {
        av_packet_free(&pkt);
    }
    SafePacket(const SafePacket &) = delete;
    SafePacket &operator=(const SafePacket &) = delete;
    SafePacket(SafePacket &&other) noexcept : pkt(other.pkt)
    {
        other.pkt = nullptr;
    }
    void refFrom(const AVPacket *src)
    {
        av_packet_ref(pkt, src);
    }
};

// =========================================================
//  Part 2: 策略A - 并行 Seek (FLAC/MP3)
// =========================================================
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
    if (streamIdx >= (int)fmt->nb_streams)
    {
        avformat_close_input(&fmt);
        return {};
    }

    AVCodecParameters *param = fmt->streams[streamIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(param->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, param);
    ctx->thread_count = 1;

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return {};
    }

    int decimation = 1;
    if (ctx->sample_rate > 48000)
    {
        decimation = ctx->sample_rate / 32000;
        if (decimation < 1)
            decimation = 1;
    }

    int64_t seekTimestamp = av_rescale_q(absoluteStartSample, (AVRational){1, ctx->sample_rate}, fmt->streams[streamIdx]->time_base);
    av_seek_frame(fmt, streamIdx, seekTimestamp, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int64_t currentGlobalSample = -1;

    while (av_read_frame(fmt, pkt) >= 0)
    {
        if (pkt->stream_index == streamIdx)
        {
            if (avcodec_send_packet(ctx, pkt) >= 0)
            {
                while (avcodec_receive_frame(ctx, frame) >= 0)
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
                        goto cleanup;

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
                    int step = 1;
                    bool isPlanar = av_sample_fmt_is_planar(ctx->sample_fmt);
                    void *rawData = frame->data[0];
                    if (!isPlanar && channels > 1)
                        step = channels;

                    int processedInFrame = 0;
                    int64_t frameBaseSample = currentGlobalSample + offset;

                    int64_t relativeSample = frameBaseSample - globalStartSample;
                    int curBarIdx = static_cast<int>(relativeSample / samplesPerBar);
                    if (curBarIdx < 0)
                        curBarIdx = 0;

                    double nextBarBoundaryRel = (curBarIdx + 1) * samplesPerBar;

                    while (processedInFrame < processSamples && curBarIdx < totalBars)
                    {
                        int64_t currentRelative = (frameBaseSample + processedInFrame) - globalStartSample;
                        int64_t needed = static_cast<int64_t>(nextBarBoundaryRel) - currentRelative;

                        if (needed <= 0)
                            needed = 1;
                        int count = std::min((int64_t)(processSamples - processedInFrame), needed);

                        ChunkResult res = {0.0f, 0};
                        int dataOffset = (offset + processedInFrame) * step;

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
        av_packet_unref(pkt);
    }
cleanup:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return localBars;
}

// =========================================================
//  Part 3: 策略B - 流水线内存解码 (M4A/MP4)
// =========================================================
static std::vector<BarData> processPacketBatch_StrategyB(
    std::vector<AVPacket *> packets,
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
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, codecPar);
    ctx->thread_count = 1;

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&ctx);
        return localBars;
    }

    int decimation = 1;
    if (ctx->sample_rate > 48000)
    {
        decimation = ctx->sample_rate / 32000;
        if (decimation < 1)
            decimation = 1;
    }
    AVFrame *frame = av_frame_alloc();

    for (auto *pkt : packets)
    {
        if (avcodec_send_packet(ctx, pkt) >= 0)
        {
            while (avcodec_receive_frame(ctx, frame) >= 0)
            {
                if (frame->pts == AV_NOPTS_VALUE)
                    continue;

                int64_t ptsSample = av_rescale_q(frame->pts, timeBase, (AVRational){1, ctx->sample_rate});
                int samples = frame->nb_samples;
                int channels = ctx->ch_layout.nb_channels;
                int step = 1;
                bool isPlanar = av_sample_fmt_is_planar(ctx->sample_fmt);
                void *rawData = frame->data[0];
                if (!isPlanar && channels > 1)
                    step = channels;

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

                    ChunkResult res = {0.0f, 0};
                    int dataOffset = processedInFrame * step;

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
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return localBars;
}

// =========================================================
//  Part 4: 主函数
// =========================================================
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
    barWidth = (totalWidth / barCount) - 2;
    if (barWidth < 1)
        barWidth = 1;

    // 1. 打开文件
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filepath.c_str(), nullptr, nullptr) < 0)
        return barHeights;
    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    int streamIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIdx < 0)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    // 2. 计算实际时间范围
    int64_t fileDurationUS = fmt->duration * 1000000 / AV_TIME_BASE;
    if (endTimeUS <= 0 || endTimeUS > fileDurationUS)
    {
        endTimeUS = fileDurationUS;
    }
    if (startTimeUS < 0)
        startTimeUS = 0;
    if (startTimeUS >= endTimeUS)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    int64_t segmentDurationUS = endTimeUS - startTimeUS;
    int sampleRate = fmt->streams[streamIdx]->codecpar->sample_rate;

    int64_t globalStartSample = av_rescale(startTimeUS, sampleRate, 1000000);
    int64_t globalEndSample = av_rescale(endTimeUS, sampleRate, 1000000);
    int64_t totalSegmentSamples = globalEndSample - globalStartSample;

    double samplesPerBar = (double)totalSegmentSamples / barCount;
    if (samplesPerBar < 1)
        samplesPerBar = 1;

    // 策略判断
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

    int threadCount = std::thread::hardware_concurrency();
    if (threadCount < 2)
        threadCount = 2;
    if (segmentDurationUS < 1000000)
        threadCount = 1;

    std::vector<std::future<std::vector<BarData>>> futures;

    // 用于最终数据汇总
    std::vector<BarData> finalBarsData(barCount);

    // === Strategy A: 并行 Seek (FLAC) ===
    if (!useStrategyB)
    {
        avformat_close_input(&fmt);

        int64_t samplesPerThread = totalSegmentSamples / threadCount;

        for (int i = 0; i < threadCount; ++i)
        {
            int64_t tStart = globalStartSample + i * samplesPerThread;
            int64_t tEnd = (i == threadCount - 1) ? globalEndSample : (tStart + samplesPerThread);

            futures.push_back(
                SimpleThreadPool::instance().enqueue(
                    processAudioChunk_StrategyA,
                    filepath, streamIdx, tStart, tEnd, globalStartSample, samplesPerBar, barCount));
        }

        // 汇总结果 A
        for (auto &fut : futures)
        {
            std::vector<BarData> chunkResult = fut.get();
            for (int i = 0; i < barCount; ++i)
            {
                if (chunkResult[i].actualCount > 0)
                {
                    finalBarsData[i] = chunkResult[i];
                }
            }
        }
    }
    // === Strategy B: 流水线读取 (M4A) ===
    else
    {
        AVCodecParameters *codecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(codecPar, fmt->streams[streamIdx]->codecpar);
        AVRational timeBase = fmt->streams[streamIdx]->time_base;

        auto packetStorage = std::make_shared<std::deque<SafePacket>>();
        std::vector<AVPacket *> currentBatch;
        const int BATCH_SIZE = 250;
        currentBatch.reserve(BATCH_SIZE);

        AVPacket *tempPkt = av_packet_alloc();

        int64_t seekTarget = av_rescale_q(startTimeUS, (AVRational){1, 1000000}, timeBase);
        av_seek_frame(fmt, streamIdx, seekTarget, AVSEEK_FLAG_BACKWARD);

        int64_t endPTS = av_rescale_q(endTimeUS, (AVRational){1, 1000000}, timeBase);

        while (av_read_frame(fmt, tempPkt) >= 0)
        {
            if (tempPkt->stream_index == streamIdx)
            {
                if (tempPkt->pts != AV_NOPTS_VALUE && tempPkt->pts > endPTS)
                {
                    av_packet_unref(tempPkt);
                    break;
                }

                packetStorage->emplace_back();
                packetStorage->back().refFrom(tempPkt);
                currentBatch.push_back(packetStorage->back().pkt);

                if (currentBatch.size() >= BATCH_SIZE)
                {
                    futures.push_back(
                        SimpleThreadPool::instance().enqueue(
                            processPacketBatch_StrategyB,
                            currentBatch, codecPar, timeBase, globalStartSample, samplesPerBar, barCount));
                    currentBatch.clear();
                    currentBatch.reserve(BATCH_SIZE);
                }
            }
            av_packet_unref(tempPkt);
        }

        if (!currentBatch.empty())
        {
            futures.push_back(
                SimpleThreadPool::instance().enqueue(
                    processPacketBatch_StrategyB,
                    currentBatch, codecPar, timeBase, globalStartSample, samplesPerBar, barCount));
        }

        av_packet_free(&tempPkt);
        avformat_close_input(&fmt);

        // 汇总结果 B
        for (auto &fut : futures)
        {
            std::vector<BarData> chunkResult = fut.get();
            for (int i = 0; i < barCount; ++i)
            {
                if (chunkResult[i].actualCount > 0)
                {
                    finalBarsData[i].sumSquares += chunkResult[i].sumSquares;
                    finalBarsData[i].actualCount += chunkResult[i].actualCount;
                }
            }
        }
        avcodec_parameters_free(&codecPar);
    }

    // =========================================================
    //  Part 5: 绝对响度算法 (dB Scale)
    // =========================================================

    // 设置分贝范围
    // 0 dBFS 是数字音频最大值
    // -55 dBFS 作为视觉上的"静音底噪" (低于此值显示为 0 或 最小高度)
    const float DB_CEILING = 0.0f;
    const float DB_FLOOR = -55.0f;
    const float DB_RANGE = DB_CEILING - DB_FLOOR;
    const int MIN_HEIGHT_PX = 2; // 最小像素高度，防止断裂感

    for (int i = 0; i < barCount; ++i)
    {
        float visualHeight = 0.0f;

        if (finalBarsData[i].actualCount > 0)
        {
            // 1. 计算 RMS (0.0 ~ 1.0)
            // 这里的 RMS 是线性的，1.0 代表 0 dBFS (满刻度)
            float rms = std::sqrt(finalBarsData[i].sumSquares / finalBarsData[i].actualCount);

            // 防止 log10(0) 导致 -inf
            if (rms < 1e-9f)
                rms = 1e-9f;

            // 2. 转换为 dB
            // 20 * log10(rms)
            float db = 20.0f * std::log10(rms);

            // 3. 映射到视觉范围 [0.0, 1.0]
            // 如果 db > 0 (Clip)，限制为 0
            if (db > DB_CEILING)
                db = DB_CEILING;

            // 如果 db < -55，设为 -55
            if (db < DB_FLOOR)
                db = DB_FLOOR;

            // 线性归一化 dB 值到 0~1
            float normalized = (db - DB_FLOOR) / DB_RANGE;

            // 4. 计算最终高度
            visualHeight = normalized * maxHeight;
        }

        barHeights[i] = static_cast<int>(std::max((float)MIN_HEIGHT_PX, visualHeight));
    }

    return barHeights;
}