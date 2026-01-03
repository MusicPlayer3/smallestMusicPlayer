#ifndef AUDIO_FILTER_CHAIN_HPP
#define AUDIO_FILTER_CHAIN_HPP

#include "PCH.h"
#include "AudioParams.hpp"
#include <mutex>
#include <string>

/**
 * @class AudioFilterChain
 * @brief 音频滤镜链管理器 (FFmpeg Filter Graph Wrapper)
 *
 * @details
 * 负责管理 FFmpeg 的 AVFilterGraph，实现音频格式转换（重采样）、混音、音量调节以及自定义滤镜效果。
 *
 * 工作流程：
 * Source (abuffer) -> [Custom Filters] -> Volume -> Format Converter -> Sink (abuffersink)
 */
class AudioFilterChain
{
public:
    AudioFilterChain();
    ~AudioFilterChain();

    /**
     * @brief 初始化或重新配置滤镜链
     *
     * @details
     * 该函数具有智能检测机制：
     * - 如果传入的参数与当前已初始化的参数一致，则直接复用现有 Graph（零开销）。
     * - 如果参数发生变更，会自动销毁旧的 Graph 并利用新参数重建。
     *
     * @param input          输入音频参数（通常来自解码器上下文）。
     * @param output         期望输出参数（通常来自 SDL/Miniaudio/ALSA 等音频后端设置）。
     * @param custom_filters (可选) 自定义滤镜描述符字符串。
     *                       例如: "equalizer=f=1000:t=q:w=1:g=2" (1kHz 增益 2dB)。
     *                       默认为空。
     * @return 0 表示成功，负值表示 FFmpeg 错误码 (AVERROR)。
     */
    int init(const AudioParams &input, const AudioParams &output, const std::string &custom_filters = "");

    /**
     * @brief 强制清空缓冲区 (Flush)
     *
     * @details
     * 用于 Seek（定位）操作或播放停止时。
     * 为了彻底清除重采样器内部的缓冲数据和滤镜延迟线，该函数会销毁并重建整个滤镜图。
     * 这比发送 flush packet 更可靠，能有效避免 Seek 后听到残留的杂音。
     *
     * @return 0 表示成功，<0 表示失败或未初始化。
     */
    int flush();

    /**
     * @brief 向滤镜链发送一帧原始数据
     *
     * @param frame 解码后的原始 AVFrame。
     *              注意：调用成功后，frame 的引用权将被滤镜图接管（Move Semantics）。
     *              调用者不应再访问该 frame 的数据，或者需要在传入前调用 av_frame_clone。
     * @return >=0 成功，<0 错误码。
     */
    int push_frame(AVFrame *frame);

    /**
     * @brief 从滤镜链获取处理后的一帧数据
     *
     * @param[out] frame 用于接收数据的帧。调用前必须已分配 (av_frame_alloc)。
     *                   如果成功，frame 内部将填充数据，使用完毕后需调用 av_frame_unref。
     * @return
     *  - 0: 成功获取一帧。
     *  - AVERROR(EAGAIN): 内部数据不足，需要更多 push_frame。
     *  - AVERROR_EOF: 流结束。
     *  - <0: 其他错误。
     */
    int pop_frame(AVFrame *frame);

    /**
     * @brief 设置音量
     *
     * @details
     * 线程安全。通过 FFmpeg 命令机制实时调整 volume 滤镜的增益参数。
     * 无需重建滤镜图，即时生效。
     *
     * @param volume 线性音量值。
     *               0.0 = 静音
     *               1.0 = 原始音量
     */
    void set_volume(double volume);

    /**
     * @brief 获取当前滤镜链输出端的时间基
     * @return 输出时间基 (如果未初始化则返回 0/0)
     */
    AVRational get_output_time_base();

    const AudioParams &get_input_params() const
    {
        return current_input;
    }
    const AudioParams &get_output_params() const
    {
        return current_output;
    }

private:
    /**
     * @brief 内部核心：构建滤镜图
     * @note  调用此函数前必须持有 mutex 锁。
     */
    int rebuild_graph(const AudioParams &in, const AudioParams &out, const std::string &filters);

    /**
     * @brief 内部核心：销毁滤镜图
     */
    void destroy_graph();

    /**
     * @brief 辅助函数：将当前音量应用到滤镜实例
     */
    void apply_volume_command(double volume);

    // --- FFmpeg Contexts ---
    AVFilterGraph *filter_graph = nullptr;     ///< 滤镜图总管
    AVFilterContext *buffersrc_ctx = nullptr;  ///< 输入端 (Source)
    AVFilterContext *buffersink_ctx = nullptr; ///< 输出端 (Sink)

    // --- State Cache ---
    AudioParams current_input;       ///< 当前生效的输入参数
    AudioParams current_output;      ///< 当前生效的输出参数
    std::string current_filters_str; ///< 当前生效的自定义滤镜串

    double current_volume = 1.0; ///< 缓存当前音量
    bool is_initialized = false; ///< 初始化标志位

    // --- Constants ---
    // 给内部音量滤镜指定一个唯一的实例名，避免查找时混淆
    const char *MAIN_VOLUME_FILTER_NAME = "main_vol";

    // --- Thread Safety ---
    std::mutex filter_mutex;
};

#endif // AUDIO_FILTER_CHAIN_HPP