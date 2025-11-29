#include "AudioPlayer.hpp"

std::string av_strerror(int errnum)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(errbuf, sizeof(errbuf), errnum);
    return errbuf;
}

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
        return AUDIO_F32SYS; // Fallback
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
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Cannot find stream info for: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }

    audioStreamIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "No audio stream in: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }

    AVCodecParameters *pCodecParameters = pFormatCtx->streams[audioStreamIndex]->codecpar;
    const AVCodec *pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if (!pCodec)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "No decoder found for: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }

    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx)
    {
        avformat_close_input(&pFormatCtx);
        return false;
    }

    if (avcodec_parameters_to_context(pCodecCtx, pCodecParameters) < 0)
    {
        avcodec_free_context(&pCodecCtx);
        avformat_close_input(&pFormatCtx);
        return false;
    }

    if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0)
    {
        avcodec_free_context(&pCodecCtx);
        avformat_close_input(&pFormatCtx);
        return false;
    }

    SDL_Log("Decoder initialized for: %s", path.c_str());
    return true;
}

bool AudioPlayer::AudioStreamSource::openSwrContext(const AudioParams &deviceParams, double volume, char *errorBuffer)
{
    if (!pCodecCtx)
        return false;

    swrCtx = swr_alloc();
    if (!swrCtx)
        return false;

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
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr_init failed: %s", errorBuffer);
        swr_free(&swrCtx);
        return false;
    }
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
    // Default params if not set
    mixingParams.sampleRate = 96000;
    mixingParams.channels = 2;
    mixingParams.sampleFormat = AV_SAMPLE_FMT_FLT;
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
    SDL_Quit();
}

void AudioPlayer::freeResources()
{
    closeAudioDevice();
    m_currentSource.reset();
    m_preloadSource.reset();
    flushQueue();

    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);
    hasPreloaded.store(false);
}

void AudioPlayer::flushQueue()
{
    std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
    std::queue<std::unique_ptr<AudioFrame>> empty;
    std::swap(audioFrameQueue, empty);
    m_currentFrame.reset();
    m_currentFramePos = 0;
}

bool AudioPlayer::isValidAudio(const std::string &path)
{
    AVFormatContext *pFormatCtx = nullptr;
    if (avformat_open_input(&pFormatCtx, path.c_str(), nullptr, nullptr) != 0)
        return false;

    bool found = false;
    if (avformat_find_stream_info(pFormatCtx, nullptr) >= 0)
    {
        if (av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0)
        {
            found = true;
        }
    }
    avformat_close_input(&pFormatCtx);
    return found;
}

bool AudioPlayer::setPath(const std::string &path)
{
    if (!isValidAudio(path))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Invalid audio: %s", path.c_str());
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
    stateCondVar.notify_one(); // Wake decoder to reset logic
    return true;
}

void AudioPlayer::setPreloadPath(const std::string &path)
{
    if (outputMode.load() != OUTPUT_MIXING)
        return;

    if (!isValidAudio(path))
        return;

    std::lock_guard<std::mutex> lock(pathMutex);
    if (path != preloadPath)
    {
        preloadPath = path;
        hasPreloaded.store(false);
        m_preloadSource.reset();
    }
}

void AudioPlayer::play()
{
    std::lock_guard<std::mutex> lock(stateMutex);
    if (playingState != PlayerState::PLAYING)
    {
        isFirstPlay = false;
        playingState = PlayerState::PLAYING;
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
        if (m_audioDeviceID != 0)
        {
            SDL_PauseAudioDevice(m_audioDeviceID, 1);
        }
        stateCondVar.notify_one();
    }
}

void AudioPlayer::seek(int64_t timeMicroseconds)
{
    std::lock_guard<std::mutex> lock(stateMutex);

    // 直接存储微秒值。
    seekTarget.store(timeMicroseconds);

    // 切换状态为 SEEKING，解码线程会处理剩下的逻辑
    playingState = PlayerState::SEEKING;

    // 如果设备已打开，暂停以防爆音
    if (m_audioDeviceID != 0)
    {
        SDL_PauseAudioDevice(m_audioDeviceID, 1);
    }
    stateCondVar.notify_one();
}

void AudioPlayer::setVolume(double vol)
{
    volume = std::max(0.0, std::min(vol, 1.0));
}

void AudioPlayer::closeAudioDevice()
{
    if (m_audioDeviceID != 0)
    {
        SDL_PauseAudioDevice(m_audioDeviceID, 1);
        SDL_CloseAudioDevice(m_audioDeviceID);
        m_audioDeviceID = 0;
    }
}

bool AudioPlayer::openAudioDevice()
{
    closeAudioDevice();

    SDL_AudioSpec desiredSpec, obtainedSpec;
    SDL_zero(desiredSpec);

    if (outputMode.load() == OUTPUT_MIXING)
    {
        desiredSpec.freq = mixingParams.sampleRate;
        desiredSpec.format = toSDLFormat(mixingParams.sampleFormat);
        desiredSpec.channels = mixingParams.channels;
    }
    else
    {
        if (!m_currentSource || !m_currentSource->pCodecCtx)
            return false;

        desiredSpec.freq = m_currentSource->pCodecCtx->sample_rate;
        desiredSpec.format = toSDLFormat(m_currentSource->pCodecCtx->sample_fmt);
        desiredSpec.channels = m_currentSource->pCodecCtx->ch_layout.nb_channels;
    }

    desiredSpec.samples = SDL_AUDIO_BUFFER_SAMPLES;
    desiredSpec.callback = AudioPlayer::sdl2_audio_callback;
    desiredSpec.userdata = this;

    SDL_SetHint(SDL_HINT_APP_NAME, "smallestMusicPlayer");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "AudioPlayback");

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

    SDL_PauseAudioDevice(m_audioDeviceID, 1); // Start paused
    return true;
}

void AudioPlayer::sdl2_audio_callback(void *userdata, Uint8 *stream, int len)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(userdata);
    memset(stream, 0, len);

    int remaining = len;
    Uint8 *streamPos = stream;

    while (remaining > 0)
    {
        if (!player->m_currentFrame)
        {
            std::unique_lock<std::mutex> lock(player->audioFrameQueueMutex);
            if (player->audioFrameQueue.empty())
            {
                // Buffer underrun or nothing to play
                // Signal state thread just in case it's waiting on queue to empty
                player->stateCondVar.notify_one();
                return;
            }
            player->m_currentFrame = std::move(player->audioFrameQueue.front());
            player->audioFrameQueue.pop();
            player->m_currentFramePos = 0;
            if (player->m_currentFrame)
            {
                player->nowPlayingTime.store(static_cast<int64_t>(player->m_currentFrame->pts * 1000000.0));
            }
            lock.unlock();                     // Unlock ASAP
            player->stateCondVar.notify_one(); // Notify decode thread space is available
        }

        if (!player->m_currentFrame)
            break;

        int frameRemaining = player->m_currentFrame->size - player->m_currentFramePos;
        int copySize = std::min(frameRemaining, remaining);

        if (copySize > 0)
        {
            SDL_MixAudioFormat(streamPos,
                               player->m_currentFrame->data.get() + player->m_currentFramePos,
                               player->deviceSpec.format,
                               copySize,
                               static_cast<int>(player->volume * SDL_MIX_MAXVOLUME));

            remaining -= copySize;
            streamPos += copySize;
            player->m_currentFramePos += copySize;
        }

        if (player->m_currentFramePos >= player->m_currentFrame->size)
        {
            player->m_currentFrame.reset();
        }
    }
}

// --- Main Decode Thread ---

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
            bool isSongLoopActive = true;
            bool playbackFinishedNaturally = false;

            // Initial state check
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (playingState == PlayerState::STOPPED)
                {
                    playingState = isFirstPlay ? PlayerState::PAUSED : PlayerState::PLAYING;
                }
                if (playingState == PlayerState::PLAYING && m_audioDeviceID != 0)
                {
                    SDL_PauseAudioDevice(m_audioDeviceID, 0);
                }
            }

            while (isSongLoopActive)
            {
                std::unique_lock<std::mutex> lock(stateMutex);

                // Wait condition:
                // 1. Quit requested
                // 2. Stopped
                // 3. Seeking (needs immediate handling)
                // 4. Playing AND queue has space
                // If Paused, we just wait here.
                stateCondVar.wait(lock, [this]
                                  {
                                      if (quitFlag.load() || playingState == PlayerState::STOPPED || playingState == PlayerState::SEEKING)
                                          return true;
                                      if (playingState == PlayerState::PLAYING)
                                      {
                                          std::lock_guard<std::mutex> qLock(audioFrameQueueMutex);
                                          return audioFrameQueue.size() < audioFrameQueueMaxSize.load();
                                      }
                                      return false; // Stuck in PAUSED, wait for signal
                                  });

                if (quitFlag.load() || playingState == PlayerState::STOPPED)
                {
                    isSongLoopActive = false;
                    continue;
                }

                if (playingState == PlayerState::SEEKING)
                {
                    // Handle Seek
                    flushQueue();

                    int streamIdx = m_currentSource->audioStreamIndex;
                    AVRational tb = m_currentSource->pFormatCtx->streams[streamIdx]->time_base;
                    int64_t stream_ts = av_rescale_q(seekTarget.load(), AV_TIME_BASE_Q, tb);

                    av_seek_frame(m_currentSource->pFormatCtx, streamIdx, stream_ts, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(m_currentSource->pCodecCtx);

                    // Reset SWR context to flush internal buffers
                    if (m_currentSource->swrCtx)
                    {
                        // Not strictly necessary to free, usually swr_init is enough,
                        // but recreating or re-init cleans state
                    }

                    playingState = PlayerState::PLAYING;
                    if (m_audioDeviceID != 0)
                        SDL_PauseAudioDevice(m_audioDeviceID, 0);
                    continue; // Loop back to decode immediately
                }

                if (playingState == PlayerState::PLAYING)
                {
                    lock.unlock(); // Unlock before heavy lifting
                    decodeAndProcessPacket(packet, isSongLoopActive, playbackFinishedNaturally);
                }
            }

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
            // Setup failed
            freeResources();
            std::lock_guard<std::mutex> lock(pathMutex);
            currentPath = "";
        }
    }
    av_packet_free(&packet);
}

void AudioPlayer::decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally)
{
    int ret = av_read_frame(m_currentSource->pFormatCtx, packet);
    if (ret < 0)
    {
        if (ret == AVERROR_EOF)
        {
            // EOF reached. Check if we can switch or if we are done.
            if (performSeamlessSwitch())
            {
                // Seamless switch successful!
                // We do NOT set isSongLoopActive to false.
                // Instead, we return immediately so the loop continues with the new source.
                return;
            }

            // No preload or switch failed, playback finished naturally.
            playbackFinishedNaturally = true;
            isSongLoopActive = false;
        }
        else
        {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Read frame error: %s", av_strerror(ret).c_str());
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
    if (!openAudioDevice())
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
                                                                    frame->best_effort_timestamp * av_q2d(m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base);

    triggerPreload(pts);

    int64_t delay = swr_get_delay(m_currentSource->swrCtx, frame->sample_rate);
    int64_t out_samples = av_rescale_rnd(delay + frame->nb_samples, deviceParams.sampleRate, frame->sample_rate, AV_ROUND_UP);

    int out_bytes_per_sample = av_get_bytes_per_sample(deviceParams.sampleFormat);
    int64_t out_buffer_size = out_samples * deviceParams.channels * out_bytes_per_sample;

    auto audioFrame = std::make_unique<AudioFrame>();
    audioFrame->data.reset((uint8_t *)av_malloc(out_buffer_size));

    if (!audioFrame->data)
        return false;

    uint8_t *out_ptr = audioFrame->data.get();
    int64_t converted = swr_convert(m_currentSource->swrCtx, &out_ptr, out_samples, (const uint8_t **)frame->data, frame->nb_samples);

    if (converted < 0)
        return false;

    audioFrame->size = converted * deviceParams.channels * out_bytes_per_sample;
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
    if (outputMode.load() != OUTPUT_MIXING || hasPreloaded.load() || currentPts < 0)
        return;

    std::string path_to_preload;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        path_to_preload = preloadPath;
    }
    if (path_to_preload.empty())
        return;

    double dur = audioDuration.load() / (double)AV_TIME_BASE;
    if (dur > 0 && (dur - currentPts) < PRELOAD_TRIGGER_SECONDS_BEFORE_END)
    {
        SDL_Log("Triggering preload: %s", path_to_preload.c_str());
        auto src = std::make_unique<AudioStreamSource>();
        if (src->initDecoder(path_to_preload, errorBuffer) && src->openSwrContext(deviceParams, volume, errorBuffer))
        {
            m_preloadSource = std::move(src);
            hasPreloaded.store(true);
        }
        else
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Preload failed");
            // Clear to avoid retry loops
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

    // Simple moving average approximation logic can go here
    // Current implementation just uses average of first 5 frames
    int64_t bytes = totalDecodedBytes.load();
    int64_t frames = totalDecodedFrames.load();
    if (frames == 0)
    {
        hasCalculatedQueueSize.store(false);
        return;
    }

    int avgBytes = bytes / frames;
    if (avgBytes == 0)
    {
        hasCalculatedQueueSize.store(false);
        return;
    }

    int bytesPerSec = deviceParams.sampleRate * deviceParams.channels * out_bytes_per_sample;
    int targetBytes = static_cast<int>(bytesPerSec * AUDIO_BUFFER_DURATION_SECONDS);
    size_t maxF = std::max(MIN_AUDIO_QUEUE_SIZE, targetBytes / avgBytes);

    audioFrameQueueMaxSize.store(maxF);
}

bool AudioPlayer::performSeamlessSwitch()
{
    if (outputMode.load() != OUTPUT_MIXING || !hasPreloaded.load() || !m_preloadSource)
        return false;
    applyFadeOutToLastFrame();
    SDL_Log("Seamless switch -> %s", m_preloadSource->path.c_str());
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
    nowPlayingTime.store(0);
    return true;
}

// --- Getters/Setters ---
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

int64_t AudioPlayer::getAudioDuration() const
{
    return audioDuration.load() / AV_TIME_BASE;
}

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

// --- Waveform Generation (Optimized) ---

std::vector<int> AudioPlayer::buildAudioWaveform(const std::string &filepath,
                                                 int barCount,
                                                 int totalWidth,
                                                 int &barWidth,
                                                 int maxHeight)
{
    std::vector<int> barHeights(barCount, 0);
    if (barCount <= 0 || totalWidth <= 0)
        return barHeights;
    barWidth = totalWidth / barCount;
    if (barWidth <= 0)
        return barHeights;

    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, filepath.c_str(), nullptr, nullptr) < 0)
        return barHeights;
    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    int audioStream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStream < 0)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    AVCodecParameters *codecpar = fmt->streams[audioStream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
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

    // Setup resampling to Float Planar for easy processing
    int sampleRate = codecCtx->sample_rate;
    int channels = codecCtx->ch_layout.nb_channels;

    SwrContext *swr = swr_alloc();
    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout, channels);
    av_channel_layout_default(&out_layout, channels);

    av_opt_set_chlayout(swr, "in_chlayout", &in_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &out_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", sampleRate, 0);
    av_opt_set_int(swr, "out_sample_rate", sampleRate, 0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt", codecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0); // Planar Float
    swr_init(swr);

    double duration = (fmt->duration > 0) ? (double)fmt->duration / AV_TIME_BASE :
                                            (fmt->streams[audioStream]->duration * av_q2d(fmt->streams[audioStream]->time_base));
    if (duration <= 0)
        duration = 1.0;

    double samplesPerBar = (duration * sampleRate) / barCount;
    if (samplesPerBar < 1.0)
        samplesPerBar = 1.0;

    std::vector<float> peaks(barCount, 0.0f);
    long long currentSampleIndex = 0;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    // Buffers
    const int MAX_BUF = 65536;
    uint8_t **out_data = nullptr;
    int out_linesize;
    av_samples_alloc_array_and_samples(&out_data, &out_linesize, channels, MAX_BUF, AV_SAMPLE_FMT_FLTP, 0);

    while (av_read_frame(fmt, pkt) >= 0)
    {
        if (pkt->stream_index == audioStream)
        {
            if (avcodec_send_packet(codecCtx, pkt) >= 0)
            {
                while (avcodec_receive_frame(codecCtx, frame) >= 0)
                {
                    int numSamples = swr_convert(swr, out_data, MAX_BUF, (const uint8_t **)frame->data, frame->nb_samples);

                    if (numSamples > 0)
                    {
                        // Optimized Peak Detection (Standard C++ allows compiler vectorization)
                        for (int i = 0; i < numSamples; ++i)
                        {
                            float maxVal = 0.0f;
                            // Find max across channels
                            for (int c = 0; c < channels; ++c)
                            {
                                float val = std::abs(((float *)out_data[c])[i]);
                                if (val > maxVal)
                                    maxVal = val;
                            }

                            // Map to bar
                            int barIdx = static_cast<int>((currentSampleIndex + i) / samplesPerBar);
                            if (barIdx >= 0 && barIdx < barCount)
                            {
                                if (maxVal > peaks[barIdx])
                                    peaks[barIdx] = maxVal;
                            }
                        }
                        currentSampleIndex += numSamples;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (out_data)
        av_freep(&out_data[0]);
    av_freep(&out_data);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmt);

    for (int i = 0; i < barCount; ++i)
    {
        float h = peaks[i];
        if (h > 1.0f)
            h = 1.0f;
        barHeights[i] = static_cast<int>(h * maxHeight);
    }

    return barHeights;
}

int64_t AudioPlayer::getCurrentPositionMicroseconds() const
{
    // 直接返回原子变量中的微秒值
    return nowPlayingTime.load();
}

// 获取总时长 (毫秒)
int64_t AudioPlayer::getDurationMillisecond() const
{
    // audioDuration 存储的是 FFmpeg 的 AV_TIME_BASE (微秒)
    int64_t duration = audioDuration.load();
    if (duration == AV_NOPTS_VALUE)
        return 0;

    return duration / 1000;
}

// 获取总时长 (微秒)
int64_t AudioPlayer::getDurationMicroseconds() const
{
    // audioDuration 原生就是微秒单位
    int64_t duration = audioDuration.load();
    if (duration == AV_NOPTS_VALUE)
        return 0;

    return duration;
}

void AudioPlayer::applyFadeOutToLastFrame()
{
    std::lock_guard<std::mutex> lock(audioFrameQueueMutex);
    if (audioFrameQueue.empty())
        return;

    // 获取队列中最后一帧（即当前歌曲的最后一帧音频）
    auto &frame = audioFrameQueue.back();
    if (!frame || !frame->data || frame->size <= 0)
        return;

    // 我们假设输出格式是 Float (AV_SAMPLE_FMT_FLT)，这是本播放器的默认设置
    // 如果使用了其他格式（如 S16），需要写对应的处理逻辑，但这里我们主要处理 Float
    if (deviceParams.sampleFormat == AV_SAMPLE_FMT_FLT)
    {
        float *samples = reinterpret_cast<float *>(frame->data.get());
        int totalSamples = frame->size / sizeof(float); // 总样本数（包含所有通道）

        // 对这一整帧进行线性淡出
        // 这一帧通常只有几十毫秒，淡出不会被听觉感知为“声音变小”，但能消除爆音
        for (int i = 0; i < totalSamples; ++i)
        {
            // gain 从 1.0 线性降至 0.0
            float gain = 1.0f - (static_cast<float>(i) / totalSamples);
            samples[i] *= gain;
        }
    }
}