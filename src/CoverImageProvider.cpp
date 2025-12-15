#include "CoverImageProvider.hpp"

QImage CoverImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    // 1. 根据 QML 传入的 ID (专辑名) 从您的缓存中获取图片指针
    std::string albumName = QUrl::fromPercentEncoding(id.toUtf8()).toStdString();
    std::shared_ptr<CoverImage> imgPtr = CoverCache::instance().get(albumName);

    // 2. 检查图片是否有效
    if (!imgPtr || !imgPtr->isValid())
    {
        qWarning() << "CoverCache: Could not find valid image for album:" << id;
        return QImage(); // 返回空图片
    }

    // 3. 确定 QImage 格式 (根据 CoverImage 的 channels 通道数)
    QImage::Format format = QImage::Format_Invalid;
    int channels = imgPtr->channels(); //

    switch (channels)
    {
    case 1:
        format = QImage::Format_Grayscale8;
        break;
    case 3:
        // 您的 CoverCache 使用 stbir_resize_uint8_srgb，通常是 RGB 字节顺序
        format = QImage::Format_RGB888;
        break;
    case 4:
        format = QImage::Format_RGBA8888;
        break;
    default:
        qWarning() << "CoverCache: Unsupported channel count:" << channels;
        return QImage();
    }

    // 4. 创建 QImage
    // 注意：QImage(data, ...) 构造函数默认不复制数据，且不接管内存所有权。
    // 为了安全，我们必须确保返回的 QImage 拥有自己的数据副本。

    // 使用 QImage::QImage(const uchar *data, ...) 临时构造一个 QImage
    QImage tempCover(
        imgPtr->data(),   // 像素数据指针
        imgPtr->width(),  // 宽度
        imgPtr->height(), // 高度
        format);

    // 5. 告知 QML 图片尺寸
    if (size)
    {
        *size = QSize(imgPtr->width(), imgPtr->height());
    }

    // 6. 返回一个 QImage，使用的是内存里面的Image，与内存内的同生命周期
    return tempCover;
}
