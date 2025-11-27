#ifndef _COVERIMAGE_HPP_
#define _COVERIMAGE_HPP_

#include "Precompiled.h"

class CoverImage
{
public:
    // ==========================================
    // 1. 构造与析构 (RAII: Resource Acquisition)
    // ==========================================

    // 默认构造函数（创建一个空对象）
    CoverImage() = default;

    /**
     * @brief 主构造函数
     * @param width 宽度
     * @param height 高度
     * @param channels 通道数
     * @param pixels 像素数据 (会被移动进类内)
     * @throw std::invalid_argument 如果像素数据大小与尺寸不匹配
     */
    CoverImage(int width, int height, int channels, std::vector<std::uint8_t> pixels) : m_width(width), m_height(height), m_channels(channels), m_pixels(std::move(pixels))
    {
        // 验证数据完整性 (Invariant check)
        // 只有当长宽都大于0时，才进行大小检查
        if (m_width > 0 && m_height > 0 && m_channels > 0)
        {
            size_t expectedSize = static_cast<size_t>(m_width) * m_height * m_channels;
            if (m_pixels.size() != expectedSize)
            {
                throw std::invalid_argument("CoverImage: Pixel data size does not match width * height * channels");
            }
        }
    }

    // 析构函数：std::vector 会自动释放内存
    ~CoverImage() = default;

    // ==========================================
    // 2. 禁止复制 (Disable Copy)
    // ==========================================
    CoverImage(const CoverImage &) = delete;
    CoverImage &operator=(const CoverImage &) = delete;

    // ==========================================
    // 3. 允许移动 (Enable Move)
    // ==========================================

    // 移动构造函数
    CoverImage(CoverImage &&other) noexcept
        : m_width(std::exchange(other.m_width, 0)), m_height(std::exchange(other.m_height, 0)), m_channels(std::exchange(other.m_channels, 0)), m_pixels(std::move(other.m_pixels))
    {
        // 这里的 std::exchange 将 other 的基础类型成员置为 0
        // std::move 将 other.m_pixels 内容转移过来，other.m_pixels 变为空
    }

    // 移动赋值运算符
    CoverImage &operator=(CoverImage &&other) noexcept
    {
        if (this != &other)
        {
            // 接管资源
            m_width = std::exchange(other.m_width, 0);
            m_height = std::exchange(other.m_height, 0);
            m_channels = std::exchange(other.m_channels, 0);
            m_pixels = std::move(other.m_pixels);
        }
        return *this;
    }

    // ==========================================
    // 4. 公共接口 (Accessors)
    // ==========================================
    int width() const
    {
        return m_width;
    }
    int height() const
    {
        return m_height;
    }
    int channels() const
    {
        return m_channels;
    }

    // 提供对像素数据的只读访问
    const std::vector<std::uint8_t> &pixels() const
    {
        return m_pixels;
    }

    // 如果需要修改数据，可以提供非 const 版本，或者通过 data() 指针访问
    const std::uint8_t *data() const
    {
        return m_pixels.data();
    }

    // 检查图片是否有效
    bool isValid() const
    {
        return !m_pixels.empty() && m_width > 0 && m_height > 0;
    }

private:
    int m_width = 0;
    int m_height = 0;
    int m_channels = 0;
    std::vector<std::uint8_t> m_pixels;
};

#endif