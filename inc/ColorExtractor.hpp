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

#include "CoverImage.hpp"

struct ColorCluster
{
    long long r = 0;
    long long g = 0;
    long long b = 0;
    int pixelCount = 0;

    QColor toColor() const
    {
        if (pixelCount == 0)
            return QColor(0, 0, 0);
        return QColor(
            std::clamp<int>(r / pixelCount, 0, 255),
            std::clamp<int>(g / pixelCount, 0, 255),
            std::clamp<int>(b / pixelCount, 0, 255));
    }
};

class ColorExtractor
{
public:
    static QList<QColor> getAdaptiveGradientColors(const QString &imagePath);
    static QList<QColor> getAdaptiveGradientColors(const std::shared_ptr<CoverImage> &coverImg);

private:
    static QList<QColor> processClusters(QVector<ColorCluster> &clusters);

    static QColor stylizeColor(const QColor &c);
    static QColor applyGamma(const QColor &c, double gamma);
    static double colorDistance(const QColor &c1, const QColor &c2);
};

#endif // COLOREXTRACTOR_H
