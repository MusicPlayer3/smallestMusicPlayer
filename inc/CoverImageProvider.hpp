#ifndef COVERIMAGEPROVIDER_H
#define COVERIMAGEPROVIDER_H

#include "CoverCache.hpp" // 包含您的缓存头文件
#include "PCH.h"

class CoverImageProvider : public QQuickImageProvider
{
public:
    // 告知 QML 引擎：我们提供的是 QImage 格式的数据
    CoverImageProvider() : QQuickImageProvider(QQuickImageProvider::Image)
    {
    }

    // Tipssssssssssssssss：当你想要使用我们的Image服务的时候，
    // 请记住这个格式哟“image://[provider_id]/[image_id]"
    // [provider_id] -> covercache   [image_id] -> 你的专辑名称哦
    // EG:image://covercache/Thriller 或者 source: "image://covercache/" + currentAlbumName/currentTitle(这个是你外部修改的专辑名称)

    /**
     * @brief 响应 QML 的图片请求
     * @param id 传入的 Image ID (即专辑名)
     * @param size 用于返回图片的实际尺寸
     * @param requestedSize QML 请求的尺寸 (在此忽略，直接返回原图)
     * @return 内存中的 QImage 对象
     */
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

#endif // COVERIMAGEPROVIDER_H
