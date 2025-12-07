#include "ColorExtractor.hpp"
#include <QDebug>
#include <cmath>

// ---------------------------------------------------------------
// Weighted RGB distance
// ---------------------------------------------------------------
double ColorExtractor::colorDistance(const QColor &c1, const QColor &c2)
{
    long rMean = (c1.red() + c2.red()) / 2;
    long r = c1.red() - c2.red();
    long g = c1.green() - c2.green();
    long b = c1.blue() - c2.blue();

    return std::sqrt((((512 + rMean) * r * r) >> 8)
                     + 4 * g * g
                     + (((767 - rMean) * b * b) >> 8));
}

// ---------------------------------------------------------------
// Stylization step: darken, desaturate, gamma correct
// ---------------------------------------------------------------
QColor ColorExtractor::stylizeColor(const QColor &c)
{
    int h = c.hslHue();
    int s = c.hslSaturation();
    int l = c.lightness();

    // Reduce saturation
    s = int(s * 0.28); // keep only ~28% of original saturation

    // Cap brightness
    l = std::min(l, 120); // <= 120
    l = int(l * 0.80);    // darker again

    QColor out = QColor::fromHsl(h, s, l);

    // Additional darkening
    out = out.darker(140);

    // Final gamma
    out = applyGamma(out, 0.55);

    return out;
}

QColor ColorExtractor::applyGamma(const QColor &c, double gamma)
{
    auto g = [&](int v)
    {
        return int(255 * std::pow(v / 255.0, gamma));
    };
    return QColor(g(c.red()), g(c.green()), g(c.blue()));
}

// ---------------------------------------------------------------
// Process K-Means clusters → pick 3 colors → stylize them
// ---------------------------------------------------------------
QList<QColor> ColorExtractor::processClusters(QVector<ColorCluster> &clusters)
{
    QList<QColor> result;
    QList<QColor> candidates;

    for (auto &c : clusters)
        if (c.pixelCount > 0)
            candidates.append(c.toColor());

    if (candidates.isEmpty())
        return {QColor(40, 40, 40), QColor(25, 25, 25), QColor(10, 10, 10)};

    // Sort by pixel size (dominance)
    std::sort(candidates.begin(), candidates.end(),
              [&](const QColor &a, const QColor &b)
              {
                  return a.lightness() < b.lightness();
              });

    result.append(candidates[0]); // main color

    for (int i = 1; i < candidates.size() && result.size() < 3; ++i)
    {
        double dist = colorDistance(candidates[i], result.back());
        if (dist > 40 && dist < 450)
            result.append(candidates[i]);
    }

    while (result.size() < 3)
        result.append(result.back().darker(130));

    // Apply global stylization
    for (QColor &c : result)
        c = stylizeColor(c);

    // Sort from lightest → darkest for gradient
    std::sort(result.begin(), result.end(),
              [](const QColor &a, const QColor &b)
              {
                  return a.lightness() > b.lightness();
              });

    return result;
}

// ---------------------------------------------------------------
// Load from CoverImage
// ---------------------------------------------------------------
QList<QColor> ColorExtractor::getAdaptiveGradientColors(const std::shared_ptr<CoverImage> &coverImg)
{
    if (!coverImg || !coverImg->isValid())
        return {QColor(40, 40, 40), QColor(25, 25, 25), QColor(10, 10, 10)};

    const int w = coverImg->width();
    const int h = coverImg->height();
    const int ch = coverImg->channels();
    const uint8_t *data = coverImg->data();

    const int K = 5;
    QVector<QColor> centers(K);
    QVector<ColorCluster> clusters(K);

    int stride = 5; // fast sampling

    for (int i = 0; i < K; ++i)
    {
        int x = (w * i) / K;
        int y = (h * i) / K;
        int idx = (y * w + x) * ch;
        centers[i] = QColor(data[idx], data[idx + 1], data[idx + 2]);
    }

    const int iterations = 3;

    for (int it = 0; it < iterations; ++it)
    {
        for (int k = 0; k < K; ++k)
            clusters[k] = ColorCluster();

        for (int y = 0; y < h; y += stride)
        {
            int base = y * w * ch;
            for (int x = 0; x < w; x += stride)
            {
                int idx = base + x * ch;

                QColor p(data[idx], data[idx + 1], data[idx + 2]);

                int best = 0;
                double bestDist = 1e15;
                for (int k = 0; k < K; ++k)
                {
                    double d = colorDistance(p, centers[k]);
                    if (d < bestDist)
                    {
                        bestDist = d;
                        best = k;
                    }
                }

                clusters[best].r += p.red();
                clusters[best].g += p.green();
                clusters[best].b += p.blue();
                clusters[best].pixelCount++;
            }
        }

        for (int k = 0; k < K; ++k)
            centers[k] = clusters[k].toColor();
    }

    return processClusters(clusters);
}

// ---------------------------------------------------------------
// Load from file path (fallback)
// ---------------------------------------------------------------
QList<QColor> ColorExtractor::getAdaptiveGradientColors(const QString &imagePath)
{
    QImage img(imagePath);
    if (img.isNull())
        return {QColor(40, 40, 40), QColor(25, 25, 25), QColor(10, 10, 10)};

    QImage s = img.scaled(64, 64, Qt::IgnoreAspectRatio)
                   .convertToFormat(QImage::Format_RGB888);

    const int K = 5;
    QVector<QColor> centers(K);
    QVector<ColorCluster> clusters(K);

    int step = s.width() / K;
    for (int i = 0; i < K; ++i)
        centers[i] = s.pixelColor(i * step, i * step);

    const uchar *bits = s.bits();
    int total = s.sizeInBytes();

    for (int it = 0; it < 3; ++it)
    {
        for (int k = 0; k < K; ++k)
            clusters[k] = ColorCluster();

        for (int i = 0; i < total; i += 3)
        {
            QColor p(bits[i], bits[i + 1], bits[i + 2]);

            int best = 0;
            double bestDist = 1e15;
            for (int k = 0; k < K; ++k)
            {
                double d = colorDistance(p, centers[k]);
                if (d < bestDist)
                {
                    best = k;
                    bestDist = d;
                }
            }

            clusters[best].r += p.red();
            clusters[best].g += p.green();
            clusters[best].b += p.blue();
            clusters[best].pixelCount++;
        }

        for (int k = 0; k < K; ++k)
            centers[k] = clusters[k].toColor();
    }

    return processClusters(clusters);
}
