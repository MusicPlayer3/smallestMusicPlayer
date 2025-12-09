#include "AudioPlayer.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
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
constexpr double AUDIO_BUFFER_DURATION_SECONDS = 0.2; // 200ms
constexpr int MIN_AUDIO_QUEUE_SIZE = 4;
constexpr int SDL_AUDIO_BUFFER_SAMPLES = 4096;
} // namespace

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
    default: return AUDIO_F32SYS;
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
        my_av_strerror(ret);
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr_init failed: %s", errorBuffer);
        swr_free(&swrCtx);
        return false;
    }
    return true;
}

// --- AudioPlayer ---

AudioPlayer::AudioPlayer()
{
    if (SDL_Init(SDL_INIT_AUDIO) != 0)
    {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        // Consider throwing exception instead of exit
    }

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
    SDL_Quit();
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

    // 重要：重置当前帧，防止回调继续读取旧数据
    // 注意：如果在回调持有锁时调用此函数（不太可能，因为回调很快），会阻塞直到回调结束
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
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Invalid audio: %s", path.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = path;
        preloadPath.clear();
        hasPreloaded = false;
        m_preloadSource.reset();
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (playingState == PlayerState::SEEKING)
        {
            // 如果当前正在 Seeking，oldPlayingState 已经保存了 Seeking 之前的状态（播放或暂停）
            // 所以这里不需要做任何操作，保持原有的 oldPlayingState 即可
        }
        else if (playingState != PlayerState::STOPPED)
        {
            // 如果当前是播放或暂停，保存这个状态
            oldPlayingState = playingState;
        }
        else
        {
            // 如果当前是 STOPPED（极少情况，或者是刚启动但还没进 play），默认切歌后尝试播放
            // 但具体的行为还会被 decodeThread 中的 isFirstPlay 逻辑覆盖
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
    if (outputMode.load() != OUTPUT_MIXING)
    {
        return;
    }
    if (!isValidAudio(path))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(pathMutex);
    if (path != preloadPath)
    {
        preloadPath = path;
        hasPreloaded = false;
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
    seekTarget.store(timeMicroseconds);

    // 如果还没开始 seek，保存状态
    if (playingState != PlayerState::SEEKING)
    {
        oldPlayingState = playingState;
    }
    playingState = PlayerState::SEEKING;

    // 暂停设备防止爆音
    if (m_audioDeviceID != 0)
    {
        SDL_PauseAudioDevice(m_audioDeviceID, 1);
    }
    stateCondVar.notify_one();
}

void AudioPlayer::setVolume(double vol)
{
    volume.store(std::max(0.0, std::min(vol, 1.0)));
}

// --- Audio Device Management ---

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
    if (outputMode.load() == OUTPUT_MIXING && m_audioDeviceID != 0)
    {
        // 确保设备处于非暂停状态将在 play() 中处理，或者在这里确保也可以
        // 但根据上下文，这里只负责设备存在的确认
        return true;
    }
    closeAudioDevice();

    SDL_AudioSpec desired, obtained;
    SDL_zero(desired);

    if (outputMode.load() == OUTPUT_MIXING)
    {
        desired.freq = mixingParams.sampleRate;
        desired.format = toSDLFormat(mixingParams.sampleFormat);
        desired.channels = mixingParams.channels;
    }
    else
    {
        if (!m_currentSource || !m_currentSource->pCodecCtx)
            return false;
        desired.freq = m_currentSource->pCodecCtx->sample_rate;
        desired.format = toSDLFormat(m_currentSource->pCodecCtx->sample_fmt);
        desired.channels = m_currentSource->pCodecCtx->ch_layout.nb_channels;
    }

    desired.samples = SDL_AUDIO_BUFFER_SAMPLES;
    desired.callback = AudioPlayer::sdl2_audio_callback;
    desired.userdata = this;

    m_audioDeviceID = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (m_audioDeviceID == 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to open audio device: %s", SDL_GetError());
        return false;
    }

    deviceParams.sampleRate = obtained.freq;
    deviceParams.sampleFormat = toAVSampleFormat(obtained.format);
    deviceParams.ch_layout = toAVChannelLayout(obtained.channels);
    deviceParams.channels = obtained.channels;
    deviceSpec = obtained;

    SDL_PauseAudioDevice(m_audioDeviceID, 1);
    return true;
}

// --- Critical: SDL Callback ---
void AudioPlayer::sdl2_audio_callback(void *userdata, Uint8 *stream, int len)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(userdata);

    // 1. 初始化静音
    memset(stream, 0, len);

    // 2. 必须全程持有锁，防止与 flushQueue 冲突
    std::lock_guard<std::mutex> lock(player->audioFrameQueueMutex);

    int remaining = len;
    Uint8 *streamPos = stream;

    while (remaining > 0)
    {
        // 如果当前帧播完了或为空，尝试获取下一帧
        if (!player->m_currentFrame)
        {
            if (player->audioFrameQueue.empty())
            {
                // 队列为空，通知解码线程可能需要工作（防饥饿）
                player->stateCondVar.notify_one();
                return; // 直接返回，输出静音
            }
            player->m_currentFrame = player->audioFrameQueue.front();
            player->audioFrameQueue.pop();
            player->m_currentFramePos = 0;

            // 更新当前播放时间
            if (player->m_currentFrame)
            {
                player->nowPlayingTime.store(player->m_currentFrame->pts);
            }
            player->stateCondVar.notify_one(); // 通知解码线程有空位了
        }

        if (!player->m_currentFrame)
            break;

        size_t frameSize = player->m_currentFrame->data.size();
        int frameRemaining = static_cast<int>(frameSize - player->m_currentFramePos);
        int copySize = std::min(frameRemaining, remaining);

        if (copySize > 0)
        {
            // 使用 SDL_MixAudioFormat 以支持音量调整
            // 注意：这里我们是在持有锁的情况下调用的。
            // 由于每次拷贝通常只有几KB，memcopy/mix 很快，不会长时间阻塞主线程。
            // 相比无锁编程的极其复杂性，这是最安全的选择。
            SDL_MixAudioFormat(streamPos,
                               player->m_currentFrame->data.data() + player->m_currentFramePos,
                               player->deviceSpec.format,
                               copySize,
                               static_cast<int>(player->volume.load() * SDL_MIX_MAXVOLUME));

            remaining -= copySize;
            streamPos += copySize;
            player->m_currentFramePos += copySize;
        }

        if (player->m_currentFramePos >= frameSize)
        {
            player->m_currentFrame.reset();
        }
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
                if (m_audioDeviceID != 0)
                {
                    SDL_PauseAudioDevice(m_audioDeviceID, playingState == PlayerState::PLAYING ? 0 : 1);
                }
            }

            bool isSongLoopActive = true;
            bool playbackFinishedNaturally = false;

            while (isSongLoopActive)
            {
                // 等待合适的状态（播放中且队列不满，或 seek/quit）
                if (!waitForDecodeState())
                {
                    isSongLoopActive = false; // Stopped or Quit
                    continue;
                }

                // 处理 Seek 请求
                if (playingState == PlayerState::SEEKING)
                {
                    handleSeekRequest();
                    continue;
                }

                // 正常解码
                decodeAndProcessPacket(packet, isSongLoopActive, playbackFinishedNaturally);
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
                              // 检查队列是否已满
                              std::lock_guard<std::mutex> qLock(audioFrameQueueMutex);
                              return audioFrameQueue.size() < audioFrameQueueMaxSize.load();
                          }
                          return false; // Paused -> wait
                      });

    return !(quitFlag.load() || playingState == PlayerState::STOPPED);
}

void AudioPlayer::handleSeekRequest()
{
    // 此时 stateMutex 是锁定的（由调用者保证上下文，或者在函数内再次锁定？需注意调用位置）
    // 注意：decodeThread 主循环中是在 stateMutex 解锁后调用的此函数，所以需要重新锁。

    // 实际上我们在主循环里检测到 SEEKING 后调用此函数，不需要锁整个函数，但操作 ffmpeg 需安全
    flushQueue();

    if (!m_currentSource)
    {
        return;
    }

    int streamIdx = m_currentSource->audioStreamIndex;
    AVRational tb = m_currentSource->pFormatCtx->streams[streamIdx]->time_base;
    int64_t target = seekTarget.load();
    int64_t stream_ts = av_rescale_q(target, AV_TIME_BASE_Q, tb);

    av_seek_frame(m_currentSource->pFormatCtx, streamIdx, stream_ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_currentSource->pCodecCtx);

    {
        std::lock_guard<std::mutex> stateLock(stateMutex);
        playingState = oldPlayingState;
        if (playingState == PlayerState::PLAYING && m_audioDeviceID != 0)
        {
            SDL_PauseAudioDevice(m_audioDeviceID, 0);
        }
    }
    nowPlayingTime.store(target);
}

void AudioPlayer::decodeAndProcessPacket(AVPacket *packet, bool &isSongLoopActive, bool &playbackFinishedNaturally)
{
    int ret = av_read_frame(m_currentSource->pFormatCtx, packet);
    if (ret < 0)
    {
        if (ret == AVERROR_EOF)
        {
            if (performSeamlessSwitch())
                return;
            playbackFinishedNaturally = true;
            isSongLoopActive = false;
        }
        else
        {
            SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Read error: %s", my_av_strerror(ret).c_str());
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

    // 计算 PTS
    double ptsSec = (frame->best_effort_timestamp == AV_NOPTS_VALUE) ? -1.0 :
                                                                       frame->best_effort_timestamp * av_q2d(m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base);
    int64_t ptsMicro = (ptsSec < 0) ? nowPlayingTime.load() : static_cast<int64_t>(ptsSec * 1000000);

    triggerPreload(ptsSec);

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
    if (!openAudioDevice())
        return false;
    // 音量在 MixAudio 中控制，这里 volume 传 1.0 给 swr 避免两次缩放，或者传 volume 给 swr 并在 mix 中传 SDL_MIX_MAXVOLUME
    // 原方案：swr 设 volume，mix 也设。这会导致音量变成 volume^2。
    // 修正：Swr 负责重采样和格式转换 (volume=1.0)，SDL_MixAudio 负责最终音量。
    if (!m_currentSource->openSwrContext(deviceParams, 1.0, errorBuffer))
        return false;

    audioDuration.store(m_currentSource->pFormatCtx->duration);
    return true;
}

// ... (triggerPreload, calculateQueueSize, performSeamlessSwitch logic remains similar but cleaned up) ...

void AudioPlayer::triggerPreload(double currentPts)
{
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
        if (src->initDecoder(pPath, errorBuffer) && src->openSwrContext(deviceParams, 1.0, errorBuffer))
        {
            m_preloadSource = std::move(src);
            hasPreloaded.store(true);
            SDL_Log("Preloaded: %s", pPath.c_str());
        }
        else
        {
            std::lock_guard<std::mutex> lock(pathMutex);
            preloadPath.clear(); // Preload failed, don't try again
        }
    }
}

void AudioPlayer::calculateQueueSize(int out_bytes_per_sample)
{
    // 简单的自适应队列大小，不需要太复杂的原子操作逻辑
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

    // Reset stats for new song
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

    auto frame = audioFrameQueue.back(); // shared_ptr
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
} // s
int64_t AudioPlayer::getCurrentPositionMicroseconds() const
{
    return nowPlayingTime.load();
}
int64_t AudioPlayer::getAudioDuration() const
{
    return audioDuration.load() / AV_TIME_BASE;
} // s
int64_t AudioPlayer::getDurationMillisecond() const
{
    return audioDuration.load() / 1000;
}
int64_t AudioPlayer::getDurationMicroseconds() const
{
    return audioDuration.load();
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