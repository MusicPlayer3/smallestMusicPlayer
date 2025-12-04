#include "ColorExtractor.hpp"
#include <QDebug>
#include <filesystem>
#include "CoverCache.hpp"

// --- 辅助函数：颜色距离 ---
double ColorExtractor::colorDistance(const QColor &c1, const QColor &c2)
{
    long rMean = (c1.red() + c2.red()) / 2;
    long r = c1.red() - c2.red();
    long g = c1.green() - c2.green();
    long b = c1.blue() - c2.blue();

    // Weighted Euclidean distance
    return std::sqrt((((512 + rMean) * r * r) >> 8) + 4 * g * g + (((767 - rMean) * b * b) >> 8));
}

// --- 核心逻辑：从聚类簇中筛选出3个合适的颜色 ---
QList<QColor> ColorExtractor::processClusters(QVector<ColorCluster> &clusters)
{
    // 1. 按像素数量排序
    std::sort(clusters.begin(), clusters.end(), [](const ColorCluster &a, const ColorCluster &b)
              { return a.pixelCount > b.pixelCount; });

    QList<QColor> result;

    // 提取有像素的候选色
    QList<QColor> candidates;
    for (const auto &cluster : clusters)
    {
        if (cluster.pixelCount > 0)
        {
            candidates.append(cluster.toColor());
        }
    }

    // 默认备选方案（深灰背景）
    if (candidates.isEmpty())
    {
        return {QColor(50, 50, 50), QColor(30, 30, 30), QColor(10, 10, 10)};
    }

    // =========================================================
    // [新增] 亮度预处理 helper lambda
    // 确保任何进入候选列表的颜色都不会太亮
    // 阈值设定：140 (范围 0-255)。超过这个值会影响白色文字的阅读。
    // =========================================================
    auto ensureDarkness = [](QColor col) -> QColor
    {
        const int MAX_LIGHTNESS = 140;
        if (col.lightness() > MAX_LIGHTNESS)
        {
            // 保持色相和饱和度，只降低亮度
            // 注意：QColor 的 HSL 设置函数参数通常是 Hue(0-359), Sat(0-255), Light(0-255)
            // 这里我们强制将亮度压到 MAX_LIGHTNESS
            col.setHsl(col.hslHue(), col.hslSaturation(), MAX_LIGHTNESS);
        }
        return col;
    };

    // 2. 筛选颜色 (距离控制)
    // 先添加主色，并确保主色不刺眼
    QColor mainColor = ensureDarkness(candidates[0]);
    result.append(mainColor);

    const double maxDist = 450.0;
    const double minDist = 40.0;

    for (int i = 1; i < candidates.size(); ++i)
    {
        if (result.size() >= 3)
            break;

        // 处理候选色
        QColor current = ensureDarkness(candidates[i]);

        bool isOk = true;
        for (const QColor &selected : result)
        {
            double dist = colorDistance(current, selected);
            if (dist < minDist || dist > maxDist)
            {
                isOk = false;
                break;
            }
        }
        if (isOk)
            result.append(current);
    }

    // 3. 补全不足的颜色
    // 注意：补全时生成的变体也必须经过亮度检查
    while (result.size() < 3)
    {
        QColor base = result[0];
        QColor newColor;

        if (result.size() == 1)
        {
            // 生成深色背景 (Darker 本身会让颜色变暗，通常是安全的，但为了保险依然可以检查)
            // darker(150) 意味着亮度变为原来的 1/1.5 ≈ 66%
            newColor = base.darker(150);
        }
        else
        {
            // 尝试生成亮一点的变体，但不能超过我们的阈值
            // 如果 base 已经很亮（接近140），lighter 可能会导致过曝
            // 所以这里我们手动调整 HSL 稍微改变一点亮度，而不是盲目 lighter
            int h = base.hslHue();
            int s = base.hslSaturation();
            int l = base.lightness();

            // 稍微调亮一点点，或者调暗一点点作为第三色
            // 为了安全，我们倾向于让第三个颜色比主色更暗，营造深邃感
            int newL = std::max(10, l - 30); // 变暗
            newColor = QColor::fromHsl(h, s, newL);
        }

        // 双重保险：再次确保不超过最大亮度
        result.append(ensureDarkness(newColor));
    }

    // 4. 按亮度排序 (UI 关键)
    // 从亮到暗排序 (lightness 大 -> 小)
    // 这样 UI 顶部稍微亮一点（模拟光源），底部暗一点（视觉重心）
    std::sort(result.begin(), result.end(), [](const QColor &a, const QColor &b)
              { return a.lightness() > b.lightness(); });

    return result;
}

// ==========================================
// [新增实现] 从 CoverImage 提取
// ==========================================
QList<QColor> ColorExtractor::getAdaptiveGradientColors(const std::shared_ptr<CoverImage> &coverImg)
{
    // 1. 安全检查
    if (!coverImg || !coverImg->isValid())
    {
        return {QColor(60, 60, 60), QColor(40, 40, 40), QColor(20, 20, 20)};
    }

    const int width = coverImg->width();
    const int height = coverImg->height();
    const int channels = coverImg->channels(); // 期望是 4 (RGBA)
    const uint8_t *data = coverImg->data();

    // 2. 设置聚类参数
    const int K = 5;
    QVector<QColor> centers(K);
    QVector<ColorCluster> clusters(K);

    // 3. 初始化中心点
    // 由于我们直接读取内存，不需要缩放图片，但我们可以使用 stride (步长) 来模拟降采样
    // 256x256 = 65k 像素，我们不需要全读，每隔 5 个像素读一个就足够精确了
    int stride = 5;

    // 简单的对角线初始化
    for (int i = 0; i < K; ++i)
    {
        int x = (width * i) / K;
        int y = (height * i) / K;
        int idx = (y * width + x) * channels;
        centers[i] = QColor(data[idx], data[idx + 1], data[idx + 2]);
    }

    // 4. K-Means 迭代
    const int iterations = 3;

    for (int it = 0; it < iterations; ++it)
    {
        // 重置
        for (int k = 0; k < K; ++k)
            clusters[k] = ColorCluster();

        // 遍历像素 (跳跃式遍历，性能极快)
        for (int y = 0; y < height; y += stride)
        {
            const int rowStart = y * width * channels;
            for (int x = 0; x < width; x += stride)
            {
                int idx = rowStart + (x * channels);

                int r = data[idx];
                int g = data[idx + 1];
                int b = data[idx + 2];
                // 忽略 Alpha (data[idx+3])，除非你需要处理透明背景

                QColor pixelColor(r, g, b);

                // 找最近中心
                int nearestIndex = 0;
                double minDist = 999999.0;
                for (int k = 0; k < K; ++k)
                {
                    double dist = colorDistance(pixelColor, centers[k]);
                    if (dist < minDist)
                    {
                        minDist = dist;
                        nearestIndex = k;
                    }
                }

                // 累加
                clusters[nearestIndex].r += r;
                clusters[nearestIndex].g += g;
                clusters[nearestIndex].b += b;
                clusters[nearestIndex].pixelCount++;
            }
        }

        // 更新中心
        for (int k = 0; k < K; ++k)
        {
            centers[k] = clusters[k].toColor();
        }
    }

    // 5. 调用通用处理逻辑
    return processClusters(clusters);
}

// ==========================================
// [原有实现] 从文件路径提取 (保留以便兼容)
// ==========================================
QList<QColor> ColorExtractor::getAdaptiveGradientColors(const QString &imagePath)
{
    QImage img(imagePath);
    if (img.isNull())
    {
        return {QColor(60, 60, 60), QColor(40, 40, 40), QColor(20, 20, 20)};
    }

    // 降采样
    QImage scaleImg = img.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_RGB888);

    const int K = 5;
    QVector<QColor> centers(K);
    QVector<ColorCluster> clusters(K);

    // 初始化
    int step = scaleImg.width() / K;
    for (int i = 0; i < K; ++i)
        centers[i] = scaleImg.pixelColor(i * step, i * step);

    const int iterations = 3;
    const uchar *bits = scaleImg.bits();
    int byteCount = scaleImg.sizeInBytes();

    // 这里的 bits 是 RGB888，步长为3
    for (int it = 0; it < iterations; ++it)
    {
        for (int k = 0; k < K; ++k)
            clusters[k] = ColorCluster();

        for (int i = 0; i < byteCount; i += 3)
        {
            int r = bits[i];
            int g = bits[i + 1];
            int b = bits[i + 2];
            QColor pixelColor(r, g, b);

            int nearestIndex = 0;
            double minDist = 999999.0;
            for (int k = 0; k < K; ++k)
            {
                double dist = colorDistance(pixelColor, centers[k]);
                if (dist < minDist)
                {
                    minDist = dist;
                    nearestIndex = k;
                }
            }
            clusters[nearestIndex].r += r;
            clusters[nearestIndex].g += g;
            clusters[nearestIndex].b += b;
            clusters[nearestIndex].pixelCount++;
        }
        for (int k = 0; k < K; ++k)
            centers[k] = clusters[k].toColor();
    }

    return processClusters(clusters);
}

void runColorExtractorTest()
{
    qDebug() << "=================in runColorExtractorTest====================";
    const QString inputDir = "/home/kaizen857/Music/CloudMusic(for MP4)/BlackYs BEATFLOOR & Luminaria - Idealize/Cover.jpg";
    auto res1 = ColorExtractor::getAdaptiveGradientColors(inputDir);

    qDebug() << "res1:" << res1;
    qDebug() << "res1 name:" << res1[0].name() << "\t" << res1[1].name() << "\t" << res1[2].name();
    const std::string alb = "Idealize";
    auto tmp = CoverCache::instance().get(alb);
    if (tmp != nullptr)
    {
        auto res2 = ColorExtractor::getAdaptiveGradientColors(CoverCache::instance().get(alb));
        qDebug() << "res2:" << res2;
        qDebug() << "res2 name:" << res2[0].name() << "\t" << res2[1].name() << "\t" << res2[2].name();
    }
}