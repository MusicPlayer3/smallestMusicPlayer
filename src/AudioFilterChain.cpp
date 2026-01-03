#include "AudioFilterChain.hpp"
#include <sstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdio> // for snprintf

AudioFilterChain::AudioFilterChain()
{
}

AudioFilterChain::~AudioFilterChain()
{
    destroy_graph();
}

void AudioFilterChain::destroy_graph()
{
    if (filter_graph)
    {
        // avfilter_graph_free 会自动释放图内所有的 Context，
        // 所以 buffersrc_ctx 和 buffersink_ctx 不需要单独释放。
        avfilter_graph_free(&filter_graph);
        filter_graph = nullptr;
    }
    buffersrc_ctx = nullptr;
    buffersink_ctx = nullptr;
    is_initialized = false;
}

int AudioFilterChain::init(const AudioParams &in, const AudioParams &out, const std::string &custom_filters)
{
    std::lock_guard<std::mutex> lock(filter_mutex);

    // 1. 智能检测：如果参数完全一致，直接复用当前 Graph (Gapless)
    if (is_initialized && in == current_input && out == current_output && custom_filters == current_filters_str)
    {
        // 确保音量设置被应用（防止因外部状态不一致导致的音量跳变）
        if (filter_graph)
        {
            apply_volume_command(current_volume);
        }
        return 0;
    }

    // 2. 更新缓存参数
    current_input = in;
    current_output = out;
    current_filters_str = custom_filters;

    // 3. 重建滤镜图
    return rebuild_graph(in, out, custom_filters);
}

int AudioFilterChain::flush()
{
    std::lock_guard<std::mutex> lock(filter_mutex);
    if (!is_initialized)
        return -1;

    // Flush 的本质是重建图。
    // 虽然可以通过 av_buffersrc_add_frame(NULL) 来 flush，但在 Seek 操作中，
    // 直接重建图能更彻底地清除重采样器内部的 buffer 和时序信息。
    return rebuild_graph(current_input, current_output, current_filters_str);
}

int AudioFilterChain::rebuild_graph(const AudioParams &in, const AudioParams &out, const std::string &custom_filters)
{
    destroy_graph();

    int ret = 0;
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph)
        return AVERROR(ENOMEM);

    // 1. 创建 Source (abuffer)
    const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    char in_layout_buf[256] = {0};
    av_channel_layout_describe(&in.ch_layout, in_layout_buf, sizeof(in_layout_buf));

    std::stringstream args_ss;
    // 【核心修改】这里不再硬编码 1/sample_rate，而是使用 AudioParams 中真实的 time_base
    args_ss << "time_base=" << in.time_base.num << "/" << in.time_base.den
            << ":sample_rate=" << in.sample_rate
            << ":sample_fmt=" << av_get_sample_fmt_name(in.fmt)
            << ":channel_layout=" << in_layout_buf;

    ret = avfilter_graph_create_filter(&buffersrc_ctx, abuffer, "in", args_ss.str().c_str(), nullptr, filter_graph);
    if (ret < 0)
    {
        destroy_graph();
        return ret;
    }

    // 2. 创建 Sink (abuffersink)
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    ret = avfilter_graph_create_filter(&buffersink_ctx, abuffersink, "out", nullptr, nullptr, filter_graph);
    if (ret < 0)
    {
        destroy_graph();
        return ret;
    }

    // 3. 构建滤镜串 (保持不变)
    char out_layout_buf[256] = {0};
    av_channel_layout_describe(&out.ch_layout, out_layout_buf, sizeof(out_layout_buf));

    std::stringstream aformat_args;
    aformat_args << "sample_fmts=" << av_get_sample_fmt_name(out.fmt)
                 << ":sample_rates=" << out.sample_rate
                 << ":channel_layouts=" << out_layout_buf;

    std::string filters_descr = custom_filters;
    if (!filters_descr.empty())
        filters_descr += ",";
    filters_descr += "volume@" + std::string(MAIN_VOLUME_FILTER_NAME) + "=1.0,aformat=" + aformat_args.str();

    // 4. Parse & Config (保持不变)
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (!outputs || !inputs)
    {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        destroy_graph();
        return AVERROR(ENOMEM);
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    ret = avfilter_graph_parse_ptr(filter_graph, filters_descr.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0)
    {
        destroy_graph();
        return ret;
    }

    ret = avfilter_graph_config(filter_graph, nullptr);
    if (ret < 0)
    {
        destroy_graph();
        return ret;
    }

    is_initialized = true;
    apply_volume_command(current_volume);
    return 0;
}

AVRational AudioFilterChain::get_output_time_base()
{
    std::lock_guard<std::mutex> lock(filter_mutex);
    if (is_initialized && buffersink_ctx)
    {
        // 询问 buffersink 它的输入端 (sink 的 input pad) 的 time_base
        // 这就是滤镜图最终输出的时间基
        return av_buffersink_get_time_base(buffersink_ctx);
    }
    return {0, 0};
}

void AudioFilterChain::apply_volume_command(double volume)
{
    if (!filter_graph || !is_initialized)
        return;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "%.4f", volume);

    // 使用 avfilter_graph_get_filter 直接获取特定名称的滤镜上下文
    // 这比遍历所有滤镜并比对名称要高效且安全得多
    AVFilterContext *vol_ctx = avfilter_graph_get_filter(filter_graph, MAIN_VOLUME_FILTER_NAME);
    if (vol_ctx)
    {
        // 发送命令修改 volume 参数
        // arg3 "volume" 是滤镜内部的参数名
        // arg4 cmd 是参数值
        avfilter_graph_send_command(filter_graph, MAIN_VOLUME_FILTER_NAME, "volume", cmd, nullptr, 0, 0);
    }
}

int AudioFilterChain::push_frame(AVFrame *frame)
{
    std::lock_guard<std::mutex> lock(filter_mutex);
    if (!is_initialized || !buffersrc_ctx)
        return AVERROR(EINVAL);

    // av_buffersrc_add_frame_flags 默认行为是接管 frame 的引用 (Ref moved)
    // 成功调用后，frame->data 将变为空，调用者无需再 unref。
    return av_buffersrc_add_frame_flags(buffersrc_ctx, frame, 0);
}

int AudioFilterChain::pop_frame(AVFrame *frame)
{
    std::lock_guard<std::mutex> lock(filter_mutex);
    if (!is_initialized || !buffersink_ctx)
        return AVERROR(EINVAL);

    // 从 Sink 获取处理后的数据
    return av_buffersink_get_frame(buffersink_ctx, frame);
}

void AudioFilterChain::set_volume(double volume)
{
    std::lock_guard<std::mutex> lock(filter_mutex);
    current_volume = volume;

    if (is_initialized && filter_graph)
    {
        apply_volume_command(volume);
    }
}