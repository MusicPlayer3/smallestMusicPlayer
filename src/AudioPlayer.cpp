#include "AudioPlayer.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_log.h>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>

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
    // 启动线程，不再 detach
    decodeThread = std::thread(&AudioPlayer::mainDecodeThread, this);
}

// 析构函数，安全停止线程
AudioPlayer::~AudioPlayer()
{
    quitFlag.store(true);

    // 唤醒所有可能在等待的条件变量
    path1CondVar.notify_one();
    decoderCondVar.notify_one();
    audioFrameQueueCondVar.notify_one();

    if (decodeThread.joinable())
    {
        decodeThread.join();
    }
    SDL_Quit(); // 在所有 SDL 操作完成后退出
}

// (新增) 释放资源 1
void AudioPlayer::freeResources1()
{
    if (swrCtx)
    {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }
    if (pCodecCtx1)
    {
        avcodec_free_context(&pCodecCtx1);
        pCodecCtx1 = nullptr;
    }
    if (pFormatCtx1)
    {
        avformat_close_input(&pFormatCtx1);
        pFormatCtx1 = nullptr;
    }
    pCodecParameters1 = nullptr;
    pCodec1 = nullptr;
    audioStreamIndex1 = -1;
}

// (新增) 释放资源 2
void AudioPlayer::freeResources2()
{
    std::lock_guard<std::mutex> lock(preloadPathMutex);
    if (swrCtx2)
    {
        swr_free(&swrCtx2);
        swrCtx2 = nullptr;
    }
    if (pCodecCtx2)
    {
        avcodec_free_context(&pCodecCtx2);
        pCodecCtx2 = nullptr;
    }
    if (pFormatCtx2)
    {
        avformat_close_input(&pFormatCtx2);
        pFormatCtx2 = nullptr;
    }
    pCodecParameters2 = nullptr;
    pCodec2 = nullptr;
    audioStreamIndex2 = -1;
    preloadPath = "";
    hasPreloaded.store(false);
}

// (修改) 统一的资源释放函数
void AudioPlayer::freeResources()
{
    if (m_audioDeviceID != 0)
    {
        SDL_PauseAudioDevice(m_audioDeviceID, 1); // 确保在关闭前暂停
        SDL_CloseAudioDevice(m_audioDeviceID);
        m_audioDeviceID = 0;
    }

    // 释放两组 FFmpeg 资源
    freeResources1();
    freeResources2();

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
    if (playingState.load() != PlayerState::STOPPED)
    {
        currentPath = ""; // 重置当前路径
    }
    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);
    playingState.store(PlayerState::STOPPED);
    hasPaused.store(true); // 重置为暂停状态
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

void AudioPlayer::setVolume(double vol)
{
    volume = vol;
    // (修改) 同时设置两个重采样器
    if (swrCtx)
    {
        av_opt_set_double(swrCtx, "out_volume", volume, 0);
    }
    if (swrCtx2)
    {
        av_opt_set_double(swrCtx2, "out_volume", volume, 0);
    }
}

// 设置播放进度，单位为秒
void AudioPlayer::seek(int64_t time)
{
    seekTarget.store(static_cast<int64_t>(time * AV_TIME_BASE));
    playingState.store(PlayerState::SEEKING);
    decoderCondVar.notify_one();
    SDL_Log("跳转到 %ld 秒", time);
}

// (修改) 首次播放逻辑
void AudioPlayer::play()
{
    isFirstPlay.store(false); // (新增) 标记为非首次播放
    playingState.store(PlayerState::PLAYING);
    decoderCondVar.notify_one();
}

void AudioPlayer::pause()
{
    playingState.store(PlayerState::PAUSED);
    decoderCondVar.notify_one();
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

// (修改) initDecoder, 使用资源 1
bool AudioPlayer::initDecoder()
{
    int ret = 0;
    std::unique_lock<std::mutex> lock(path1Mutex);
    // 打开文件
    ret = avformat_open_input(&pFormatCtx1, currentPath.c_str(), nullptr, nullptr);
    if (ret != 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "无法打开音频文件: %s, Error:\n%s", currentPath.c_str(), errorBuffer);
        freeResources(); // 统一清理
        return false;
    }
    // 查找流信息
    ret = avformat_find_stream_info(pFormatCtx1, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "无法找到流信息: %s, Error:\n%s", currentPath.c_str(), errorBuffer);
        freeResources(); // 统一清理
        return false;
    }

    // 查找音频流
    int audioStreamIndex = av_find_best_stream(pFormatCtx1,
                                               AVMEDIA_TYPE_AUDIO,
                                               -1,
                                               -1,
                                               nullptr,
                                               0);
    if (audioStreamIndex < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "无法找到音频流: %s", currentPath.c_str());
        freeResources(); // 统一清理
        return false;
    }
    audioStreamIndex1 = audioStreamIndex;

    // 获取解码器参数
    pCodecParameters1 = pFormatCtx1->streams[audioStreamIndex]->codecpar;
    // 查找解码器
    pCodec1 = avcodec_find_decoder(pCodecParameters1->codec_id);
    if (pCodec1 == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法找到解码器: %s", currentPath.c_str());
        freeResources(); // 统一清理
        return false;
    }
    // 创建解码器上下文
    pCodecCtx1 = avcodec_alloc_context3(pCodec1);
    if (pCodecCtx1 == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法分配解码器上下文: %s", currentPath.c_str());
        freeResources(); // 统一清理
        return false;
    }
    // 将解码器参数复制到上下文
    ret = avcodec_parameters_to_context(pCodecCtx1, pCodecParameters1);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法将解码器参数复制到上下文: %s, Error:\n%s", currentPath.c_str(), errorBuffer);
        freeResources(); // 统一清理
        return false;
    }
    // 打开解码器
    ret = avcodec_open2(pCodecCtx1, pCodec1, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法打开解码器: %s, Error:\n%s", currentPath.c_str(), errorBuffer);
        freeResources(); // 统一清理
        return false;
    }

    audioDuration.store(pFormatCtx1->duration); // 获取音频总时长

    SDL_Log("音乐总时长：%ld", audioDuration.load());

    lock.unlock();
    return true;
}

// (新增) initDecoder2, 使用资源 2
bool AudioPlayer::initDecoder2()
{
    std::lock_guard<std::mutex> lock(preloadPathMutex);
    if (preloadPath.empty())
        return false;

    int ret = 0;
    // 打开文件
    ret = avformat_open_input(&pFormatCtx2, preloadPath.c_str(), nullptr, nullptr);
    if (ret != 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "预加载: 无法打开音频文件: %s, Error:\n%s", preloadPath.c_str(), errorBuffer);
        return false;
    }
    // 查找流信息
    ret = avformat_find_stream_info(pFormatCtx2, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "预加载: 无法找到流信息: %s, Error:\n%s", preloadPath.c_str(), errorBuffer);
        avformat_close_input(&pFormatCtx2);
        return false;
    }

    // 查找音频流
    int audioStreamIndex = av_find_best_stream(pFormatCtx2, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStreamIndex < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "预加载: 无法找到音频流: %s", preloadPath.c_str());
        avformat_close_input(&pFormatCtx2);
        return false;
    }
    audioStreamIndex2 = audioStreamIndex;

    // 获取解码器参数
    pCodecParameters2 = pFormatCtx2->streams[audioStreamIndex]->codecpar;
    // 查找解码器
    pCodec2 = avcodec_find_decoder(pCodecParameters2->codec_id);
    if (pCodec2 == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "预加载: 无法找到解码器: %s", preloadPath.c_str());
        avformat_close_input(&pFormatCtx2);
        return false;
    }
    // 创建解码器上下文
    pCodecCtx2 = avcodec_alloc_context3(pCodec2);
    if (pCodecCtx2 == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "预加载: 无法分配解码器上下文: %s", preloadPath.c_str());
        avformat_close_input(&pFormatCtx2);
        return false;
    }
    // 将解码器参数复制到上下文
    ret = avcodec_parameters_to_context(pCodecCtx2, pCodecParameters2);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "预加载: 无法将解码器参数复制到上下文: %s, Error:\n%s", preloadPath.c_str(), errorBuffer);
        avcodec_free_context(&pCodecCtx2);
        avformat_close_input(&pFormatCtx2);
        return false;
    }
    // 打开解码器
    ret = avcodec_open2(pCodecCtx2, pCodec2, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "预加载: 无法打开解码器: %s, Error:\n%s", preloadPath.c_str(), errorBuffer);
        avcodec_free_context(&pCodecCtx2);
        avformat_close_input(&pFormatCtx2);
        return false;
    }
    SDL_Log("预加载成功: %s", preloadPath.c_str());
    return true;
}

// openAudioDevice, 使用资源 1
bool AudioPlayer::openAudioDevice()
{
    SDL_AudioSpec desiredSpec, obtainedSpec;
    if (outputMode.load() == OUTPUT_MIXING)
    {
        desiredSpec.freq = mixingParams.sampleRate;
        desiredSpec.format = toSDLFormat(mixingParams.sampleFormat);
        desiredSpec.channels = mixingParams.ch_layout.nb_channels;
        desiredSpec.samples = 4096;
        desiredSpec.callback = AudioPlayer::sdl2_audio_callback;
        desiredSpec.userdata = this;
    }
    else
    {
        // OUTPUT_DIRECT 模式
        desiredSpec.freq = pCodecCtx1->sample_rate;
        desiredSpec.format = toSDLFormat(pCodecCtx1->sample_fmt);
        desiredSpec.channels = pCodecCtx1->ch_layout.nb_channels;
        desiredSpec.samples = 4096;
        desiredSpec.callback = AudioPlayer::sdl2_audio_callback;
        desiredSpec.userdata = this;
    }

    // (修改) m_audioDeviceID 应该是唯一的，如果已打开，先关闭
    if (m_audioDeviceID != 0)
    {
        SDL_CloseAudioDevice(m_audioDeviceID);
    }

    m_audioDeviceID = SDL_OpenAudioDevice(nullptr, 0, &desiredSpec, &obtainedSpec, 0);
    if (m_audioDeviceID == 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "audio device open failed! Error:\n%s", SDL_GetError());
        freeResources(); // 统一清理
        return false;
    }

    swrCtx = swr_alloc();
    if (!swrCtx)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr alloc failed! Error:\n");
        freeResources(); // 统一清理
        return false;
    }
    // 输入参数 (来自 pCodecCtx1)
    av_opt_set_chlayout(swrCtx, "in_chlayout", &(pCodecCtx1->ch_layout), 0);
    av_opt_set_int(swrCtx, "in_sample_rate", pCodecCtx1->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", pCodecCtx1->sample_fmt, 0);

    // 输出参数 (来自 obtainedSpec)
    AVChannelLayout obtainedChLayout = toAVChannelLayout(obtainedSpec.channels);
    av_opt_set_chlayout(swrCtx, "out_chlayout", &obtainedChLayout, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", obtainedSpec.freq, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", toAVSampleFormat(obtainedSpec.format), 0);
    av_channel_layout_uninit(&obtainedChLayout); // 释放 toAVChannelLayout 可能分配的内存

    // 音量设置
    av_opt_set_double(swrCtx, "out_volume", volume, 0);

    int ret = swr_init(swrCtx);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr init failed! Error:%s\n", errorBuffer);
        freeResources(); // 统一清理
        return false;
    }

    SDL_PauseAudioDevice(m_audioDeviceID, 1); // 默认暂停音频设备

    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);
    return true;
}

// (新增) openSwrContext2, 使用资源 2
bool AudioPlayer::openSwrContext2()
{
    // 仅在 MIXING 模式下有效, 且必须已打开音频设备
    if (outputMode.load() != OUTPUT_MIXING || m_audioDeviceID == 0 || pCodecCtx2 == nullptr)
    {
        return false;
    }

    swrCtx2 = swr_alloc();
    if (!swrCtx2)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "预加载: swr alloc failed! Error:\n");
        return false;
    }

    // 输入参数 (来自 pCodecCtx2)
    av_opt_set_chlayout(swrCtx2, "in_chlayout", &(pCodecCtx2->ch_layout), 0);
    av_opt_set_int(swrCtx2, "in_sample_rate", pCodecCtx2->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx2, "in_sample_fmt", pCodecCtx2->sample_fmt, 0);

    // 输出参数 (来自 mixingParams, 因为这是 MIXING 模式)
    av_opt_set_chlayout(swrCtx2, "out_chlayout", &(mixingParams.ch_layout), 0);
    av_opt_set_int(swrCtx2, "out_sample_rate", mixingParams.sampleRate, 0);
    av_opt_set_sample_fmt(swrCtx2, "out_sample_fmt", mixingParams.sampleFormat, 0);

    // 音量设置
    av_opt_set_double(swrCtx2, "out_volume", volume, 0);

    int ret = swr_init(swrCtx2);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "预加载: swr init failed! Error:%s\n", errorBuffer);
        swr_free(&swrCtx2);
        return false;
    }
    SDL_Log("预加载: SwrContext2 初始化成功");
    return true;
}

// (修改) 移除 static 变量，使用成员变量
void AudioPlayer::sdl2_audio_callback(void *userdata, Uint8 *stream, int len)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(userdata);
    // (移除) static AudioFrame *currentFrame = nullptr;
    // (移除) static int currentFramePos = 0;

    int remaining = len;
    Uint8 *streamPos = stream;
    bool needsNotify = false; // (新增) 标记是否需要唤醒解码器

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
                // (新增) 检查队列是否之前是满的
                if (player->audioFrameQueue.size() >= player->audioFrameQueueMaxSize.load())
                {
                    needsNotify = true; // 消费后队列将不再满，需要唤醒
                }
                player->m_currentFrame = player->audioFrameQueue.front();
                player->audioFrameQueue.pop();
                player->m_currentFramePos = 0;
                player->nowPlayingTime.store(static_cast<int64_t>(player->m_currentFrame->pts));
            }
            lock.unlock();

            // (新增) 在锁外通知
            if (needsNotify)
            {
                player->decoderCondVar.notify_one();
                needsNotify = false;
            }
        }

        int frameRemaining = player->m_currentFrame->size - player->m_currentFramePos;
        int copySize = std::min(frameRemaining, remaining);

        memcpy(streamPos, player->m_currentFrame->data + player->m_currentFramePos, copySize);

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

// (修改) setPath1, 实现手动切歌逻辑
bool AudioPlayer::setPath1(const std::string &path)
{
    if (!isValidAudio(path))
    {
        return false;
    }

    // (新增) 手动设置路径时，清除预加载路径
    {
        std::lock_guard<std::mutex> lock(preloadPathMutex);
        if (!preloadPath.empty())
        {
            SDL_Log("手动切歌, 清除预加载: %s", preloadPath.c_str());
            freeResources2(); // 立即释放预加载资源
        }
    }

    {
        std::lock_guard<std::mutex> lock(path1Mutex);
        currentPath = path;
    }

    // (新增) 停止当前播放，以强制重新加载
    playingState.store(PlayerState::STOPPED);
    decoderCondVar.notify_one(); // 唤醒解码线程(如果它在wait)
    path1CondVar.notify_one();   // 唤醒解码线程(如果它在wait path)

    SDL_Log("设置播放路径: %s\n", path.c_str());
    return true;
}

// (新增) setPreloadPath
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
    }

    freeResources2(); // 释放旧的预加载资源
    preloadPath = path;
    hasPreloaded.store(false); // 标记为尚未加载
    SDL_Log("设置预加载路径: %s", preloadPath.c_str());
}

int64_t AudioPlayer::getNowPlayingTime() const
{
    return nowPlayingTime.load();
}

int64_t AudioPlayer::getAudioLength() const
{
    return audioDuration.load() / AV_TIME_BASE;
}

// (修改) 完整的 mainDecodeThread 重构
void AudioPlayer::mainDecodeThread()
{
    bool isFileEOF = false;
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

        // 2. 初始化 (内部已包含失败时的 freeResources)
        if (!initDecoder())
        {
            continue; // 初始化解码器失败，继续等待新的路径
        }
        if (!openAudioDevice())
        {
            continue; // 打开音频设备失败，继续等待新的路径
        }

        // 3. (修改) 初始化成功，根据 isFirstPlay 决定状态
        isFileEOF = false;
        if (isFirstPlay.load())
        {
            playingState.store(PlayerState::PAUSED);
            hasPaused.store(true);
            // 此时 SDL_PauseAudioDevice(m_audioDeviceID, 1); 已在 openAudioDevice 中调用
        }
        else
        {
            playingState.store(PlayerState::PLAYING);
            SDL_PauseAudioDevice(m_audioDeviceID, 0); // 立即开始播放
            hasPaused.store(false);
        }

        // 4. 主解码循环
        while (quitFlag.load() == false)
        {
            PlayerState currentState = playingState.load();

            // (新增) 状态处理：STOPPED (用于手动切歌)
            if (currentState == PlayerState::STOPPED)
            {
                SDL_Log("Playback stopped.");
                break; // 退出内层循环, 将在外层 freeResources()
            }

            // 状态处理：SEEKING
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

                AVRational tb = pFormatCtx1->streams[audioStreamIndex1]->time_base;
                int64_t stream_ts = av_rescale_q(target, AV_TIME_BASE_Q, tb);
                int ret = av_seek_frame(pFormatCtx1, audioStreamIndex1, stream_ts, AVSEEK_FLAG_BACKWARD);
                if (ret < 0)
                {
                    SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Error seeking audio stream:%s", av_err2str(ret));
                }

                avcodec_flush_buffers(pCodecCtx1);
                if (swrCtx)
                {
                    swr_init(swrCtx); // 重置重采样器状态
                }

                isFileEOF = false;                        // 跳转后文件不再是 EOF
                playingState.store(PlayerState::PLAYING); // 跳转完成，恢复播放
                SDL_PauseAudioDevice(m_audioDeviceID, 0); // 恢复播放
                hasPaused.store(false);
                continue;
            }

            // (修改) 状态处理：PAUSED / 队列已满 (使用条件变量等待)
            {
                std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
                decoderCondVar.wait(lock, [this, &isFileEOF]
                                    {
                                        PlayerState state = playingState.load();
                                        if (quitFlag.load() || state == PlayerState::STOPPED || state == PlayerState::SEEKING)
                                            return true;
                                        if (state == PlayerState::PAUSED)
                                            return false; // 暂停时等待
                                        // 播放中
                                        return (audioFrameQueue.size() < audioFrameQueueMaxSize.load() || isFileEOF); // 队列未满或已读完
                                    });

                currentState = playingState.load(); // 重新获取状态

                if (quitFlag.load() || currentState == PlayerState::STOPPED)
                {
                    break; // 退出或停止
                }
                if (currentState == PlayerState::SEEKING)
                {
                    continue; // 跳转
                }

                if (currentState == PlayerState::PAUSED)
                {
                    if (hasPaused.load() == false)
                    {
                        SDL_PauseAudioDevice(m_audioDeviceID, 1); // 暂停音频设备
                        hasPaused.store(true);
                    }
                    continue; // 继续等待 (wait 会重新检查)
                }

                // 此时一定是 PLAYING 状态
                if (hasPaused.load() == true)
                {
                    SDL_PauseAudioDevice(m_audioDeviceID, 0); // 恢复音频设备
                    hasPaused.store(false);
                }
            }

            // (修改) 检查：文件已读完
            if (isFileEOF)
            {
                std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
                if (audioFrameQueue.empty() && m_currentFrame == nullptr)
                {
                    SDL_Log("音频播放完毕: %s", currentPath.c_str());

                    // (新增) 检查是否可以无缝切换
                    bool didSwitch = false;
                    if (outputMode.load() == OUTPUT_MIXING && hasPreloaded.load())
                    {
                        std::lock_guard<std::mutex> preloadLock(preloadPathMutex);
                        if (pFormatCtx2 != nullptr && swrCtx2 != nullptr) // 确保预加载已成功
                        {
                            SDL_Log("无缝切换到: %s", preloadPath.c_str());
                            // 释放资源 1
                            freeResources1();

                            // 将资源 2 切换到资源 1
                            pFormatCtx1 = pFormatCtx2;
                            pCodecParameters1 = pCodecParameters2;
                            pCodecCtx1 = pCodecCtx2;
                            pCodec1 = pCodec2;
                            audioStreamIndex1 = audioStreamIndex2;
                            swrCtx = swrCtx2;
                            currentPath = preloadPath;
                            audioDuration.store(pFormatCtx1->duration);

                            // 清空资源 2 (指针)
                            pFormatCtx2 = nullptr;
                            pCodecParameters2 = nullptr;
                            pCodecCtx2 = nullptr;
                            pCodec1 = nullptr;
                            audioStreamIndex2 = -1;
                            swrCtx2 = nullptr;
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
                        break; // 无法切换或非 mixing 模式，退出内层循环
                    }
                    // 如果切换成功，循环继续，开始解码新的 pFormatCtx1
                }
                else
                {
                    // 队列未空，休眠一下等待队列消耗
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }

            if (isFileEOF)
                continue; // (新增) 如果刚切换完，或 EOF 等待队列清空，则跳过解码

            // --- 执行解码 ---
            av_packet_unref(packet);
            int ret = av_read_frame(pFormatCtx1, packet);
            if (ret < 0)
            {
                if (ret == AVERROR_EOF)
                {
                    isFileEOF = true;
                    SDL_Log("AVERROR_EOF reached for: %s", currentPath.c_str());
                }
                else
                {
                    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error reading frame: %s\n", av_err2str(ret));
                    break;
                }
                continue;
            }

            if (packet->stream_index != audioStreamIndex1)
            {
                continue; // 不是我们的音频流
            }

            ret = avcodec_send_packet(pCodecCtx1, packet);
            if (ret < 0)
            {
                SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error sending packet for decoding: %s\n", av_err2str(ret));
                break;
            }

            while (ret >= 0)
            {
                ret = avcodec_receive_frame(pCodecCtx1, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    break;
                }
                else if (ret < 0)
                {
                    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Error during decoding: %s\n", av_err2str(ret));
                    break;
                }

                // --- 解码成功，开始重采样 ---

                int64_t best_pts = frame->best_effort_timestamp;
                double pts = (best_pts == AV_NOPTS_VALUE) ? -1.0 :
                                                            best_pts * av_q2d(pFormatCtx1->streams[audioStreamIndex1]->time_base);

                // (新增) 检查是否需要触发预加载 (仅 Mixing 模式)
                if (outputMode.load() == OUTPUT_MIXING && !hasPreloaded.load() && pts > 0)
                {
                    std::string path_to_preload;
                    {
                        std::lock_guard<std::mutex> lock(preloadPathMutex);
                        path_to_preload = preloadPath;
                    }

                    if (!path_to_preload.empty())
                    {
                        double durationSec = audioDuration.load() / (double)AV_TIME_BASE;
                        // 提前 10 秒开始预加载
                        if ((durationSec - pts) < 10.0)
                        {
                            SDL_Log("Near end of track, starting preload for: %s", path_to_preload.c_str());
                            if (initDecoder2() && openSwrContext2())
                            {
                                hasPreloaded.store(true);
                            }
                            else
                            {
                                SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to preload track.");
                                freeResources2(); // 清理失败的预加载
                            }
                        }
                    }
                }

                // --- 重采样逻辑 ---
                int out_sample_rate = 0;
                int out_nb_channels = 0;
                AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_NONE;

                av_opt_get_int(swrCtx, "out_sample_rate", 0, (int64_t *)&out_sample_rate);
                av_opt_get_int(swrCtx, "out_sample_fmt", 0, (int64_t *)&out_sample_fmt);
                AVChannelLayout out_ch_layout;
                av_opt_get_chlayout(swrCtx, "out_chlayout", 0, &out_ch_layout);
                out_nb_channels = out_ch_layout.nb_channels;
                av_channel_layout_uninit(&out_ch_layout);

                int out_samples = av_rescale_rnd(
                    swr_get_delay(swrCtx, frame->sample_rate) + frame->nb_samples,
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

                int converted_samples = swr_convert(swrCtx, &out_buffer, out_samples,
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

                // ... (自动计算队列大小逻辑不变) ...
                if (!hasCalculatedQueueSize.load() && totalDecodedFrames.load() >= 5)
                {
                    int64_t totalBytes = totalDecodedBytes.load();
                    int64_t totalFrames = totalDecodedFrames.load();
                    int avgFrameBytes = std::max(1, static_cast<int>(totalBytes / totalFrames));
                    double bufferDuration = 0.2;
                    int bytesPerSecond = out_sample_rate * out_nb_channels * out_bytes_per_sample;
                    int targetBufferBytes = static_cast<int>(bytesPerSecond * bufferDuration);
                    int maxFrames = std::max(2, targetBufferBytes / avgFrameBytes);
                    audioFrameQueueMaxSize.store(maxFrames);
                    hasCalculatedQueueSize.store(true);
                    SDL_Log("Auto-calculated audio buffer: %.1f ms buffer, %d frames (avgFrame=%d bytes, sampleRate=%d, ch=%d, bytes/sample=%d)",
                            bufferDuration * 1000.0, maxFrames, avgFrameBytes, out_sample_rate, out_nb_channels, out_bytes_per_sample);
                }

                av_frame_unref(frame); // (修改) 释放 frame 引用
            }

            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            {
                SDL_LogError(SDL_LOG_CATEGORY_ERROR, "A critical error occurred during decode/resample, stopping playback.");
                break; // 发生严重错误，退出内层循环
            }
        }

        // 5. 内层循环退出（播放完毕、出错或退出），清理资源
        SDL_Log("内层循环退出Cleaning up resources for %s", currentPath.c_str());
        freeResources();
    }

    // 释放循环外分配的内存
    av_frame_free(&frame);
    av_packet_free(&packet);
}