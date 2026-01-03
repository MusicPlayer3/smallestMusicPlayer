#ifndef _AUDIO_PARAMS_HPP_
#define _AUDIO_PARAMS_HPP_

#include "PCH.h"

/**
 * @brief 音频参数封装结构体 (RAII)
 * @note 针对 FFmpeg 8.0 设计。由于 AVChannelLayout 内部可能包含动态分配的内存
 *       (如自定义声道映射)，本结构体实现了深拷贝构造和赋值，防止内存泄漏或 Double Free。
 */
struct AudioParams
{
    int sample_rate = 0;                          ///< 采样率 (e.g., 44100, 48000)
    enum AVSampleFormat fmt = AV_SAMPLE_FMT_NONE; ///< 采样格式 (e.g., AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S16)
    AVChannelLayout ch_layout;                    ///< 声道布局
    AVRational time_base = {1, 1};                ///< 时间基 (Time Base)，默认为 1/1

    /**
     * @brief 默认构造函数
     */
    AudioParams()
    {
        av_channel_layout_default(&ch_layout, 2);
    }

    /**
     * @brief 析构函数
     * 自动释放 AVChannelLayout 中的动态内存。
     */
    ~AudioParams()
    {
        av_channel_layout_uninit(&ch_layout);
    }

    /**
     * @brief 拷贝构造函数 (深拷贝)
     * 确保 AVChannelLayout 被正确复制。
     */
    AudioParams(const AudioParams &other)
    {
        sample_rate = other.sample_rate;
        fmt = other.fmt;
        time_base = other.time_base;
        av_channel_layout_copy(&ch_layout, &other.ch_layout);
    }

    /**
     * @brief 赋值运算符 (深拷贝)
     */
    AudioParams &operator=(const AudioParams &other)
    {
        if (this != &other)
        {
            sample_rate = other.sample_rate;
            fmt = other.fmt;
            time_base = other.time_base;
            av_channel_layout_uninit(&ch_layout);
            av_channel_layout_copy(&ch_layout, &other.ch_layout);
        }
        return *this;
    }

    /**
     * @brief 相等性比较
     * 比较采样率、格式和声道布局是否完全一致。
     */
    bool operator==(const AudioParams &other) const
    {
        // 使用 av_cmp_q 比较分数
        return sample_rate == other.sample_rate && fmt == other.fmt && (av_cmp_q(time_base, other.time_base) == 0) && (av_channel_layout_compare(&ch_layout, &other.ch_layout) == 0);
    }

    /**
     * @brief 不等性比较
     */
    bool operator!=(const AudioParams &other) const
    {
        return !(*this == other);
    }
};

#endif