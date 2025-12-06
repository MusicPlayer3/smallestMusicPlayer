#include "AudioPlayer.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

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
    SDL_Quit();
}

void AudioPlayer::freeResources()
{
    closeAudioDevice();
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

std::vector<int> AudioPlayer::buildAudioWaveform(const std::string &filepath, int barCount, int totalWidth, int &barWidth, int maxHeight)
{
    std::vector<int> barHeights(barCount, 0);
    if (barCount <= 0 || totalWidth <= 0)
        return barHeights;
    barWidth = totalWidth / barCount;

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

    AVCodecParameters *param = fmt->streams[streamIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(param->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
    {
        avformat_close_input(&fmt);
        return barHeights;
    }

    avcodec_parameters_to_context(ctx, param);
    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return barHeights;
    }

    SwrContext *swr = swr_alloc();
    AVChannelLayout in_layout = ctx->ch_layout;
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, 1); // Mix to Mono for waveform

    av_opt_set_chlayout(swr, "in_chlayout", &in_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &out_layout, 0);
    av_opt_set_int(swr, "in_sample_rate", ctx->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", 44100, 0); // Downsample for speed? Keep original often better
    av_opt_set_sample_fmt(swr, "in_sample_fmt", ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    swr_init(swr);

    int64_t duration = fmt->duration;
    double durationSec = (duration > 0) ? (double)duration / AV_TIME_BASE : 1.0;

    // 粗略计算每个 bar 代表的采样数 (44100Hz 下)
    double samplesPerBar = (durationSec * 44100) / barCount;
    if (samplesPerBar < 1)
        samplesPerBar = 1;

    std::vector<float> peaks(barCount, 0.0f);
    int64_t currentSample = 0;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    // Reuse buffer
    uint8_t *outData = nullptr;
    int outLineSize = 0;
    const int MAX_SAMPLES = 8192;
    av_samples_alloc(&outData, &outLineSize, 1, MAX_SAMPLES, AV_SAMPLE_FMT_FLT, 0);

    while (av_read_frame(fmt, pkt) >= 0)
    {
        if (pkt->stream_index == streamIdx)
        {
            if (avcodec_send_packet(ctx, pkt) >= 0)
            {
                while (avcodec_receive_frame(ctx, frame) >= 0)
                {
                    int out_samples = swr_convert(swr, &outData, MAX_SAMPLES, (const uint8_t **)frame->data, frame->nb_samples);
                    if (out_samples > 0)
                    {
                        float *samples = (float *)outData;
                        for (int i = 0; i < out_samples; ++i)
                        {
                            float val = std::abs(samples[i]);
                            int idx = static_cast<int>((currentSample + i) / samplesPerBar);
                            if (idx < barCount && val > peaks[idx])
                            {
                                peaks[idx] = val;
                            }
                        }
                        currentSample += out_samples;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (outData)
        av_freep(&outData);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    for (int i = 0; i < barCount; ++i)
    {
        barHeights[i] = static_cast<int>(std::min(peaks[i], 1.0f) * maxHeight);
    }
    return barHeights;
}