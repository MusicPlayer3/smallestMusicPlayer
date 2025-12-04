#ifndef COLOREXTRACTOR_H
#define COLOREXTRACTOR_H

#include <QString>
#include <QList>
#include <QColor>
#include <QImage>
#include <QVector>
#include <cmath>
#include <algorithm>
#include <memory>

// 引入你的 CoverImage 类定义
#include "CoverImage.hpp"

// 内部辅助结构
struct ColorCluster
{
    long long r = 0;
    long long g = 0;
    long long b = 0;
    int pixelCount = 0;

    QColor toColor() const
    {
        if (pixelCount == 0)
            return Qt::black;
        // 限制范围防止溢出（虽然逻辑上不会）
        int red = std::clamp(static_cast<int>(r / pixelCount), 0, 255);
        int green = std::clamp(static_cast<int>(g / pixelCount), 0, 255);
        int blue = std::clamp(static_cast<int>(b / pixelCount), 0, 255);
        return QColor(red, green, blue);
    }
};

class ColorExtractor
{
public:
    /**
     * @brief [原有的] 从文件路径提取颜色
     */
    static QList<QColor> getAdaptiveGradientColors(const QString &imagePath);

    /**
     * @brief [新增的] 从内存中的 CoverImage 对象提取颜色
     * @param coverImg 指向 CoverImage 的智能指针
     * @return 包含三个 QColor 的列表。如果输入无效返回默认深色组。
     */
    static QList<QColor> getAdaptiveGradientColors(const std::shared_ptr<CoverImage> &coverImg);

private:
    // 通用逻辑：处理聚类结果并生成最终的三色渐变
    static QList<QColor> processClusters(QVector<ColorCluster> &clusters);

    // 计算两个颜色的欧氏距离 (加权RGB)
    static double colorDistance(const QColor &c1, const QColor &c2);

    friend void runColorExtractorTest();
};

#endif // COLOREXTRACTOR_H