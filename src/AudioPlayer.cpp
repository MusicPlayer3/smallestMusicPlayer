#include "AudioPlayer.hpp"
#include <QMediaDevices>
#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_log.h>
#include <chrono>
#include <mutex>
#include <string>
#include <iostream>
#ifdef USE_QT
QAudioFormat::SampleFormat toQtAudioFormat(AVSampleFormat avFormat)
{
    switch (avFormat)
    {
    case AV_SAMPLE_FMT_U8: // unsigned 8 bits
        return QAudioFormat::UInt8;

    case AV_SAMPLE_FMT_S16: // signed 16 bits
        return QAudioFormat::Int16;

    case AV_SAMPLE_FMT_S32: // signed 32 bits
        return QAudioFormat::Int32;

        // FFmpeg 6.0 之后 AV_SAMPLE_FMT_S64 被分为 S64 和 S64P
        // 早期版本只有 AV_SAMPLE_FMT_S64 (交错)
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 2, 100) // FFmpeg 6.0+
    case AV_SAMPLE_FMT_S64:                             // signed 64 bits
        std::cerr << "QAudioFormat does not directly support 64-bit integer audio format.";
        return QAudioFormat::Unknown;
#endif

    case AV_SAMPLE_FMT_FLT: // float
        return QAudioFormat::Float;

    // QAudioFormat 不直接支持 double，但可以转换为 Float
    case AV_SAMPLE_FMT_DBL:
        std::cerr << "QAudioFormat does not directly support double audio format. Consider converting to float.";
        return QAudioFormat::Unknown;

    // 所有平面格式 (planar) 都不被 QAudioSink 直接支持
    case AV_SAMPLE_FMT_U8P:
    case AV_SAMPLE_FMT_S16P:
    case AV_SAMPLE_FMT_S32P:
    case AV_SAMPLE_FMT_FLTP:
    case AV_SAMPLE_FMT_DBLP:
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 2, 100) // FFmpeg 6.0+
    case AV_SAMPLE_FMT_S64P:
#endif
        std::cerr << "Planar audio format is not directly supported by QAudioSink. Resampling is required.";
        return QAudioFormat::Unknown;

    default:
        std::cerr << "Unknown AVSampleFormat:" << av_get_sample_fmt_name(avFormat);
        return QAudioFormat::Unknown;
    }
}
#endif
#ifdef USE_SDL
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
        std::cerr << "不支持的 FFmpeg 采样格式: " << av_get_sample_fmt_name(ffmpegFormat);
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
#endif

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
#ifdef USE_SDL
    if (SDL_Init(SDL_INIT_AUDIO))
    {
        std::cerr << "无法初始化 SDL: " << SDL_GetError() << std::endl;
        exit(1); // 如果 SDL 初始化失败，则退出程序
    }
#endif
    decodeThread = std::thread(&AudioPlayer::mainDecodeThread, this);
    decodeThread.detach();
}

void AudioPlayer::freeffmpegResources()
{
    if (pFormatCtx1)
    {
        avformat_close_input(&pFormatCtx1);
        pFormatCtx1 = nullptr;
    }
    if (pCodecCtx1)
    {
        avcodec_free_context(&pCodecCtx1);
        pCodecCtx1 = nullptr;
    }
    if (swrCtx)
    {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }
    currentPath = "";
}

/**
 * @brief 检查给定路径的文件是否为有效的音频文件
 * @param path 音频文件的路径
 * @return 如果是有效的音频文件返回true，否则返回false
 */
bool AudioPlayer::isValidAudio(const std::string &path)
{
    AVFormatContext *pFormatCtx = nullptr;
    std::cout << "检查音频文件: " << path << std::endl;
    if (avformat_open_input(&pFormatCtx, path.c_str(), nullptr, nullptr) != 0) // 打开文件，尝试解析文件头
    {
        return false; // 打开文件失败，返回false
    }
    if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) // 获取流信息，包括解码所需的信息
    {
        avformat_close_input(&pFormatCtx); // 获取流信息失败，关闭输入文件
        return false;                      // 返回false
    }
    // 查找音频流，检查文件中是否包含音频流
    if (av_find_best_stream(pFormatCtx,
                            AVMEDIA_TYPE_AUDIO, // 指定查找音频流
                            -1,
                            -1,
                            nullptr,
                            0)
        < 0)
    {
        avformat_close_input(&pFormatCtx); // 没有找到音频流，关闭输入文件
        return false;                      // 返回false
    }
    // 查找解码器
    const AVCodec *pCodec = avcodec_find_decoder(pFormatCtx->streams[0]->codecpar->codec_id);
    if (pCodec == nullptr)
    {
        avformat_close_input(&pFormatCtx); // 没有找到解码器，关闭输入文件
        return false;                      // 返回false
    }

    avformat_close_input(&pFormatCtx); // 成功找到音频流，关闭输入文件
    return true;                       // 返回true，表示文件是有效的音频文件
}

void AudioPlayer::setVolume(double vol)
{
    volume = vol;
    if (swrCtx)
    {
        av_opt_set_double(swrCtx, "out_volume", volume, 0); // 设置输出音量
    }
}

// 设置播放进度，单位为秒
void AudioPlayer::seek(int64_t time)
{
    seekTarget.store(static_cast<int64_t>(time * AV_TIME_BASE));
    playingState.store(PlayerState::SEEKING);
    SDL_Log("跳转到 %ld 秒", time); // 打印跳转请求
}

void AudioPlayer::play()
{
    playingState.store(PlayerState::PLAYING);
}

void AudioPlayer::pause()
{
    playingState.store(PlayerState::PAUSED);
}

void AudioPlayer::setMixingParameters(const AudioParams &params)
{
    // TODO:设置混音器参数（向解码线程发送重新初始化混音器信号）
}

void AudioPlayer::setMixingParameters(int sampleRate, AVSampleFormat sampleFormat, uint64_t channelLayout, int channels)
{
    // TODO:设置混音器参数（向解码线程发送重新初始化混音器信号）
}

AudioParams AudioPlayer::getMixingParameters() const
{
    return mixingParams;
}

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
        currentPath = ""; // 清空path1
        return false;
    }
    // 查找流信息
    ret = avformat_find_stream_info(pFormatCtx1, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "无法找到流信息: %s, Error:\n%s", currentPath.c_str(), errorBuffer);
        avformat_close_input(&pFormatCtx1);
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
        avformat_close_input(&pFormatCtx1);
        currentPath = ""; // 清空path1
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
        avformat_close_input(&pFormatCtx1);
        currentPath = ""; // 清空path1
        return false;
    }
    // 创建解码器上下文
    pCodecCtx1 = avcodec_alloc_context3(pCodec1);
    if (pCodecCtx1 == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法分配解码器上下文: %s", currentPath.c_str());
        avformat_close_input(&pFormatCtx1);
        currentPath = ""; // 清空path1
        return false;
    }
    // 将解码器参数复制到上下文
    ret = avcodec_parameters_to_context(pCodecCtx1, pCodecParameters1);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法将解码器参数复制到上下文: %s, Error:\n%s", currentPath.c_str(), errorBuffer);
        avcodec_free_context(&pCodecCtx1);
        avformat_close_input(&pFormatCtx1);
        currentPath = ""; // 清空path1
        return false;
    }
    // 打开解码器
    ret = avcodec_open2(pCodecCtx1, pCodec1, nullptr);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "无法打开解码器: %s, Error:\n%s", currentPath.c_str(), errorBuffer);
        avcodec_free_context(&pCodecCtx1);
        avformat_close_input(&pFormatCtx1);
        currentPath = ""; // 清空path1
        return false;
    }

    audioDuration.store(pFormatCtx1->duration); // 获取音频总时长

    std::cout << "音乐总时长：" << audioDuration.load();

    lock.unlock();
    return true;
}
#ifdef USE_QT
bool AudioPlayer::openAudioDevice()
{
    QAudioFormat outputFormat; // 设置输出格式

    // 如果输出模式为重采样模式，则创建重采样器
    if (m_outputMode.load() == OUTPUT_MIXING)
    {
        // 设置音频参数
        swrCtx = swr_alloc();
        if (!swrCtx)
        {
            std::cerr << "无法分配重采样上下文";
            avcodec_free_context(&pCodecCtx1);
            avformat_close_input(&pFormatCtx1);
            currentPath = ""; // 清空path1
            return false;
        }
        // 输入参数
        av_opt_set_chlayout(swrCtx, "in_chlayout", &(pCodecCtx1->ch_layout), 0);
        av_opt_set_int(swrCtx, "in_sample_rate", pCodecCtx1->sample_rate, 0);
        av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", pCodecCtx1->sample_fmt, 0);

        // 输出参数
        av_opt_set_chlayout(swrCtx, "out_chlayout", &(mixingParams.ch_layout), 0);
        av_opt_set_int(swrCtx, "out_sample_rate", mixingParams.sampleRate, 0);
        av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", mixingParams.sampleFormat, 0);

        // 初始化重采样器
        if (swr_init(swrCtx) < 0)
        {
            std::cerr << "无法初始化重采样器\n";
            currentPath = ""; // 清空path1
            swr_free(&swrCtx);
            swrCtx = nullptr;
            avcodec_free_context(&pCodecCtx1);
            avformat_close_input(&pFormatCtx1);
            return false;
        }

        outputFormat.setSampleRate(mixingParams.sampleRate);
        outputFormat.setChannelCount(mixingParams.ch_layout.nb_channels);
        outputFormat.setSampleFormat(toQtAudioFormat(mixingParams.sampleFormat));
    }
    else
    {
        outputFormat.setSampleRate(pCodecCtx1->sample_rate);
        outputFormat.setChannelCount(pCodecCtx1->ch_layout.nb_channels);
        outputFormat.setSampleFormat(toQtAudioFormat(pCodecCtx1->sample_fmt));
    }

    const auto &devices = QMediaDevices::defaultAudioOutput();
    if (!devices.isFormatSupported(outputFormat))
    {
        std::cerr << "音频设备不支持此格式, 使用默认格式代替";
        outputFormat = devices.preferredFormat();
        std::cerr << "默认格式: "
                  << "采样率:" << outputFormat.sampleRate()
                  << " 通道数:" << outputFormat.channelCount()
                  << " 采样格式:" << outputFormat.sampleFormat();
        if (!devices.isFormatSupported(outputFormat))
        {
            std::cerr << "默认格式同样不被支持！";
            // 清理资源
            if (swrCtx)
            {
                swr_free(&swrCtx);
            }
            avcodec_free_context(&pCodecCtx1);
            avformat_close_input(&pFormatCtx1);
            currentPath = "";
            return false;
        }
    }

    // --- 4. 创建并打开音频设备 ---
    auto m_audioSink = new QAudioSink(outputFormat);
    auto m_audioDevice = m_audioSink->start();
    if (!m_audioDevice)
    {
        std::cerr << "无法打开音频设备!";
        delete m_audioSink;
        m_audioSink = nullptr;
        // 清理其他资源
        if (swrCtx)
        {
            swr_free(&swrCtx);
        }
        avcodec_free_context(&pCodecCtx1);
        avformat_close_input(&pFormatCtx1);
        currentPath = "";
        return false;
    }

    return true;
}
#endif

#ifdef USE_SDL
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
        desiredSpec.freq = pCodecCtx1->sample_rate;
        desiredSpec.format = toSDLFormat(pCodecCtx1->sample_fmt);
        desiredSpec.channels = pCodecCtx1->ch_layout.nb_channels;
        desiredSpec.samples = 4096;
        desiredSpec.callback = AudioPlayer::sdl2_audio_callback;
        desiredSpec.userdata = this;
    }

    if (SDL_OpenAudio(&desiredSpec, &obtainedSpec) < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "audio device open failed! Error:\n%s", SDL_GetError());
        return false;
    }

    swrCtx = swr_alloc();
    if (!swrCtx)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr alloc failed! Error:\n");
        avcodec_free_context(&pCodecCtx1);
        avformat_close_input(&pFormatCtx1);
        currentPath = ""; // 清空path1
        return false;
    }
    // 输入参数
    av_opt_set_chlayout(swrCtx, "in_chlayout", &(pCodecCtx1->ch_layout), 0);
    av_opt_set_int(swrCtx, "in_sample_rate", pCodecCtx1->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", pCodecCtx1->sample_fmt, 0);

    // 输出参数
    AVChannelLayout obtainedChLayout = toAVChannelLayout(obtainedSpec.channels);
    av_opt_set_chlayout(swrCtx, "out_chlayout", &obtainedChLayout, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", obtainedSpec.freq, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", toAVSampleFormat(obtainedSpec.format), 0);

    // 音量设置
    av_opt_set_double(swrCtx, "out_volume", volume, 0);

    int ret = swr_init(swrCtx);
    if (ret < 0)
    {
        av_strerror(ret, errorBuffer, sizeof(errorBuffer));
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "swr init failed! Error:%s\n", errorBuffer);
        swr_free(&swrCtx);
        avcodec_free_context(&pCodecCtx1);
        avformat_close_input(&pFormatCtx1);
        currentPath = ""; // 清空path1
        return false;
    }
    SDL_PauseAudio(1); // 暂停音频设备

    totalDecodedBytes.store(0);
    totalDecodedFrames.store(0);
    hasCalculatedQueueSize.store(false);
    return true;
}
#endif

void AudioPlayer::sdl2_audio_callback(void *userdata, Uint8 *stream, int len)
{
    AudioPlayer *player = static_cast<AudioPlayer *>(userdata);
    static AudioFrame *currentFrame = nullptr;
    static int currentFramePos = 0;

    int remaining = len;
    Uint8 *streamPos = stream;

    while (remaining > 0)
    {
        if (currentFrame == nullptr)
        {
            std::unique_lock<std::mutex> lock(player->audioFrameQueueMutex);
            if (player->audioFrameQueue.empty())
            {
                // 音频帧队列为空，填充静音数据
                memset(streamPos, 0, remaining);
                return;
            }
            else
            {
                currentFrame = player->audioFrameQueue.front();
                player->audioFrameQueue.pop();
                currentFramePos = 0;
                player->nowPlayingTime.store(static_cast<int64_t>(currentFrame->pts));
            }
            lock.unlock();
        }

        int frameRemaining = currentFrame->size - currentFramePos;
        int copySize = std::min(frameRemaining, remaining);

        memcpy(streamPos, currentFrame->data + currentFramePos, copySize);

        remaining -= copySize;
        streamPos += copySize;
        currentFramePos += copySize;

        if (currentFramePos == currentFrame->size)
        {
            // 当前帧播放完毕，释放内存
            delete currentFrame;
            currentFrame = nullptr; // 重置当前帧指针
            currentFramePos = 0;
        }
    }
}
bool AudioPlayer::setPath1(const std::string &path)
{
    if (isValidAudio(path))
    {
        std::unique_lock<std::mutex> lock(path1Mutex);
        currentPath = path;
        path1CondVar.notify_one();
        std::cout << "设置播放路径: " << currentPath << std::endl;
        return true;
    }
    return false;
}

int64_t AudioPlayer::getNowPlayingTime() const
{
    // TODO: 获取当前播放时间（单位为秒）
    return nowPlayingTime.load();
}

int64_t AudioPlayer::getAudioLength() const
{
    return audioDuration.load() / AV_TIME_BASE;
}
void AudioPlayer::mainDecodeThread()
{
    while (quitFlag.load() == false)
    {
        if (hasPreloaded)
        {
            // TODO: 切换到预加载的音乐
            hasPreloaded = false;
        }
        else
        {
            {
                std::unique_lock<std::mutex> lock(path1Mutex);
                path1CondVar.wait(lock, [this]
                                  { return (!currentPath.empty()) || quitFlag.load(); });
                lock.unlock();
                if (quitFlag.load())
                {
                    break;
                }
            }
            if (!initDecoder())
            {
                continue; // 初始化解码器失败，继续等待新的路径
            }
            if (!openAudioDevice())
            {
                continue; // 打开音频设备失败，继续等待新的路径
            }
        }

        // 主解码循环
        while (quitFlag.load() == false)
        {
            if (playingState.load() == PlayerState::SEEKING)
            {
                // TODO: 处理跳转逻辑
                int64_t target = seekTarget.load();
                // 清空音频队列
                {
                    std::unique_lock<std::mutex> lock(audioFrameQueueMutex);
                    while (!audioFrameQueue.empty())
                    {
                        delete audioFrameQueue.front();
                        audioFrameQueue.pop();
                    }
                }

                AVRational tb = pFormatCtx1->streams[audioStreamIndex1]->time_base;
                int64_t stream_ts = av_rescale_q(target, AV_TIME_BASE_Q, tb);
                int ret = av_seek_frame(pFormatCtx1, audioStreamIndex1, stream_ts, AVSEEK_FLAG_BACKWARD);
                if (ret < 0)
                {
                    std::cerr << "Error seeking audio stream: " << av_err2str(ret) << std::endl;
                    continue;
                }
                avcodec_flush_buffers(pCodecCtx1);
                if (swrCtx)
                {
                    swr_init(swrCtx);
                }
                playingState.store(PlayerState::PLAYING);
            }

            if (playingState.load() == PlayerState::PAUSED)
            {
                if (hasPaused.load() == false)
                {
                    SDL_PauseAudio(1); // 暂停音频设备
                    hasPaused.store(true);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            else if (playingState.load() == PlayerState::PLAYING)
            {
                if (hasPaused.load() == true)
                {
                    SDL_PauseAudio(0); // 恢复音频设备
                    hasPaused.store(false);
                }
                if (audioFrameQueue.size() > audioFrameQueueMaxSize.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                else
                {
                    // TODO: 解码音频帧并放入队列
                    AVPacket *packet = av_packet_alloc();
                    if (!packet)
                    {
                        std::cerr << "Failed to allocate packet" << std::endl;
                        break;
                    }
                    // 读取一个音频帧
                    int ret = av_read_frame(pFormatCtx1, packet);
                    if (ret < 0)
                    {
                        if (ret == AVERROR_EOF)
                        {
                            // TODO:
                            std::cout << "End of file reached" << std::endl;
                        }
                        else
                        {
                            std::cerr << "Error reading frame: " << av_err2str(ret) << std::endl;
                            break;
                        }
                    }

                    if (packet->stream_index == audioStreamIndex1)
                    {
                        // 将包发送至解码器
                        ret = avcodec_send_packet(pCodecCtx1, packet);
                        if (ret < 0)
                        {
                            std::cerr << "Error sending packet for decoding: " << av_err2str(ret) << std::endl;
                            break;
                        }

                        AVFrame *frame = av_frame_alloc();
                        if (!frame)
                        {
                            std::cerr << "Failed to allocate frame" << std::endl;
                            break;
                        }
                        // 从解码器接收解码后的帧
                        ret = avcodec_receive_frame(pCodecCtx1, frame);

                        if (ret < 0)
                        {
                            std::cerr << "Error during decoding: " << av_err2str(ret) << std::endl;
                            av_frame_free(&frame);
                            continue;
                        }

                        int64_t best_pts = frame->best_effort_timestamp;
                        double pts = (best_pts == AV_NOPTS_VALUE) ? -1.0 :
                                                                    best_pts * av_q2d(pFormatCtx1->streams[audioStreamIndex1]->time_base);

                        // 重采样音频帧
                        int out_samples = av_rescale_rnd(
                            swr_get_delay(swrCtx, frame->sample_rate) + frame->nb_samples,
                            (outputMode.load() == OUTPUT_MIXING) ? mixingParams.sampleRate : pCodecCtx1->sample_rate,
                            frame->sample_rate, AV_ROUND_UP);

                        // 分配重采样后的数据缓冲区
                        int out_nb_channels = (outputMode.load() == OUTPUT_MIXING) ? mixingParams.ch_layout.nb_channels : pCodecCtx1->ch_layout.nb_channels;
                        int out_bytes_per_sample = av_get_bytes_per_sample(
                            (outputMode.load() == OUTPUT_MIXING) ? mixingParams.sampleFormat : pCodecCtx1->sample_fmt);

                        int out_buffer_size = out_samples * out_nb_channels * out_bytes_per_sample;

                        uint8_t *out_buffer = (uint8_t *)av_malloc(out_buffer_size);
                        if (!out_buffer)
                        {
                            std::cerr << "Failed to allocate output buffer" << std::endl;
                            av_frame_free(&frame);
                            break;
                        }

                        // 重采样
                        uint8_t **in_data = frame->data;
                        int in_nb_samples = frame->nb_samples;

                        int converted_samples = swr_convert(swrCtx, &out_buffer, out_samples,
                                                            (const uint8_t **)in_data, in_nb_samples);

                        if (converted_samples < 0)
                        {
                            std::cerr << "Error resampling audio frame" << std::endl;
                            av_free(out_buffer);
                            av_frame_free(&frame);
                            break;
                        }

                        // 将重采样后的数据放入队列
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

                        // 自动计算队列大小
                        if (!hasCalculatedQueueSize.load() && totalDecodedFrames.load() >= 5)
                        {
                            int64_t totalBytes = totalDecodedBytes.load();
                            int64_t totalFrames = totalDecodedFrames.load();
                            int avgFrameBytes = std::max(1, static_cast<int>(totalBytes / totalFrames));

                            // 获取当前播放参数（由 openAudioDevice 设置）
                            int sampleRate = (outputMode.load() == OUTPUT_MIXING) ? mixingParams.sampleRate : pCodecCtx1->sample_rate;
                            int channels = (outputMode.load() == OUTPUT_MIXING) ? mixingParams.ch_layout.nb_channels : pCodecCtx1->ch_layout.nb_channels;
                            int bytesPerSample = av_get_bytes_per_sample(
                                (outputMode.load() == OUTPUT_MIXING) ? mixingParams.sampleFormat : pCodecCtx1->sample_fmt);

                            // 200ms 缓冲
                            double bufferDuration = 0.2;
                            int bytesPerSecond = sampleRate * channels * bytesPerSample;
                            int targetBufferBytes = static_cast<int>(bytesPerSecond * bufferDuration);

                            int maxFrames = std::max(2, targetBufferBytes / avgFrameBytes);
                            audioFrameQueueMaxSize.store(maxFrames);
                            hasCalculatedQueueSize.store(true);

                            SDL_Log("Auto-calculated audio buffer: %.1f ms buffer, %d frames (avgFrame=%d bytes, sampleRate=%d, ch=%d, bytes/sample=%d)",
                                    bufferDuration * 1000.0, maxFrames, avgFrameBytes, sampleRate, channels, bytesPerSample);
                        }

                        av_frame_free(&frame);
                    }
                    av_packet_free(&packet);
                }
            }
        }
    }
}
