#ifndef MUSICLISTMODEL_H
#define MUSICLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QUrl>

// 1. 定义数据结构
struct MusicItem {
    QString title;
    QString artist;
    QUrl imageSource;
    bool isPlaying = false;
};

class MusicListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit MusicListModel(QObject *parent = nullptr);

    // QAbstractItemModel 必需的接口
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // 关键：定义角色，让 QML 知道如何访问数据
    QHash<int, QByteArray> roleNames() const override;

    // 添加数据的方法
    void loadInitialData();
    void addMusicItem(const MusicItem &item);

private:
    QList<MusicItem> m_musicList;

    // 2. 定义自定义角色
    enum MusicRoles {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        ImageRole,
        PlayingRole
    };
};



#endif // MUSICLISTMODEL_H
