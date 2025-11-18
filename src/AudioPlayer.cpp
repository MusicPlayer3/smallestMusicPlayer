#include "AudioPlayer.hpp"

// --- Constants ---
namespace
{
constexpr double PRELOAD_TRIGGER_SECONDS_BEFORE_END = 10.0;
constexpr double AUDIO_BUFFER_DURATION_SECONDS = 0.2; // 200ms
constexpr int MIN_AUDIO_QUEUE_SIZE = 2;
constexpr int SDL_AUDIO_BUFFER_SAMPLES = 4096;
} // namespace

// --- Format Conversion Utilities ---
static SDL_AudioFormat toSDLFormat(AVSampleFormat ffmpegFormat)
{
    switch (ffmpegFormat)
    {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P: return AUDIO_U8;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: return AUDIO_S16SYS;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: return AUDIO_S32SYS;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: return AUDIO_F32SYS;
    default:
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Unsupported FFmpeg sample format: %s", av_get_sample_fmt_name(ffmpegFormat));
        return 0;
    }
}

static AVSampleFormat toAVSampleFormat(SDL_AudioFormat sdlFormat)
{
    switch (sdlFormat)
    {
    case AUDIO_U8: return AV_SAMPLE_FMT_U8;
    case AUDIO_S16SYS: return AV_SAMPLE_FMT_S16;
    case AUDIO_S32SYS: return AV_SAMPLE_FMT_S32;
    case AUDIO_F32SYS: return AV_SAMPLE_FMT_FLT;
    default: return AV_SAMPLE_FMT_NONE;
    }
}

static AVChannelLayout toAVChannelLayout(const uint8_t &layout)
{
    switch (layout)
    {
    case 1: return AV_CHANNEL_LAYOUT_MONO;
    case 2: return AV_CHANNEL_LAYOUT_STEREO;
    case 3: return AV_CHANNEL_LAYOUT_SURROUND;
    case 4: return AV_CHANNEL_LAYOUT_4POINT0;
    case 5: return AV_CHANNEL_LAYOUT_5POINT0_BACK;
    case 6: return AV_CHANNEL_LAYOUT_5POINT1_BACK;
    case 7: return AV_CHANNEL_LAYOUT_7POINT1_WIDE_BACK;
    default: return AV_CHANNEL_LAYOUT_STEREO;
    }
}

// --- AudioStreamSource Implementation (Fixed) ---
void AudioPlayer::AudioStreamSource::free()
{
    if (swrCtx)
    {
        swr_free(&swrCtx); // 修复：使用swr_free正确释放重采样器
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
    path = "";
}

bool AudioPlayer::AudioStreamSource::initDecoder(const std::string &inputPath, char *errorBuffer)
{
    if (inputPath.empty())
        return false;
    path = inputPath;

    if (avformat_open_input(&pFormatCtx, path.c_str(), nullptr, nullptr) != 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Cannot open audio file: %s", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Cannot find stream information for: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }

    audioStreamIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Cannot find an audio stream in: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }

    AVCodecParameters *pCodecParameters = pFormatCtx->streams[audioStreamIndex]->codecpar;
    const AVCodec *pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if (!pCodec)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Cannot find decoder for: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }

    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Cannot allocate decoder context for: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }

    if (avcodec_parameters_to_context(pCodecCtx, pCodecParameters) < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Cannot copy codec parameters to context for: %s", path.c_str());
        avcodec_free_context(&pCodecCtx);
        avformat_close_input(&pFormatCtx);
        return false;
    }

    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Cannot open decoder for: %s", path.c_str());
        avcodec_free_context(&pCodecCtx);
        avformat_close_input(&pFormatCtx);
        return false;
    }

    SDL_Log("Decoder initialized successfully for: %s", path.c_str());
    return true;
}

bool AudioPlayer::AudioStreamSource::openSwrContext(const AudioParams &deviceParams, double volume, char *errorBuffer)
{
    if (!pCodecCtx)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "SwrContext open failed: Codec context is null for %s.", path.c_str());
        return false;
    }

    swrCtx = swr_alloc();
    if (!swrCtx)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr_alloc failed!");
        return false;
    }

    av_opt_set_chlayout(swrCtx, "in_chlayout", &(pCodecCtx->ch_layout), 0);
    av_opt_set_int(swrCtx, "in_sample_rate", pCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", pCodecCtx->sample_fmt, 0);

    av_opt_set_chlayout(swrCtx, "out_chlayout", &(deviceParams.ch_layout), 0);
    av_opt_set_int(swrCtx, "out_sample_rate", deviceParams.sampleRate, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", deviceParams.sampleFormat, 0);
    av_opt_set_double(swrCtx, "out_volume", volume, 0);

    int ret = swr_init(swrCtx);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, AV_ERROR_MAX_STRING_SIZE * 2);
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr_init failed! Error: %s", errorBuffer);
        swr_free(&swrCtx);
        return false;
    }

    SDL_Log("SwrContext initialized successfully for: %s", path.c_str());
    return true;
}

// --- AudioPlayer Implementation ---

AudioPlayer::AudioPlayer()
{
    if (SDL_Init(SDL_INIT_AUDIO) != 0)
    {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << '\n';
        exit(1);
    }
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

    freeResources(); // Final cleanup
    SDL_Quit();
}

void AudioPlayer::freeResources()
{
    if (m_audioDeviceID != 0)
    {
        SDL_PauseAudioDevice(m_audioDeviceID, 1);
        SDL_CloseAudioDevice(m_audioDeviceID);
        m_audioDeviceID = 0;
    }

    m_currentSource.reset();
    m_preloadSource.reset();

    {
        std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
        std::queue<std::unique_ptr<AudioFrame>> emptyQueue;
        audioFrameQueue.swap(emptyQueue);
        m_currentFrame.reset();
        m_currentFramePos = 0;
    }

    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);
    hasPreloaded.store(false);
}

bool AudioPlayer::isValidAudio(const std::string &path)
{
    AVFormatContext *pFormatCtx = nullptr;
    if (avformat_open_input(&pFormatCtx, path.c_str(), nullptr, nullptr) != 0)
    {
        return false;
    }
    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0)
    {
        avformat_close_input(&pFormatCtx);
        return false;
    }
    int audioStreamIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    avformat_close_input(&pFormatCtx);
    return audioStreamIndex >= 0;
}

bool AudioPlayer::setPath(const std::string &path)
{
    if (!isValidAudio(path))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SetPath failed: Invalid audio file %s", path.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = path;
        preloadPath = "";
        hasPreloaded.store(false);
        m_preloadSource.reset();
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        playingState = PlayerState::STOPPED;
    }

    pathCondVar.notify_one();
    stateCondVar.notify_one();
    SDL_Log("Set new path: %s", path.c_str());
    return true;
}

void AudioPlayer::setPreloadPath(const std::string &path)
{
    if (outputMode.load() != OUTPUT_MIXING)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Preloading is only available in MIXING mode.");
        return;
    }
    if (!isValidAudio(path))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Invalid preload path: %s", path.c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(pathMutex);
    if (path != preloadPath)
    {
        preloadPath = path;
        hasPreloaded.store(false);
        m_preloadSource.reset();
        SDL_Log("Set preload path: %s", preloadPath.c_str());
    }
}

void AudioPlayer::play()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (playingState != PlayerState::PLAYING)
    {
        isFirstPlay = false;
        playingState = PlayerState::PLAYING;

        // 优化：立即解除设备暂停，以实现即时播放
        if (m_audioDeviceID != 0)
        {
            SDL_PauseAudioDevice(m_audioDeviceID, 0);
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

        // 优化：立即暂停设备，以实现即时暂停
        if (m_audioDeviceID != 0)
        {
            SDL_PauseAudioDevice(m_audioDeviceID, 1);
        }

        stateCondVar.notify_one();
    }
}

void AudioPlayer::seek(int64_t time)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    seekTarget.store(static_cast<int64_t>(time * AV_TIME_BASE));
    playingState = PlayerState::SEEKING;
    stateCondVar.notify_one();
    SDL_Log("Seek command issued to %ld seconds", time);
}

void AudioPlayer::setVolume(double vol)
{
    volume = std::max(0.0, std::min(vol, 1.0));
    if (m_currentSource && m_currentSource->swrCtx)
    {
        av_opt_set_double(m_currentSource->swrCtx, "out_volume", volume, 0);
    }
    if (m_preloadSource && m_preloadSource->swrCtx)
    {
        av_opt_set_double(m_preloadSource->swrCtx, "out_volume", volume, 0);
    }
}

bool AudioPlayer::openAudioDevice()
{
    if (m_audioDeviceID != 0)
    {
        SDL_CloseAudioDevice(m_audioDeviceID);
    }

    SDL_AudioSpec desiredSpec, obtainedSpec;
    SDL_zero(desiredSpec);

    if (outputMode.load() == OUTPUT_MIXING)
    {
        desiredSpec.freq = mixingParams.sampleRate;
        desiredSpec.format = toSDLFormat(mixingParams.sampleFormat);
        desiredSpec.channels = mixingParams.ch_layout.nb_channels;
    }
    else
    {
        if (!m_currentSource || !m_currentSource->pCodecCtx)
        {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Cannot open device in DIRECT mode: no codec context.");
            return false;
        }
        desiredSpec.freq = m_currentSource->pCodecCtx->sample_rate;
        desiredSpec.format = toSDLFormat(m_currentSource->pCodecCtx->sample_fmt);
        desiredSpec.channels = m_currentSource->pCodecCtx->ch_layout.nb_channels;
    }

    desiredSpec.samples = SDL_AUDIO_BUFFER_SAMPLES;
    desiredSpec.callback = AudioPlayer::sdl2_audio_callback;
    desiredSpec.userdata = this;

    m_audioDeviceID = SDL_OpenAudioDevice(nullptr, 0, &desiredSpec, &obtainedSpec, 0);
    if (m_audioDeviceID == 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to open audio device: %s", SDL_GetError());
        return false;
    }

    deviceParams.sampleRate = obtainedSpec.freq;
    deviceParams.sampleFormat = toAVSampleFormat(obtainedSpec.format);
    deviceParams.ch_layout = toAVChannelLayout(obtainedSpec.channels);
    deviceParams.channels = obtainedSpec.channels;
    deviceSpec = obtainedSpec;

    // 优化：默认保持暂停，直到 play() 被调用
    SDL_PauseAudioDevice(m_audioDeviceID, 1);
    return true;
}

void AudioPlayer::sdl2_audio_callback(void *userdata, Uint8 *stream, int len)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(userdata);
    memset(stream, 0, len);

    int remaining = len;
    Uint8 *streamPos = stream;
    bool needsNotify = false;

    while (remaining > 0)
    {
        if (!player->m_currentFrame)
        {
            std::unique_lock<std::mutex> lock(player->audioFrameQueueMutex);
            if (player->audioFrameQueue.empty())
            {
                if (needsNotify)
                    player->stateCondVar.notify_one();
                return;
            }
            player->m_currentFrame = std::move(player->audioFrameQueue.front());
            player->audioFrameQueue.pop();
            player->m_currentFramePos = 0;
            player->nowPlayingTime.store(static_cast<int64_t>(player->m_currentFrame->pts));
            needsNotify = true;
        }

        int frameRemaining = player->m_currentFrame->size - player->m_currentFramePos;
        int copySize = std::min(frameRemaining, remaining);

        SDL_MixAudioFormat(streamPos,
                           player->m_currentFrame->data.get() + player->m_currentFramePos,
                           player->deviceSpec.format,
                           copySize,
                           static_cast<int>(player->volume * SDL_MIX_MAXVOLUME));

        remaining -= copySize;
        streamPos += copySize;
        player->m_currentFramePos += copySize;

        if (player->m_currentFramePos >= player->m_currentFrame->size)
        {
            player->m_currentFrame.reset();
        }
    }

    if (needsNotify)
    {
        player->stateCondVar.notify_one();
    }
}

// --- Main Decode Thread and Helpers ---

void AudioPlayer::mainDecodeThread()
{
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
            bool playbackFinishedNaturally = false;
            bool isSongLoopActive = true;

            AVPacket *packet = av_packet_alloc();
            if (!packet)
            {
                SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to allocate AVPacket");
                isSongLoopActive = false;
            }

            {
                std::lock_guard<std::mutex> lock(stateMutex);
                playingState = isFirstPlay ? PlayerState::PAUSED : PlayerState::PLAYING;

                // 优化：如果非首次播放，立即取消暂停设备
                if (!isFirstPlay && m_audioDeviceID != 0)
                {
                    SDL_PauseAudioDevice(m_audioDeviceID, 0);
                }
            }
            stateCondVar.notify_one(); // Notify to handle initial state

            // === REFACTORED DECODING LOOP ===
            while (isSongLoopActive)
            {
                std::unique_lock<std::mutex> lock(stateMutex);

                // Wait until there's work to do
                stateCondVar.wait(lock, [this]
                                  {
                    if (quitFlag.load() || playingState == PlayerState::STOPPED || playingState == PlayerState::SEEKING) {
                        return true;
                    }
                    if (playingState == PlayerState::PLAYING) {
                        std::lock_guard<std::mutex> queueLock(audioFrameQueueMutex);
                        return audioFrameQueue.size() < audioFrameQueueMaxSize.load();
                    }
                    // For PAUSED state, we wait, so return false
                    return false; });

                if (quitFlag.load() || playingState == PlayerState::STOPPED)
                {
                    isSongLoopActive = false;
                    continue;
                }

                if (playingState == PlayerState::SEEKING)
                {
                    SDL_PauseAudioDevice(m_audioDeviceID, 1);
                    {
                        std::lock_guard<std::mutex> queueLock(audioFrameQueueMutex);
                        std::queue<std::unique_ptr<AudioFrame>> emptyQueue;
                        audioFrameQueue.swap(emptyQueue);
                        m_currentFrame.reset();
                        m_currentFramePos = 0;
                    }

                    AVRational tb = m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base;
                    int64_t stream_ts = av_rescale_q(seekTarget.load(), AV_TIME_BASE_Q, tb);
                    av_seek_frame(m_currentSource->pFormatCtx, m_currentSource->audioStreamIndex, stream_ts, AVSEEK_FLAG_BACKWARD);

                    avcodec_flush_buffers(m_currentSource->pCodecCtx);
                    if (m_currentSource->swrCtx)
                        swr_init(m_currentSource->swrCtx);

                    playingState = PlayerState::PLAYING;
                    // 优化：在seek后立即取消暂停，以继续播放
                    SDL_PauseAudioDevice(m_audioDeviceID, 0);
                }

                // 优化：移除了多余的 SDL_PauseAudioDevice 调用
                // play() 和 pause() 函数现在负责立即控制设备状态
                if (playingState == PlayerState::PLAYING)
                {
                    lock.unlock();
                    decodeAndProcessPacket(packet, isSongLoopActive, playbackFinishedNaturally);
                }
                else
                {
                    // 处于 PAUSED 状态时，线程会在此处阻塞在 stateCondVar.wait()
                    // 设备已由 pause() 函数暂停，所以此处无需操作
                }
            } // End of single song loop

            av_packet_free(&packet);
            freeResources();

            if (playbackFinishedNaturally)
            {
                std::lock_guard<std::mutex> lock(pathMutex);
                currentPath = "";
                preloadPath = "";
            }
        }
        else
        {
            freeResources();
        }
    }
}

void AudioPlayer::decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally)
{
    if (!packet)
    {
        isSongLoopActive = false;
        return;
    }

    int ret = av_read_frame(m_currentSource->pFormatCtx, packet);
    if (ret < 0)
    {
        if (ret == AVERROR_EOF)
        {
            std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
            if (audioFrameQueue.empty() && !m_currentFrame)
            {
                if (!performSeamlessSwitch())
                {
                    playbackFinishedNaturally = true;
                }
                isSongLoopActive = false;
            }
        }
        else
        {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error reading frame: %s", av_err2str(ret));
            isSongLoopActive = false;
        }
        return;
    }

    if (packet->stream_index == m_currentSource->audioStreamIndex)
    {
        if (avcodec_send_packet(m_currentSource->pCodecCtx, packet) >= 0)
        {
            AVFrame *frame = av_frame_alloc();
            if (!frame)
            {
                av_packet_unref(packet);
                isSongLoopActive = false;
                return;
            }

            while (avcodec_receive_frame(m_currentSource->pCodecCtx, frame) >= 0)
            {
                if (!processFrame(frame))
                {
                    isSongLoopActive = false;
                    break;
                }
            }
            av_frame_free(&frame);
        }
    }
    av_packet_unref(packet);
}

bool AudioPlayer::setupDecodingSession(const std::string &path)
{
    m_currentSource = std::make_unique<AudioStreamSource>();
    if (!m_currentSource->initDecoder(path, errorBuffer))
        return false;

    if (!openAudioDevice()) // openAudioDevice 内部已设置为默认暂停
        return false;

    if (!m_currentSource->openSwrContext(deviceParams, volume, errorBuffer))
        return false;

    audioDuration.store(m_currentSource->pFormatCtx->duration);
    return true;
}

bool AudioPlayer::processFrame(AVFrame *frame)
{
    if (!frame || !m_currentSource)
        return false;

    double pts = (frame->best_effort_timestamp == AV_NOPTS_VALUE) ? -1.0 :
                                                                    static_cast<double>(frame->best_effort_timestamp) * av_q2d(m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base);

    triggerPreload(pts);

    int64_t out_samples = av_rescale_rnd(
        swr_get_delay(m_currentSource->swrCtx, frame->sample_rate) + frame->nb_samples,
        deviceParams.sampleRate, frame->sample_rate, AV_ROUND_UP);

    int out_bytes_per_sample = av_get_bytes_per_sample(deviceParams.sampleFormat);
    int64_t out_buffer_size = out_samples * deviceParams.channels * out_bytes_per_sample;

    auto audioFrame = std::make_unique<AudioFrame>();
    audioFrame->data.reset((uint8_t *)av_malloc(out_buffer_size));
    if (!audioFrame->data)
        return false;

    uint8_t *out_buffer_ptr = audioFrame->data.get();
    int64_t converted_samples = swr_convert(m_currentSource->swrCtx, &out_buffer_ptr, out_samples,
                                            (const uint8_t **)frame->data, frame->nb_samples);

    if (converted_samples < 0)
        return false;

    audioFrame->size = converted_samples * deviceParams.channels * out_bytes_per_sample;
    audioFrame->pts = pts;

    totalDecodedBytes.fetch_add(audioFrame->size);
    totalDecodedFrames.fetch_add(1);
    calculateQueueSize(out_bytes_per_sample);

    {
        std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
        audioFrameQueue.push(std::move(audioFrame));
    }

    return true;
}

void AudioPlayer::triggerPreload(double currentPts)
{
    if (outputMode.load() != OUTPUT_MIXING || hasPreloaded.load() || currentPts < 0 || !m_currentSource)
        return;

    std::string path_to_preload;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        path_to_preload = preloadPath;
    }

    if (path_to_preload.empty())
        return;

    double durationSec = audioDuration.load() / (double)AV_TIME_BASE;
    if (durationSec > 0 && (durationSec - currentPts) < PRELOAD_TRIGGER_SECONDS_BEFORE_END)
    {
        SDL_Log("Preloading track: %s", path_to_preload.c_str());

        auto preloadedSource = std::make_unique<AudioStreamSource>();
        if (preloadedSource->initDecoder(path_to_preload, errorBuffer) && preloadedSource->openSwrContext(deviceParams, volume, errorBuffer))
        {
            m_preloadSource = std::move(preloadedSource);
            hasPreloaded.store(true);
            SDL_Log("Preload successful for: %s", path_to_preload.c_str());
        }
        else
        {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to preload track: %s", path_to_preload.c_str());
            std::lock_guard<std::mutex> lock(pathMutex);
            preloadPath = "";
        }
    }
}

void AudioPlayer::calculateQueueSize(int out_bytes_per_sample)
{
    bool expected = false;
    if (!hasCalculatedQueueSize.compare_exchange_strong(expected, true))
        return;

    if (totalDecodedFrames.load() < 5)
    {
        hasCalculatedQueueSize.store(false);
        return;
    }

    int64_t totalBytes = totalDecodedBytes.load();
    int64_t totalFrames = totalDecodedFrames.load();
    if (totalFrames == 0)
    {
        hasCalculatedQueueSize.store(false);
        return;
    }

    int avgFrameBytes = static_cast<int>(totalBytes / totalFrames);
    if (avgFrameBytes == 0)
    {
        hasCalculatedQueueSize.store(false);
        return;
    }

    int bytesPerSecond = deviceParams.sampleRate * deviceParams.channels * out_bytes_per_sample;
    int targetBufferBytes = static_cast<int>(bytesPerSecond * AUDIO_BUFFER_DURATION_SECONDS);
    size_t maxFrames = std::max(MIN_AUDIO_QUEUE_SIZE, targetBufferBytes / avgFrameBytes);

    audioFrameQueueMaxSize.store(maxFrames);

    SDL_Log("Auto-calculated audio queue size: %zu frames", maxFrames);
}

bool AudioPlayer::performSeamlessSwitch()
{
    if (outputMode.load() != OUTPUT_MIXING || !hasPreloaded.load() || !m_preloadSource)
        return false;

    SDL_Log("Performing seamless switch to: %s", m_preloadSource->path.c_str());

    m_currentSource = std::move(m_preloadSource);

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = m_currentSource->path;
        preloadPath = "";
    }

    audioDuration.store(m_currentSource->pFormatCtx->duration);
    hasPreloaded.store(false);
    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);

    return true;
}

// --- Public Getters ---
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
    return nowPlayingTime.load();
}

int64_t AudioPlayer::getAudioDuration() const
{
    return audioDuration.load() / AV_TIME_BASE;
}

// --- Unimplemented Stubs ---
void AudioPlayer::setMixingParameters(const AudioParams &params)
{
    mixingParams = params;
}

AudioParams AudioPlayer::getMixingParameters() const
{
    return mixingParams;
}

void AudioPlayer::setOutputMode(outputMod mode)
{
    outputMode.store(mode);
}

std::vector<int> AudioPlayer::buildAudioWaveform(const std::string &filepath, int barCount, int totalWidth, int &barWidth, int maxHeight)
{
    std::vector<int> barHeights(barCount, 0);
    if (barCount <= 0 || totalWidth <= 0)
        return barHeights;
    barWidth = totalWidth / barCount;
    if (barWidth <= 0)
        return barHeights;

    // ---------- FFmpeg open ----------
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filepath.c_str(), nullptr, nullptr) < 0)
        return barHeights;
    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    int audioStream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
    {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioStream = i;
            break;
        }
    }
    if (audioStream < 0)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    AVCodecParameters *codecpar = fmt->streams[audioStream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }
    avcodec_parameters_to_context(codecCtx, codecpar);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmt);
        return barHeights;
    }

    int sampleRate = codecCtx->sample_rate;
    int channels = codecCtx->ch_layout.nb_channels;
    if (channels <= 0)
        channels = 1;

    // ---------- duration & sample mapping ----------
    double durationSec = (fmt->duration > 0) ? (double)fmt->duration / AV_TIME_BASE : 0.0;
    if (durationSec <= 0.0)
    {
        // fallback to stream duration estimate
        if (fmt->streams[audioStream]->duration > 0)
            durationSec = fmt->streams[audioStream]->duration * av_q2d(fmt->streams[audioStream]->time_base);
    }
    if (durationSec <= 0.0)
        durationSec = 1.0; // 最小保护

    // 总采样点数估计（可能非整数）与每bar对应样本数
    double totalSamplesEst = durationSec * sampleRate;
    if (totalSamplesEst < 1.0)
        totalSamplesEst = 1.0;
    double samplesPerBar = totalSamplesEst / (double)barCount;
    double invSamplesPerBar = 1.0 / samplesPerBar;

    std::vector<float> peaks(barCount, 0.0f); // 存储每个bar的峰值（0..1）

    // ---------- swr: 输出 planar float (AV_SAMPLE_FMT_FLT_PLANAR) ----------
    SwrContext *swr = swr_alloc();
    if (!swr)
    {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmt);
        return barHeights;
    }

    // 输入布局（来自解码器）
    AVChannelLayout in_ch_layout;
    av_channel_layout_default(&in_ch_layout, channels);

    // 输出布局（与输入相同，只是格式变成 planar float）
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, channels);

    // 设置输入/输出参数
    av_opt_set_chlayout(swr, "in_chlayout", &in_ch_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &out_ch_layout, 0);

    av_opt_set_int(swr, "in_sample_rate", sampleRate, 0);
    av_opt_set_int(swr, "out_sample_rate", sampleRate, 0);

    av_opt_set_sample_fmt(swr, "in_sample_fmt", codecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    // 初始化
    if (swr_init(swr) < 0)
    {
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmt);
        return barHeights;
    }

    // 固定缓冲以降低分配开销
    const int MAX_SAMPLES = 65536; // 每次 swr 输出的最大样本数（每声道）
    // we allocate a single contiguous buffer for all channels: channels * MAX_SAMPLES floats
    float *planarBuf = (float *)av_malloc(sizeof(float) * (size_t)channels * MAX_SAMPLES);
    if (!planarBuf)
    {
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmt);
        return barHeights;
    }
    // 创建 channel 指针数组指向 planarBuf
    uint8_t **outPtrs = (uint8_t **)av_mallocz(sizeof(uint8_t *) * channels);
    std::vector<float *> outFloatPtrs(channels);
    for (int c = 0; c < channels; ++c)
    {
        outFloatPtrs[c] = planarBuf + (size_t)c * MAX_SAMPLES;
        outPtrs[c] = (uint8_t *)(outFloatPtrs[c]);
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame)
    {
        if (pkt)
            av_packet_free(&pkt);
        if (frame)
            av_frame_free(&frame);
        av_free(planarBuf);
        av_free(outPtrs);
        swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmt);
        return barHeights;
    }

    // ---------- CPU feature detection (runtime) ----------
    bool haveAVX = false, haveSSE = false;
#if defined(__GNUC__) || defined(__clang__)
// __builtin_cpu_supports works on x86/x86_64 with GCC/Clang on Linux
#if defined(__x86_64__) || defined(__i386__)
    haveAVX = __builtin_cpu_supports("avx");
    // prefer AVX2 if available (avx2 covers more)
    haveAVX = haveAVX && __builtin_cpu_supports("avx2");
    haveSSE = __builtin_cpu_supports("sse4.1") || __builtin_cpu_supports("sse2");
#endif
#endif

    // ---------- 解码并采样 ----------
    // 用 long long 记录全局样本计数（以声道样本为单位，即每采样点计为 1）
    // 这里 sampleCounter 表示已处理样本数（每声道一个样本计为 1），用于计算 bar 索引
    long long sampleCounter = 0;

    while (av_read_frame(fmt, pkt) >= 0)
    {
        if (pkt->stream_index != audioStream)
        {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(codecCtx, pkt) < 0)
        {
            av_packet_unref(pkt);
            continue;
        }

        while (avcodec_receive_frame(codecCtx, frame) == 0)
        {
            // 将 frame 重采样为 planar float，outSamples 是每声道样本数
            int outSamples = swr_convert(swr, outPtrs, MAX_SAMPLES, (const uint8_t **)frame->data, frame->nb_samples);
            if (outSamples <= 0)
                continue;

            // 对于每个声道 outFloatPtrs[c][i] 是第 i 个样本的 float 值（可能负）
            // 我们需要计算每个 i 的 value = max_c fabs(out[c][i])
            // 然后将 value 映射到 bar index 并更新 peaks[index] = max(peaks[index], value)

            // 优化思路：使用 SIMD 批量计算 value（每次处理 chunk 个样本），然后标量更新 peaks。
            const int CHUNK = haveAVX ? 8 : (haveSSE ? 4 : 1); // AVX: 8 floats, SSE:4 floats
            int i = 0;
            int samples = outSamples;

            if (channels == 1)
            {
                // 单声道：直接对 outFloatPtrs[0] 做 abs + SIMD
                float *ch0 = outFloatPtrs[0];
                if (haveAVX)
                {
                    // AVX 路径
                    const __m256 signMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
                    for (; i + 8 <= samples; i += 8)
                    {
                        __m256 v = _mm256_loadu_ps(ch0 + i);
                        v = _mm256_and_ps(v, signMask); // abs
                        // 将 v 拆成 8 标量更新 peaks
                        float buf[8];
                        _mm256_storeu_ps(buf, v);
                        for (int j = 0; j < 8; ++j)
                        {
                            long long idxLL = (long long)((sampleCounter + i + j) * invSamplesPerBar);
                            int idx = (int)idxLL;
                            if (idx >= 0 && idx < barCount)
                            {
                                if (buf[j] > peaks[idx])
                                    peaks[idx] = buf[j];
                            }
                        }
                    }
                }
                else if (haveSSE)
                {
                    // SSE 路径 (4 floats)
                    const __m128 signMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
                    for (; i + 4 <= samples; i += 4)
                    {
                        __m128 v = _mm_loadu_ps(ch0 + i);
                        v = _mm_and_ps(v, signMask);
                        float buf[4];
                        _mm_storeu_ps(buf, v);
                        for (int j = 0; j < 4; ++j)
                        {
                            long long idxLL = (long long)((sampleCounter + i + j) * invSamplesPerBar);
                            int idx = (int)idxLL;
                            if (idx >= 0 && idx < barCount)
                            {
                                if (buf[j] > peaks[idx])
                                    peaks[idx] = buf[j];
                            }
                        }
                    }
                }
                // 标量剩余
                for (; i < samples; ++i)
                {
                    float v = fabsf(ch0[i]);
                    int idx = (int)((sampleCounter + i) * invSamplesPerBar);
                    if (idx >= 0 && idx < barCount && v > peaks[idx])
                        peaks[idx] = v;
                }
            }
            else if (channels == 2)
            {
                // 立体声：常见场景。value = max(abs(L), abs(R))
                float *ch0 = outFloatPtrs[0];
                float *ch1 = outFloatPtrs[1];

                if (haveAVX)
                {
                    const __m256 signMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
                    for (; i + 8 <= samples; i += 8)
                    {
                        __m256 a = _mm256_loadu_ps(ch0 + i);
                        __m256 b = _mm256_loadu_ps(ch1 + i);
                        a = _mm256_and_ps(a, signMask);
                        b = _mm256_and_ps(b, signMask);
                        __m256 m = _mm256_max_ps(a, b); // 8 values
                        float buf[8];
                        _mm256_storeu_ps(buf, m);
                        for (int j = 0; j < 8; ++j)
                        {
                            int idx = (int)((sampleCounter + i + j) * invSamplesPerBar);
                            if (idx >= 0 && idx < barCount && buf[j] > peaks[idx])
                                peaks[idx] = buf[j];
                        }
                    }
                }
                else if (haveSSE)
                {
                    const __m128 signMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
                    for (; i + 4 <= samples; i += 4)
                    {
                        __m128 a = _mm_loadu_ps(ch0 + i);
                        __m128 b = _mm_loadu_ps(ch1 + i);
                        a = _mm_and_ps(a, signMask);
                        b = _mm_and_ps(b, signMask);
                        __m128 m = _mm_max_ps(a, b);
                        float buf[4];
                        _mm_storeu_ps(buf, m);
                        for (int j = 0; j < 4; ++j)
                        {
                            int idx = (int)((sampleCounter + i + j) * invSamplesPerBar);
                            if (idx >= 0 && idx < barCount && buf[j] > peaks[idx])
                                peaks[idx] = buf[j];
                        }
                    }
                }
                // 标量剩余
                for (; i < samples; ++i)
                {
                    float v0 = fabsf(ch0[i]);
                    float v1 = fabsf(ch1[i]);
                    float v = (v0 > v1) ? v0 : v1;
                    int idx = (int)((sampleCounter + i) * invSamplesPerBar);
                    if (idx >= 0 && idx < barCount && v > peaks[idx])
                        peaks[idx] = v;
                }
            }
            else
            {
                // 多声道（>2）：使用逐通道取最大值，SIMD 提速有限，采用每通道 SIMD abs 然后标量合并
                // 我们先对每个通道按块取 abs 存到临时 buf，然后合并取 max（标量合并）
                // 这里仍对每通道使用 SIMD 加速 abs，如果可用
                std::vector<float> tmpBuf(CHUNK > 1 ? CHUNK : 1); // 临时小缓冲，用于逐样本合并
                for (; i < samples; ++i)
                {
                    float maxCh = 0.0f;
                    for (int c = 0; c < channels; ++c)
                    {
                        float v = fabsf(outFloatPtrs[c][i]);
                        if (v > maxCh)
                            maxCh = v;
                    }
                    int idx = (int)((sampleCounter + i) * invSamplesPerBar);
                    if (idx >= 0 && idx < barCount && maxCh > peaks[idx])
                        peaks[idx] = maxCh;
                }
            }

            // 更新全局样本计数
            sampleCounter += samples;
        } // recv_frame
        av_packet_unref(pkt);
    } // read_frame

    // ---------- 映射 peaks -> 像素高度 ----------
    for (int b = 0; b < barCount; ++b)
    {
        float v = peaks[b];
        if (v > 1.0f)
            v = 1.0f;
        if (v < 0.0f)
            v = 0.0f;
        barHeights[b] = (int)(v * (float)maxHeight + 0.5f);
    }

    // ---------- cleanup ----------
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_free(planarBuf);
    av_free(outPtrs);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmt);

    return barHeights;
}