extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
}

#include <iostream>
#include <queue>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <string>

// 定义音频参数结构
typedef struct AudioParams
{
    int sampleRate;            // 采样率
    AVChannelLayout ch_layout; // 通道布局
    AVSampleFormat format;     // 样本格式
} AudioParams;

// 音频帧结构
typedef struct AudioFrame
{
    uint8_t *data;
    int size;
    double pts;
    AudioFrame() :
        data(nullptr), size(0), pts(0.0)
    {
    }
    ~AudioFrame()
    {
        if (data)
            av_free(data);
    }
} AudioFrame;

// 播放状态枚举
enum class PlaybackState
{
    PLAYING,
    PAUSED,
    SEEKING
};

// 全局变量
static std::atomic<double> audio_clock{0.0};
static int64_t audio_duration = 0;
static std::queue<AudioFrame *> audio_frame_queue;
static std::mutex audio_queue_mutex;
static std::condition_variable audio_queue_cv;
static std::atomic<bool> quit_flag{false};
static SwrContext *global_swr_ctx = nullptr;
static std::atomic<PlaybackState> playback_state{PlaybackState::PLAYING};
static std::atomic<int64_t> seek_target{0};

// GUI相关变量
static SDL_Window *window = nullptr;
static SDL_Renderer *renderer = nullptr;
static TTF_Font *font = nullptr;
static const int WINDOW_WIDTH = 400;
static const int WINDOW_HEIGHT = 200;
static SDL_Rect playPauseButton = {175, 150, 50, 30};
static SDL_Rect rewindButton = {125, 150, 50, 30};
static SDL_Rect forwardButton = {225, 150, 50, 30};

// 信号处理函数
void signal_handler(int signal)
{
    std::cout << "\nReceived interrupt signal, stopping playback..." << std::endl;
    quit_flag.store(true);
    audio_queue_cv.notify_one();
}

// 音频回调函数
void audio_callback(void *userdata, Uint8 *stream, int len)
{
    if (playback_state.load() == PlaybackState::PAUSED)
    {
        // 在暂停状态下填充静音
        SDL_memset(stream, 0, len);
        return;
    }

    static AudioFrame *current_frame = nullptr;
    static int current_frame_pos = 0;
    std::unique_lock<std::mutex> lock(audio_queue_mutex);

    int len_remaining = len;
    Uint8 *stream_ptr = stream;

    while (len_remaining > 0 && !quit_flag.load())
    {
        // 如果当前帧为空或已经播放完，从队列中获取新帧
        if (!current_frame || current_frame_pos >= current_frame->size)
        {
            // 等待队列中有数据或退出信号
            if (audio_frame_queue.empty() && !quit_flag.load())
            {
                audio_queue_cv.wait_for(lock, std::chrono::milliseconds(100));
            }

            if (quit_flag.load())
            {
                // 填充剩余的静音数据
                SDL_memset(stream_ptr, 0, len_remaining);
                return;
            }

            if (!audio_frame_queue.empty())
            {
                current_frame = audio_frame_queue.front();
                audio_frame_queue.pop();
                current_frame_pos = 0;

                // 更新音频时钟
                if (current_frame->pts >= 0)
                {
                    audio_clock.store(current_frame->pts, std::memory_order_relaxed);
                }
            }
            else
            {
                // 队列为空，填充静音
                SDL_memset(stream_ptr, 0, len_remaining);
                return;
            }
        }

        if (current_frame && current_frame_pos < current_frame->size)
        {
            int bytes_to_copy = std::min(current_frame->size - current_frame_pos, len_remaining);

            // 复制音频数据到输出流
            SDL_memcpy(stream_ptr, current_frame->data + current_frame_pos, bytes_to_copy);

            current_frame_pos += bytes_to_copy;
            stream_ptr += bytes_to_copy;
            len_remaining -= bytes_to_copy;

            // 如果当前帧播放完毕，释放它
            if (current_frame_pos >= current_frame->size)
            {
                delete current_frame;
                current_frame = nullptr;
                current_frame_pos = 0;
            }
        }
    }
}

// 暂停播放
void pause_playback()
{
    playback_state.store(PlaybackState::PAUSED);
    SDL_PauseAudio(1);
}

// 继续播放
void resume_playback()
{
    playback_state.store(PlaybackState::PLAYING);
    SDL_PauseAudio(0);
}

// 定位到指定位置
void seek_playback(double target_seconds)
{
    playback_state.store(PlaybackState::SEEKING);
    seek_target.store(static_cast<int64_t>(target_seconds * AV_TIME_BASE));
    std::cout << "seek target: " << static_cast<int64_t>(target_seconds * AV_TIME_BASE) << std::endl;
}

// 前进5秒
void forward_5s()
{
    double current_time = audio_clock.load();
    double duration_in_seconds = (double)audio_duration / AV_TIME_BASE;
    double target_time = std::min(current_time + 5.0, duration_in_seconds);
    seek_playback(target_time);
}

// 后退5秒
void rewind_5s()
{
    double current_time = audio_clock.load();
    double target_time = std::max(current_time - 5.0, 0.0);
    seek_playback(target_time);
}

// 切换播放/暂停状态
void toggle_playback()
{
    if (playback_state.load() == PlaybackState::PLAYING)
    {
        pause_playback();
    }
    else
    {
        resume_playback();
    }
}

// 解码线程函数
// 修复后的解码线程函数
void decode_thread(AVCodecContext *codec_ctx, AVFormatContext *format_ctx,
                   int audio_stream_index)
{
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    // 设置重采样输出参数
    int out_sample_rate = 48000;
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_FLT;
    AVChannelLayout out_ch_layout;
    av_channel_layout_from_mask(&out_ch_layout, AV_CH_LAYOUT_STEREO);

    bool eof = false;

    while (!quit_flag.load())
    {
        // 检查是否有定位请求（优先处理 SEEKING）
        if (playback_state.load() == PlaybackState::SEEKING)
        {
            int64_t target = seek_target.load(); // 单位：AV_TIME_BASE

            // 清空音频队列
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex);
                while (!audio_frame_queue.empty())
                {
                    delete audio_frame_queue.front();
                    audio_frame_queue.pop();
                }
            }

            // 将 AV_TIME_BASE 单位的 target 转换为音频流 time_base 单位，然后对该流进行 seek
            AVRational tb = format_ctx->streams[audio_stream_index]->time_base;
            int64_t stream_ts = av_rescale_q(target, AV_TIME_BASE_Q, tb);

            int ret = av_seek_frame(format_ctx, audio_stream_index, stream_ts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0)
            {
                std::cerr << "Seek failed: " << ret << std::endl;
            }
            else
            {
                // 刷新解码器缓冲区并重置 resampler 状态
                avcodec_flush_buffers(codec_ctx);
                if (global_swr_ctx)
                    swr_init(global_swr_ctx);
                eof = false; // seek 后继续读取
            }

            // 恢复播放状态
            playback_state.store(PlaybackState::PLAYING);
        }

        if (!eof && av_read_frame(format_ctx, packet) >= 0 && !quit_flag.load())
        {
            if (packet->stream_index == audio_stream_index)
            {
                // 发送 packet 到解码器
                int ret = avcodec_send_packet(codec_ctx, packet);
                if (ret < 0)
                {
                    std::cerr << "Error sending packet to decoder: " << ret << std::endl;
                    av_packet_unref(packet);
                    continue;
                }

                // 接收解码后的 frame
                while (avcodec_receive_frame(codec_ctx, frame) == 0 && !quit_flag.load())
                {
                    // 使用 frame->best_effort_timestamp（新版 ffmpeg 推荐直接读取该字段）
                    int64_t best_pts = frame->best_effort_timestamp;
                    double pts = (best_pts == AV_NOPTS_VALUE) ? -1.0 :
                                                                best_pts * av_q2d(format_ctx->streams[audio_stream_index]->time_base);

                    // 重采样音频帧
                    int out_samples = av_rescale_rnd(
                        swr_get_delay(global_swr_ctx, frame->sample_rate) + frame->nb_samples,
                        out_sample_rate, frame->sample_rate, AV_ROUND_UP);

                    // 分配重采样后的数据缓冲区
                    int out_nb_channels = out_ch_layout.nb_channels;
                    int out_bytes_per_sample = av_get_bytes_per_sample(out_sample_fmt);
                    int out_buffer_size = out_samples * out_nb_channels * out_bytes_per_sample;

                    uint8_t *out_buffer = (uint8_t *)av_malloc(out_buffer_size);
                    if (!out_buffer)
                    {
                        std::cerr << "Failed to allocate output buffer" << std::endl;
                        continue;
                    }

                    // 执行重采样
                    uint8_t **in_data = frame->data;
                    int in_nb_samples = frame->nb_samples;

                    int converted_samples = swr_convert(global_swr_ctx, &out_buffer, out_samples,
                                                        (const uint8_t **)in_data, in_nb_samples);

                    if (converted_samples < 0)
                    {
                        std::cerr << "Error resampling audio frame" << std::endl;
                        av_free(out_buffer);
                        continue;
                    }

                    // 创建音频帧并添加到队列
                    AudioFrame *audio_frame = new AudioFrame();
                    audio_frame->size = converted_samples * out_nb_channels * out_bytes_per_sample;
                    audio_frame->data = out_buffer;
                    audio_frame->pts = pts;

                    // 添加到队列
                    {
                        std::lock_guard<std::mutex> lock(audio_queue_mutex);
                        audio_frame_queue.push(audio_frame);
                    }
                    audio_queue_cv.notify_one();
                }
            }
            av_packet_unref(packet);
        }
        else
        {
            // 读取失败（通常为 EOF 或错误），进入 EOF 状态并把解码器缓冲区里的残留帧取出推入队列
            eof = true;

            // 发送刷新包以清空解码器并接收剩余帧
            avcodec_send_packet(codec_ctx, nullptr);
            while (avcodec_receive_frame(codec_ctx, frame) == 0 && !quit_flag.load())
            {
                int64_t best_pts = frame->best_effort_timestamp;
                double pts = (best_pts == AV_NOPTS_VALUE) ? -1.0 :
                                                            best_pts * av_q2d(format_ctx->streams[audio_stream_index]->time_base);

                int out_samples = av_rescale_rnd(
                    swr_get_delay(global_swr_ctx, frame->sample_rate) + frame->nb_samples,
                    out_sample_rate, frame->sample_rate, AV_ROUND_UP);

                int out_nb_channels = out_ch_layout.nb_channels;
                int out_bytes_per_sample = av_get_bytes_per_sample(out_sample_fmt);
                int out_buffer_size = out_samples * out_nb_channels * out_bytes_per_sample;
                uint8_t *out_buffer = (uint8_t *)av_malloc(out_buffer_size);
                if (!out_buffer)
                    continue;

                int converted_samples = swr_convert(global_swr_ctx, &out_buffer, out_samples,
                                                    (const uint8_t **)frame->data, frame->nb_samples);
                if (converted_samples < 0)
                {
                    av_free(out_buffer);
                    continue;
                }

                AudioFrame *audio_frame = new AudioFrame();
                audio_frame->size = converted_samples * out_nb_channels * out_bytes_per_sample;
                audio_frame->data = out_buffer;
                audio_frame->pts = pts;

                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex);
                    audio_frame_queue.push(audio_frame);
                }
                audio_queue_cv.notify_one();
            }

            // 不退出循环；等待可能的 seek 请求或退出信号
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 清理资源
    av_frame_free(&frame);
    av_packet_free(&packet);
    av_channel_layout_uninit(&out_ch_layout);
}

// 渲染文本到屏幕
void renderText(const std::string &text, int x, int y, SDL_Color color)
{
    SDL_Surface *surface = TTF_RenderText_Solid(font, text.c_str(), color);
    if (surface)
    {
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture)
        {
            SDL_Rect dstRect = {x, y, surface->w, surface->h};
            SDL_RenderCopy(renderer, texture, NULL, &dstRect);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(surface);
    }
}

// 绘制GUI
void renderGUI()
{
    // 清空屏幕
    SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
    SDL_RenderClear(renderer);

    // 绘制进度条背景
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    SDL_Rect progressBg = {50, 50, 300, 20};
    SDL_RenderFillRect(renderer, &progressBg);

    // 计算进度条填充
    double current_time = audio_clock.load();
    double duration_in_seconds = (double)audio_duration / AV_TIME_BASE;
    double progress = (duration_in_seconds > 0) ? (current_time / duration_in_seconds) : 0;
    if (progress > 1.0)
        progress = 1.0;

    // 绘制进度条
    SDL_SetRenderDrawColor(renderer, 70, 130, 180, 255);
    SDL_Rect progressFill = {50, 50, static_cast<int>(300 * progress), 20};
    SDL_RenderFillRect(renderer, &progressFill);

    // 绘制时间文本
    SDL_Color textColor = {0, 0, 0, 255};
    std::string timeText = std::to_string(static_cast<int>(current_time)) + "s / " + std::to_string(static_cast<int>(duration_in_seconds)) + "s";
    renderText(timeText, 150, 80, textColor);

    // 绘制按钮
    SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
    SDL_RenderFillRect(renderer, &rewindButton);
    SDL_RenderFillRect(renderer, &playPauseButton);
    SDL_RenderFillRect(renderer, &forwardButton);

    // 绘制按钮文本
    renderText("<<", 140, 155, textColor);

    if (playback_state.load() == PlaybackState::PLAYING)
    {
        renderText("||", 190, 155, textColor);
    }
    else
    {
        renderText(">", 190, 155, textColor);
    }

    renderText(">>", 240, 155, textColor);

    // 更新屏幕
    SDL_RenderPresent(renderer);
}

// 处理GUI事件
bool handleGUIEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
        {
            return true;
        }
        else if (event.type == SDL_MOUSEBUTTONDOWN)
        {
            int x = event.button.x;
            int y = event.button.y;

            // 检查是否点击了播放/暂停按钮
            if (x >= playPauseButton.x && x <= playPauseButton.x + playPauseButton.w && y >= playPauseButton.y && y <= playPauseButton.y + playPauseButton.h)
            {
                toggle_playback();
            }
            // 检查是否点击了后退按钮
            else if (x >= rewindButton.x && x <= rewindButton.x + rewindButton.w && y >= rewindButton.y && y <= rewindButton.y + rewindButton.h)
            {
                rewind_5s();
            }
            // 检查是否点击了前进按钮
            else if (x >= forwardButton.x && x <= forwardButton.x + forwardButton.w && y >= forwardButton.y && y <= forwardButton.y + forwardButton.h)
            {
                forward_5s();
            }
        }
    }
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <audio_file>" << std::endl;
        return -1;
    }

    const char *filename = argv[1];

    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    const AVCodec *codec = nullptr;
    int audio_stream_index = -1;

    // 设置信号处理
    std::signal(SIGINT, signal_handler);

    // 初始化FFmpeg
    avformat_network_init();

    // 打开音频文件
    if (avformat_open_input(&format_ctx, filename, nullptr, nullptr) != 0)
    {
        std::cerr << "Could not open file: " << filename << std::endl;
        return -1;
    }

    // 获取流信息
    if (avformat_find_stream_info(format_ctx, nullptr) < 0)
    {
        std::cerr << "Could not find stream information" << std::endl;
        avformat_close_input(&format_ctx);
        return -1;
    }

    // 查找音频流
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++)
    {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_stream_index = i;
            break;
        }
    }

    if (audio_stream_index == -1)
    {
        std::cerr << "Could not find audio stream" << std::endl;
        avformat_close_input(&format_ctx);
        return -1;
    }

    AVCodecParameters *codec_params = format_ctx->streams[audio_stream_index]->codecpar;
    codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec)
    {
        std::cerr << "Unsupported codec" << std::endl;
        avformat_close_input(&format_ctx);
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
        std::cerr << "Could not allocate codec context" << std::endl;
        avformat_close_input(&format_ctx);
        return -1;
    }

    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0)
    {
        std::cerr << "Could not copy codec parameters to context" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return -1;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0)
    {
        std::cerr << "Could not open codec" << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return -1;
    }

    // 获取音频总时长
    audio_duration = format_ctx->duration;
    double duration_in_seconds = (double)audio_duration / AV_TIME_BASE;
    std::cout << "Audio duration: " << duration_in_seconds << " seconds" << std::endl;

    // 初始化SDL音频子系统
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) < 0)
    {
        std::cerr << "Could not initialize SDL: " << SDL_GetError() << std::endl;
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return -1;
    }

    // 初始化SDL_ttf
    if (TTF_Init() == -1)
    {
        std::cerr << "Could not initialize SDL_ttf: " << TTF_GetError() << std::endl;
        SDL_Quit();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return -1;
    }

    // 创建窗口和渲染器
    window = SDL_CreateWindow("Audio Player",
                              SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              WINDOW_WIDTH, WINDOW_HEIGHT,
                              SDL_WINDOW_SHOWN);
    if (!window)
    {
        std::cerr << "Could not create window: " << SDL_GetError() << std::endl;
        TTF_Quit();
        SDL_Quit();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return -1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
    {
        std::cerr << "Could not create renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return -1;
    }

    // 加载字体
    font = TTF_OpenFont("/usr/share/fonts/truetype/freefont/FreeSans.ttf", 24);
    if (!font)
    {
        // 尝试其他常见字体路径
        font = TTF_OpenFont("/usr/share/fonts/WindowsFonts/simhei.ttf", 24);
        if (!font)
        {
            std::cerr << "Could not load font: " << TTF_GetError() << std::endl;
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            TTF_Quit();
            SDL_Quit();
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            return -1;
        }
    }

    // 设置音频参数
    AudioParams audio_params;
    audio_params.sampleRate = codec_ctx->sample_rate;
    audio_params.ch_layout = codec_ctx->ch_layout;
    audio_params.format = codec_ctx->sample_fmt;

    // 检查通道布局是否有效
    if (audio_params.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    {
        std::cout << "Channel layout is unspecified, using default layout based on channel count: "
                  << audio_params.ch_layout.nb_channels << std::endl;

        // 根据通道数设置默认布局
        AVChannelLayout default_layout;
        if (audio_params.ch_layout.nb_channels == 1)
        {
            av_channel_layout_from_mask(&default_layout, AV_CH_LAYOUT_MONO);
        }
        else if (audio_params.ch_layout.nb_channels == 2)
        {
            av_channel_layout_from_mask(&default_layout, AV_CH_LAYOUT_STEREO);
        }
        else
        {
            av_channel_layout_default(&default_layout, audio_params.ch_layout.nb_channels);
        }

        av_channel_layout_uninit(&audio_params.ch_layout);
        audio_params.ch_layout = default_layout;
    }

    // 初始化重采样器
    global_swr_ctx = swr_alloc();

    // 创建输出通道布局
    AVChannelLayout out_ch_layout;
    av_channel_layout_from_mask(&out_ch_layout, AV_CH_LAYOUT_STEREO);

    // 设置重采样器参数
    av_opt_set_chlayout(global_swr_ctx, "in_chlayout", &audio_params.ch_layout, 0);
    av_opt_set_chlayout(global_swr_ctx, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(global_swr_ctx, "in_sample_rate", audio_params.sampleRate, 0);
    av_opt_set_int(global_swr_ctx, "out_sample_rate", 48000, 0);
    av_opt_set_sample_fmt(global_swr_ctx, "in_sample_fmt", audio_params.format, 0);
    av_opt_set_sample_fmt(global_swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(global_swr_ctx) < 0)
    {
        std::cerr << "Failed to initialize the resampling context" << std::endl;
        swr_free(&global_swr_ctx);
        av_channel_layout_uninit(&out_ch_layout);
        av_channel_layout_uninit(&audio_params.ch_layout);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_CloseFont(font);
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    // 设置SDL音频参数
    SDL_AudioSpec desired_spec, obtained_spec;
    desired_spec.freq = 48000;
    desired_spec.format = AUDIO_F32SYS;
    desired_spec.channels = 2;
    desired_spec.samples = 4096;
    desired_spec.callback = audio_callback;
    desired_spec.userdata = nullptr;

    // 打开音频设备
    if (SDL_OpenAudio(&desired_spec, &obtained_spec) < 0)
    {
        std::cerr << "Could not open audio device: " << SDL_GetError() << std::endl;
        swr_free(&global_swr_ctx);
        av_channel_layout_uninit(&out_ch_layout);
        av_channel_layout_uninit(&audio_params.ch_layout);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_CloseFont(font);
        TTF_Quit();
        SDL_Quit();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return -1;
    }

    // 开始播放
    SDL_PauseAudio(0);

    // 创建解码线程
    std::thread decode_thread_obj(decode_thread, codec_ctx, format_ctx, audio_stream_index);

    // 主循环：GUI事件处理和渲染
    std::cout << "Playing: " << filename << std::endl;

    bool should_quit = false;
    while (!quit_flag.load() && !should_quit)
    {
        // 处理GUI事件
        should_quit = handleGUIEvents();

        // 渲染GUI
        renderGUI();

        // 检查是否播放完毕
        double current_time = audio_clock.load();
        if (duration_in_seconds > 0 && current_time >= duration_in_seconds && playback_state.load() == PlaybackState::PLAYING)
        {
            // 播放完毕，暂停播放
            pause_playback();
        }

        // 稍微延迟以减少CPU使用率
        SDL_Delay(50);
    }

    quit_flag.store(true);
    audio_queue_cv.notify_all(); // 唤醒可能在等待的线程

    // 等待解码线程结束
    decode_thread_obj.join();

    // 清理音频队列
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex);
        while (!audio_frame_queue.empty())
        {
            delete audio_frame_queue.front();
            audio_frame_queue.pop();
        }
    }

    // 清理资源
    swr_free(&global_swr_ctx);
    av_channel_layout_uninit(&out_ch_layout);
    av_channel_layout_uninit(&audio_params.ch_layout);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    SDL_CloseAudio();

    // 清理GUI资源
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();

    std::cout << "\nPlayback finished!" << std::endl;
    return 0;
}