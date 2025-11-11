#include "AudioPlayer.hpp"


// (移除) 此处不再需要包含 ffmpeg 和 SDL 头文件，因为 .hpp 已包含

static SDL_AudioFormat toSDLFormat(AVSampleFormat ffmpegFormat)
{
    switch (ffmpegFormat)
    {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
        return AUDIO_U8;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        return AUDIO_S16SYS;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        return AUDIO_S32SYS;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        return AUDIO_F32SYS;
    default:
        SDL_LogWarn(SDL_LOG_CATEGORY_ERROR, "不支持的FFmpeg1采样格式 %s", av_get_sample_fmt_name(ffmpegFormat));
        return 0; // 返回0表示无效格式
    }
}

static AVSampleFormat toAVSampleFormat(SDL_AudioFormat sdlFormat)
{
    switch (sdlFormat)
    {
    case AUDIO_U8:
        return AV_SAMPLE_FMT_U8;
    case AUDIO_S16SYS:
        return AV_SAMPLE_FMT_S16;
    case AUDIO_S32SYS:
        return AV_SAMPLE_FMT_S32;
    case AUDIO_F32SYS:
        return AV_SAMPLE_FMT_FLT;
    default: // 如果 SDL_AudioFormat 不在上述列表中，则返回一个无效的 AVSampleFormat
        return AV_SAMPLE_FMT_NONE;
    }
}

static AVChannelLayout toAVChannelLayout(const uint8_t &layout)
{
    switch (layout)
    {
    case 1:
        return AV_CHANNEL_LAYOUT_MONO; // 单声道
    case 2:
        return AV_CHANNEL_LAYOUT_STEREO; // 立体声
    case 3:
        return AV_CHANNEL_LAYOUT_SURROUND; // 环绕声
    case 4:
        return AV_CHANNEL_LAYOUT_4POINT0; // 四声道
    case 5:
        return AV_CHANNEL_LAYOUT_5POINT0_BACK; // 5.0声道
    case 6:
        return AV_CHANNEL_LAYOUT_5POINT1_BACK; // 5.1声道
    case 7:
        return AV_CHANNEL_LAYOUT_7POINT1_WIDE_BACK; // 7.1声道
    default:
        return AV_CHANNEL_LAYOUT_MONO; // 默认单声道
    }
}

AudioPlayer::AudioPlayer()
{
    if (SDL_Init(SDL_INIT_AUDIO))
    {
        std::cerr << "无法初始化 SDL: " << SDL_GetError() << std::endl;
        exit(1); // 如果 SDL 初始化失败，则退出程序
    }
    // 启动线程
    decodeThread = std::thread(&AudioPlayer::mainDecodeThread, this);
}

// 析构函数，安全停止线程
AudioPlayer::~AudioPlayer()
{
    quitFlag.store(true);

    // 唤醒所有可能在等待的条件变量
    path1CondVar.notify_one();
    audioFrameQueueCondVar.notify_one();

    if (decodeThread.joinable())
    {
        decodeThread.join();
    }

    // (新增) 确保在 SDL_Quit 之前释放所有资源
    freeResources();

    SDL_Quit(); // 在所有 SDL 操作完成后退出
}

// (移除) freeResources1
// (移除) freeResources2

// (修改) 统一的资源释放函数
void AudioPlayer::freeResources()
{
    if (m_audioDeviceID != 0)
    {
        SDL_PauseAudioDevice(m_audioDeviceID, 1); // 确保在关闭前暂停
        SDL_CloseAudioDevice(m_audioDeviceID);
        m_audioDeviceID = 0;
        isDeviceOpen.store(false);
    }

    // (修改) 释放两组 FFmpeg 资源
    delete m_currentSource;
    m_currentSource = nullptr;
    delete m_preloadSource;
    m_preloadSource = nullptr;

    // 清空音频帧队列
    {
        std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
        while (!audioFrameQueue.empty())
        {
            delete audioFrameQueue.front();
            audioFrameQueue.pop();
        }
        // 清理回调中可能持有的帧
        delete m_currentFrame;
        m_currentFrame = nullptr;
        m_currentFramePos = 0;
    }

    // 重置状态
    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);
    playingState.store(PlayerState::STOPPED);
    hasPaused.store(true);
    hasPreloaded.store(false);

    // (修改) 移除路径重置逻辑
    // 路径管理现在由 mainDecodeThread 的状态机负责
}

/**
 * @brief 检查给定路径的文件是否为有效的音频文件
 */
bool AudioPlayer::isValidAudio(const std::string &path)
{
    AVFormatContext *pFormatCtx = nullptr;
    SDL_Log("检查音频文件: %s\n", path.c_str());
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
    if (audioStreamIndex < 0)
    {
        avformat_close_input(&pFormatCtx);
        return false;
    }

    const AVCodec *pCodec = avcodec_find_decoder(pFormatCtx->streams[audioStreamIndex]->codecpar->codec_id);
    if (pCodec == nullptr)
    {
        avformat_close_input(&pFormatCtx);
        return false;
    }

    avformat_close_input(&pFormatCtx);
    return true;
}

// (修改) setVolume
void AudioPlayer::setVolume(double vol)
{
    volume = vol;
    // (修改) 同时设置两个重采样器
    if (m_currentSource && m_currentSource->swrCtx)
    {
        av_opt_set_double(m_currentSource->swrCtx, "out_volume", volume, 0);
    }
    if (m_preloadSource && m_preloadSource->swrCtx)
    {
        av_opt_set_double(m_preloadSource->swrCtx, "out_volume", volume, 0);
    }
}

// (修改) seek (移除 CV)
void AudioPlayer::seek(int64_t time)
{
    seekTarget.store(static_cast<int64_t>(time * AV_TIME_BASE));
    playingState.store(PlayerState::SEEKING);
    // (移除) decoderCondVar.notify_one();
    SDL_Log("跳转到 %ld 秒", time);
}

// (修改) play (移除 CV)
void AudioPlayer::play()
{
    isFirstPlay.store(false);
    playingState.store(PlayerState::PLAYING);
    // (移除) decoderCondVar.notify_one();
}

// (修改) pause (移除 CV)
void AudioPlayer::pause()
{
    playingState.store(PlayerState::PAUSED);
    // (移除) decoderCondVar.notify_one();
}

void AudioPlayer::setMixingParameters(const AudioParams &params)
{
    // TODO:
}

void AudioPlayer::setMixingParameters(int sampleRate, AVSampleFormat sampleFormat, uint64_t channelLayout, int channels)
{
    // TODO:
}

AudioParams AudioPlayer::getMixingParameters() const
{
    return mixingParams;
}

// (移除) initDecoder
// (移除) initDecoder2

// (新增) AudioStreamSource::initDecoder 实现
bool AudioPlayer::AudioStreamSource::initDecoder(const std::string &inputPath, char *errorBuffer)
{
    if (inputPath.empty())
        return false;

    path = inputPath; // 存储路径
    int ret = 0;

    // 1. 打开文件
    ret = avformat_open_input(&pFormatCtx, path.c_str(), nullptr, nullptr);
    if (ret != 0)
    {
        av_strerror(ret, errorBuffer, AV_ERROR_MAX_STRING_SIZE * 2);
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "无法打开音频文件: %s, Error:\n%s", path.c_str(), errorBuffer);
        return false;
    }
    // 2. 查找流信息
    ret = avformat_find_stream_info(pFormatCtx, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, AV_ERROR_MAX_STRING_SIZE * 2);
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "无法找到流信息: %s, Error:\n%s", path.c_str(), errorBuffer);
        avformat_close_input(&pFormatCtx); // (修复) 确保失败时关闭
        return false;
    }
    // 3. 查找音频流
    audioStreamIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "无法找到音频流: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }
    // 4. 获取解码器参数
    pCodecParameters = pFormatCtx->streams[audioStreamIndex]->codecpar;
    // 5. 查找解码器
    pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if (pCodec == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法找到解码器: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }
    // 6. 分配解码器上下文
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (pCodecCtx == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法分配解码器上下文: %s", path.c_str());
        avformat_close_input(&pFormatCtx);
        return false;
    }
    // 7. 复制参数到上下文
    ret = avcodec_parameters_to_context(pCodecCtx, pCodecParameters);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, AV_ERROR_MAX_STRING_SIZE * 2);
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法将解码器参数复制到上下文: %s, Error:\n%s", path.c_str(), errorBuffer);
        avcodec_free_context(&pCodecCtx);
        avformat_close_input(&pFormatCtx);
        return false;
    }
    // 8. 打开解码器
    ret = avcodec_open2(pCodecCtx, pCodec, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, AV_ERROR_MAX_STRING_SIZE * 2);
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法打开解码器: %s, Error:\n%s", path.c_str(), errorBuffer);
        avcodec_free_context(&pCodecCtx);
        avformat_close_input(&pFormatCtx);
        return false;
    }

    SDL_Log("Decoder initialized successfully for: %s", path.c_str());
    return true;
}

// (修改) openAudioDevice, 仅打开设备，不创建 SwrContext
bool AudioPlayer::openAudioDevice()
{
    SDL_AudioSpec desiredSpec, obtainedSpec;
    if (outputMode.load() == OUTPUT_MIXING)
    {
        desiredSpec.freq = mixingParams.sampleRate;
        desiredSpec.format = toSDLFormat(mixingParams.sampleFormat);
        desiredSpec.channels = mixingParams.ch_layout.nb_channels;
    }
    else
    {
        // DIRECT 模式依赖 m_currentSource
        if (!m_currentSource || !m_currentSource->pCodecCtx)
        {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Cannot open device in DIRECT mode: no codec context.");
            return false;
        }
        desiredSpec.freq = m_currentSource->pCodecCtx->sample_rate;
        desiredSpec.format = toSDLFormat(m_currentSource->pCodecCtx->sample_fmt);
        desiredSpec.channels = m_currentSource->pCodecCtx->ch_layout.nb_channels;
    }

    desiredSpec.samples = 4096;
    desiredSpec.callback = AudioPlayer::sdl2_audio_callback;
    desiredSpec.userdata = this;

    // (修改) m_audioDeviceID 应该是唯一的，如果已打开，先关闭
    if (m_audioDeviceID != 0)
    {
        SDL_CloseAudioDevice(m_audioDeviceID);
    }

    m_audioDeviceID = SDL_OpenAudioDevice(nullptr, 0, &desiredSpec, &obtainedSpec, 0);
    if (m_audioDeviceID == 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "audio device open failed! Error:\n%s", SDL_GetError());
        return false; // 让调用者 (mainDecodeThread) 处理 freeResources
    }

    // (新增) 保存实际的设备参数
    deviceParams.sampleRate = obtainedSpec.freq;
    deviceParams.sampleFormat = toAVSampleFormat(obtainedSpec.format);
    deviceParams.ch_layout = toAVChannelLayout(obtainedSpec.channels);
    deviceParams.channels = obtainedSpec.channels;
    isDeviceOpen.store(true);
    deviceSpec = obtainedSpec;

    // (移除) SwrContext 创建逻辑

    SDL_PauseAudioDevice(m_audioDeviceID, 1); // 默认暂停音频设备

    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);
    return true;
}

// (移除) openSwrContext2

// (新增) AudioStreamSource::openSwrContext 实现
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
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr alloc failed!");
        return false;
    }

    // 输入参数 (来自此 source)
    av_opt_set_chlayout(swrCtx, "in_chlayout", &(pCodecCtx->ch_layout), 0);
    av_opt_set_int(swrCtx, "in_sample_rate", pCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", pCodecCtx->sample_fmt, 0);

    // 输出参数 (来自 device)
    av_opt_set_chlayout(swrCtx, "out_chlayout", &(deviceParams.ch_layout), 0);
    av_opt_set_int(swrCtx, "out_sample_rate", deviceParams.sampleRate, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", deviceParams.sampleFormat, 0);

    // 音量
    av_opt_set_double(swrCtx, "out_volume", volume, 0);

    int ret = swr_init(swrCtx);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, AV_ERROR_MAX_STRING_SIZE * 2);
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr init failed! Error:%s\n", errorBuffer);
        swr_free(&swrCtx); // swr_free 会设置 swrCtx 为 NULL
        return false;
    }
    SDL_Log("SwrContext initialized successfully for: %s", path.c_str());
    return true;
}

// (修改) sdl2_audio_callback (移除 CV)
void AudioPlayer::sdl2_audio_callback(void *userdata, Uint8 *stream, int len)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(userdata);
    memset(stream, 0, len);
    int remaining = len;
    Uint8 *streamPos = stream;
    // (移除) bool needsNotify = false;

    while (remaining > 0)
    {
        if (player->m_currentFrame == nullptr)
        {
            std::unique_lock<std::mutex> lock(player->audioFrameQueueMutex);
            if (player->audioFrameQueue.empty())
            {
                // 音频帧队列为空，填充静音数据
                lock.unlock();
                memset(streamPos, 0, remaining);
                return;
            }
            else
            {
                // (移除) 检查队列是否之前是满的
                player->m_currentFrame = player->audioFrameQueue.front();
                player->audioFrameQueue.pop();
                player->m_currentFramePos = 0;
                player->nowPlayingTime.store(static_cast<int64_t>(player->m_currentFrame->pts));
            }
            lock.unlock();

            // (移除) 在锁外通知
            // if (needsNotify) ...
        }

        int frameRemaining = player->m_currentFrame->size - player->m_currentFramePos;
        int copySize = std::min(frameRemaining, remaining);
        int sdl_volume = static_cast<int>(player->volume * SDL_MIX_MAXVOLUME);
        SDL_AudioFormat deviceFormat = player->deviceSpec.format;

        SDL_MixAudioFormat(
            streamPos,                                                // 目标: SDL流缓冲区
            player->m_currentFrame->data + player->m_currentFramePos, // 源: 你的PCM数据
            deviceFormat,                                             // 你的SDL音频格式
            copySize,                                                 // 拷贝长度
            sdl_volume                                                // 音量 (0-128)
        );

        remaining -= copySize;
        streamPos += copySize;
        player->m_currentFramePos += copySize;

        if (player->m_currentFramePos == player->m_currentFrame->size)
        {
            // 当前帧播放完毕，释放内存
            delete player->m_currentFrame;
            player->m_currentFrame = nullptr; // 重置当前帧指针
            player->m_currentFramePos = 0;
        }
    }
}

// (修改) setPath1, 使用 AudioStreamSource
bool AudioPlayer::setPath1(const std::string &path)
{
    if (!isValidAudio(path))
    {
        return false;
    }

    // (修改) 手动设置路径时，清除预加载路径
    {
        std::lock_guard<std::mutex> lock(preloadPathMutex);
        if (!preloadPath.empty())
        {
            SDL_Log("手动切歌, 清除预加载: %s", preloadPath.c_str());
            delete m_preloadSource; // (修改) 释放预加载资源
            m_preloadSource = nullptr;
            preloadPath = "";
            hasPreloaded.store(false);
        }
    }

    {
        std::lock_guard<std::mutex> lock(path1Mutex);
        currentPath = path;
    }

    // (修改) 停止当前播放，以强制重新加载
    playingState.store(PlayerState::STOPPED);
    // (移除) decoderCondVar.notify_one();
    path1CondVar.notify_one(); // 唤醒解码线程(如果它在wait path)

    SDL_Log("设置播放路径: %s\n", path.c_str());
    return true;
}

// (修改) setPreloadPath
void AudioPlayer::setPreloadPath(const std::string &path)
{
    if (outputMode.load() != OUTPUT_MIXING)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "非 MIXING 模式, 无法预加载");
        return;
    }
    if (!isValidAudio(path))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "预加载路径无效: %s", path.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(preloadPathMutex);
        if (path == preloadPath)
            return; // 路径未改变

        // (修改) 释放旧的预加载资源
        delete m_preloadSource;
        m_preloadSource = nullptr;

        preloadPath = path;
        hasPreloaded.store(false); // 标记为尚未加载
    }

    SDL_Log("设置预加载路径: %s", preloadPath.c_str());
}

int64_t AudioPlayer::getNowPlayingTime() const
{
    return nowPlayingTime.load();
}

int64_t AudioPlayer::getAudioLength() const
{
    // (修改) 确保 m_currentSource 存在
    if (m_currentSource && m_currentSource->pFormatCtx)
    {
        return m_currentSource->pFormatCtx->duration / AV_TIME_BASE;
    }
    return audioDuration.load() / AV_TIME_BASE; // 降级
}

// (修改) 完整的 mainDecodeThread 重构
void AudioPlayer::mainDecodeThread()
{
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();

    if (!frame || !packet)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to allocate frame or packet");
        quitFlag.store(true);
    }

    while (quitFlag.load() == false)
    {
        // 1. 等待新路径
        {
            std::unique_lock<std::mutex> lock(path1Mutex);
            path1CondVar.wait(lock, [this]
                              { return (!currentPath.empty()) || quitFlag.load(); });
        }
        if (quitFlag.load())
        {
            break;
        }

        // 2. 初始化 (使用 AudioStreamSource)
        m_currentSource = new AudioStreamSource();
        std::string path;
        {
            std::lock_guard<std::mutex> lock(path1Mutex);
            path = currentPath;
        }

        if (!m_currentSource->initDecoder(path, errorBuffer))
        {
            freeResources(); // 自动清理 m_currentSource
            continue;
        }
        if (!openAudioDevice()) // 依赖 m_currentSource
        {
            freeResources();
            continue;
        }
        if (!m_currentSource->openSwrContext(deviceParams, volume, errorBuffer))
        {
            freeResources();
            continue;
        }

        audioDuration.store(m_currentSource->pFormatCtx->duration); // 获取音频总时长
        SDL_Log("音乐总时长：%ld", audioDuration.load());

        bool isFileEOF = false;
        // 3. (修改) 初始化成功，根据 isFirstPlay 决定状态
        if (isFirstPlay.load())
        {
            playingState.store(PlayerState::PAUSED);
            hasPaused.store(true);
        }
        else
        {
            isFirstPlay.store(false);
            playingState.store(PlayerState::PLAYING);
            SDL_PauseAudioDevice(m_audioDeviceID, 0); // 立即开始播放
            hasPaused.store(false);
        }

        // 4. 主解码循环
        bool playbackFinishedNaturally = false; // (新增) 用于跟踪播放是如何结束的
        while (quitFlag.load() == false)
        {
            PlayerState currentState = playingState.load();

            // 4.1 状态处理：STOPPED (用于手动切歌)
            if (currentState == PlayerState::STOPPED)
            {
                SDL_Log("Playback stopped.");
                break; // 退出内层循环, 将在外层 freeResources()
            }

            // 4.2 状态处理：SEEKING
            if (currentState == PlayerState::SEEKING)
            {
                SDL_PauseAudioDevice(m_audioDeviceID, 1); // 跳转时暂停
                int64_t target = seekTarget.load();

                // 清空音频队列
                {
                    std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
                    while (!audioFrameQueue.empty())
                    {
                        delete audioFrameQueue.front();
                        audioFrameQueue.pop();
                    }
                    delete m_currentFrame;
                    m_currentFrame = nullptr;
                    m_currentFramePos = 0;
                }

                AVRational tb = m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base;
                int64_t stream_ts = av_rescale_q(target, AV_TIME_BASE_Q, tb);
                int ret = av_seek_frame(m_currentSource->pFormatCtx, m_currentSource->audioStreamIndex, stream_ts, AVSEEK_FLAG_BACKWARD);
                if (ret < 0)
                {
                    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Error seeking audio stream:%s", av_err2str(ret));
                }

                avcodec_flush_buffers(m_currentSource->pCodecCtx);
                if (m_currentSource->swrCtx)
                {
                    swr_init(m_currentSource->swrCtx); // 重置重采样器状态
                }

                isFileEOF = false;                        // 跳转后文件不再是 EOF
                playingState.store(PlayerState::PLAYING); // 跳转完成，恢复播放
                SDL_PauseAudioDevice(m_audioDeviceID, 0); // 恢复播放
                hasPaused.store(false);
                continue;
            }

            // 4.3 状态处理：PAUSED (Task 2)
            if (currentState == PlayerState::PAUSED)
            {
                if (hasPaused.load() == false)
                {
                    SDL_PauseAudioDevice(m_audioDeviceID, 1);
                    hasPaused.store(true);
                }
                // (修改) 使用 sleep 替代 CV
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 4.4 状态处理：PLAYING (从 PAUSED 恢复)
            if (hasPaused.load() == true)
            {
                SDL_PauseAudioDevice(m_audioDeviceID, 0); // 恢复音频设备
                hasPaused.store(false);
            }

            // 4.5 状态处理: 队列已满 (Task 2)
            {
                std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
                if (audioFrameQueue.size() >= audioFrameQueueMaxSize.load() && !isFileEOF)
                {
                    lock.unlock();
                    // (修改) 使用 sleep 替代 CV
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue; // 队列已满, 稍后重试
                }
            } // lock released

            // 4.6 状态处理: EOF
            if (isFileEOF)
            {
                std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
                if (audioFrameQueue.empty() && m_currentFrame == nullptr)
                {
                    SDL_Log("音频播放完毕: %s", m_currentSource->path.c_str());

                    // (修改) 检查是否可以无缝切换
                    bool didSwitch = false;
                    if (outputMode.load() == OUTPUT_MIXING && hasPreloaded.load())
                    {
                        std::lock_guard<std::mutex> preloadLock(preloadPathMutex);
                        // (修改) 检查 m_preloadSource 及其 swrCtx
                        if (m_preloadSource != nullptr && m_preloadSource->swrCtx != nullptr)
                        {
                            SDL_Log("无缝切换到: %s", m_preloadSource->path.c_str());

                            // 释放资源 1
                            delete m_currentSource;

                            // 将资源 2 切换到资源 1
                            m_currentSource = m_preloadSource;
                            m_preloadSource = nullptr; // 转移所有权

                            {
                                std::lock_guard<std::mutex> lock(path1Mutex);
                                currentPath = m_currentSource->path;
                            }
                            audioDuration.store(m_currentSource->pFormatCtx->duration);

                            // 清空资源 2 状态
                            preloadPath = "";
                            hasPreloaded.store(false);

                            // 重置状态
                            isFileEOF = false;
                            totalDecodedBytes.store(0);
                            totalDecodedFrames.store(0);
                            hasCalculatedQueueSize.store(false);
                            didSwitch = true;
                        }
                    }

                    if (!didSwitch)
                    {
                        playbackFinishedNaturally = true; // (修改) 标记为自然结束
                        break;                            // 无法切换或非 mixing 模式，退出内层循环
                    }
                    // 如果切换成功，循环继续
                    continue;
                }
                else
                {
                    // 队列未空，休眠一下等待队列消耗
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            } // end if(isFileEOF)

            // --- 5. 执行解码 ---
            av_packet_unref(packet);
            int ret = av_read_frame(m_currentSource->pFormatCtx, packet);
            if (ret < 0)
            {
                if (ret == AVERROR_EOF)
                {
                    isFileEOF = true;
                    SDL_Log("AVERROR_EOF reached for: %s", m_currentSource->path.c_str());
                }
                else
                {
                    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error reading frame: %s\n", av_err2str(ret));
                    break;
                }
                continue;
            }

            if (packet->stream_index != m_currentSource->audioStreamIndex)
            {
                continue; // 不是我们的音频流
            }

            ret = avcodec_send_packet(m_currentSource->pCodecCtx, packet);
            if (ret < 0)
            {
                SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error sending packet for decoding: %s\n", av_err2str(ret));
                break;
            }

            while (ret >= 0)
            {
                ret = avcodec_receive_frame(m_currentSource->pCodecCtx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    break;
                }
                else if (ret < 0)
                {
                    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error during decoding: %s\n", av_err2str(ret));
                    break;
                }

                // --- 6. 解码成功，开始重采样 ---

                int64_t best_pts = frame->best_effort_timestamp;
                double pts = (best_pts == AV_NOPTS_VALUE) ? -1.0 :
                                                            best_pts * av_q2d(m_currentSource->pFormatCtx->streams[m_currentSource->audioStreamIndex]->time_base);

                // (新增) 检查是否需要触发预加载
                mainLoop_TriggerPreload(pts);

                // --- 重采样逻辑 ---
                int out_sample_rate = 0;
                int out_nb_channels = 0;
                AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_NONE;

                av_opt_get_int(m_currentSource->swrCtx, "out_sample_rate", 0, (int64_t *)&out_sample_rate);
                av_opt_get_int(m_currentSource->swrCtx, "out_sample_fmt", 0, (int64_t *)&out_sample_fmt);
                AVChannelLayout out_ch_layout;
                av_opt_get_chlayout(m_currentSource->swrCtx, "out_chlayout", 0, &out_ch_layout);
                out_nb_channels = out_ch_layout.nb_channels;
                av_channel_layout_uninit(&out_ch_layout); // (注意) 这里的 uninit 是正确的

                int out_samples = av_rescale_rnd(
                    swr_get_delay(m_currentSource->swrCtx, frame->sample_rate) + frame->nb_samples,
                    out_sample_rate,
                    frame->sample_rate, AV_ROUND_UP);

                int out_bytes_per_sample = av_get_bytes_per_sample(out_sample_fmt);
                int out_buffer_size = out_samples * out_nb_channels * out_bytes_per_sample;

                uint8_t *out_buffer = (uint8_t *)av_malloc(out_buffer_size);
                if (!out_buffer)
                {
                    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Failed to allocate output buffer\n");
                    ret = AVERROR(ENOMEM);
                    break;
                }

                int converted_samples = swr_convert(m_currentSource->swrCtx, &out_buffer, out_samples,
                                                    (const uint8_t **)frame->data, frame->nb_samples);
                if (converted_samples < 0)
                {
                    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error while converting\n");
                    av_free(out_buffer);
                    ret = AVERROR_UNKNOWN;
                    break;
                }

                AudioFrame *audio_frame = new AudioFrame();
                audio_frame->size = converted_samples * out_nb_channels * out_bytes_per_sample;
                audio_frame->data = out_buffer;
                audio_frame->pts = pts;
                {
                    std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
                    audioFrameQueue.push(audio_frame);
                    audioFrameQueueCondVar.notify_one();
                }

                totalDecodedBytes.fetch_add(audio_frame->size);
                totalDecodedFrames.fetch_add(1);

                // (新增) 自动计算队列大小
                mainLoop_CalculateQueueSize(frame, out_bytes_per_sample);

                av_frame_unref(frame);
            }

            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            {
                SDL_LogError(SDL_LOG_CATEGORY_ERROR, "A critical error occurred during decode/resample, stopping playback.");
                break; // 发生严重错误，退出内层循环
            }
        } // --- End Inner Loop ---

        // 5. 内层循环退出（播放完毕、出错或退出），清理资源
        SDL_Log("Cleaning up resources for %s", m_currentSource ? m_currentSource->path.c_str() : "unknown");
        freeResources();

        // (修改) 修复：仅在自然播放完毕时才清除路径
        // 如果是 setPath1 触发的 STOP, playbackFinishedNaturally 为 false, 路径被保留
        if (playbackFinishedNaturally)
        {
            std::lock_guard<std::mutex> lock(path1Mutex);
            currentPath = "";
            // 同样清除预加载路径字符串，保持一致性
            std::lock_guard<std::mutex> lock2(preloadPathMutex);
            preloadPath = "";
        }

    } // --- End Outer Loop ---

    // 释放循环外分配的内存
    av_frame_free(&frame);
    av_packet_free(&packet);
}

// (新增) 触发预加载的辅助函数
void AudioPlayer::mainLoop_TriggerPreload(double currentPts)
{
    if (outputMode.load() != OUTPUT_MIXING || hasPreloaded.load() || currentPts < 0)
    {
        return; // 模式不符、已预加载 或 pts 无效
    }

    std::string path_to_preload;
    {
        std::lock_guard<std::mutex> lock(preloadPathMutex);
        path_to_preload = preloadPath;
    }

    if (path_to_preload.empty())
    {
        return; // 没有设置预加载路径
    }

    double durationSec = audioDuration.load() / (double)AV_TIME_BASE;
    // 提前 10 秒开始预加载
    if ((durationSec - currentPts) < 10.0)
    {
        SDL_Log("Near end of track, starting preload for: %s", path_to_preload.c_str());

        // (修改) 创建并初始化预加载源
        // (注意) m_preloadSource 此时应为 nullptr，或在 setPreloadPath 中被清理
        if (m_preloadSource)
        {
            delete m_preloadSource;
            m_preloadSource = nullptr;
        }

        m_preloadSource = new AudioStreamSource();
        // (修改) 预加载也需要打开 SwrContext
        if (m_preloadSource->initDecoder(path_to_preload, errorBuffer) && m_preloadSource->openSwrContext(deviceParams, volume, errorBuffer))
        {
            hasPreloaded.store(true);
            SDL_Log("Preload successful for: %s", path_to_preload.c_str());
        }
        else
        {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to preload track: %s", path_to_preload.c_str());
            delete m_preloadSource; // 清理失败的预加载
            m_preloadSource = nullptr;

            // (新增) 预加载失败，清除路径，避免重复尝试
            std::lock_guard<std::mutex> lock(preloadPathMutex);
            preloadPath = "";
        }
    }
}

// (新增) 自动计算队列大小的辅助函数
void AudioPlayer::mainLoop_CalculateQueueSize(AVFrame *frame, int out_bytes_per_sample)
{
    if (hasCalculatedQueueSize.load() || totalDecodedFrames.load() < 5)
    {
        return; // 已计算或样本不足
    }

    // (修改) 使用实际的设备参数
    int out_sample_rate = deviceParams.sampleRate;
    int out_nb_channels = deviceParams.channels;

    int64_t totalBytes = totalDecodedBytes.load();
    int64_t totalFrames = totalDecodedFrames.load();
    int avgFrameBytes = std::max(1, static_cast<int>(totalBytes / totalFrames));
    double bufferDuration = 0.2; // 200ms 缓冲
    int bytesPerSecond = out_sample_rate * out_nb_channels * out_bytes_per_sample;
    int targetBufferBytes = static_cast<int>(bytesPerSecond * bufferDuration);
    int maxFrames = std::max(2, targetBufferBytes / avgFrameBytes);

    audioFrameQueueMaxSize.store(maxFrames);
    hasCalculatedQueueSize.store(true);

    SDL_Log("Auto-calculated audio buffer: %.1f ms buffer, %d frames (avgFrame=%d bytes, sampleRate=%d, ch=%d, bytes/sample=%d)",
            bufferDuration * 1000.0, maxFrames, avgFrameBytes, out_sample_rate, out_nb_channels, out_bytes_per_sample);
}