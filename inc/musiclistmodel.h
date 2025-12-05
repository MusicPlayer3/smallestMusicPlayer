#ifndef MUSICLISTMODEL_H
#define MUSICLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QUrl>
#include <QMap>
#include <memory>

class PlaylistNode;

// 1. 定义数据结构
struct MusicItem
{
    int id; // [新增] 唯一编号，对应 Map 的 Key
    QString title;
    QString artist;
    QString imageSource; // 改为 QString 方便拼接 image://
    bool isPlaying = false;
    bool isFolder = false; // [新增] 是否为文件夹
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
    // void loadInitialData();
    // void addMusicItem(const MusicItem &item);

    // 1. 加载根目录 (扫描完成后调用)
    Q_INVOKABLE void loadRoot();

    // 2. 处理点击事件 (前端传入 index/id)
    Q_INVOKABLE void handleClick(int index);

    // 3. 刷新播放状态 (轮询或切歌时调用，更新高亮)
    Q_INVOKABLE void refreshPlayingState();

    // 4. 返回上一级 (可选，用于面包屑导航)
    Q_INVOKABLE void goBack();

private:
    // 内部函数：根据传入的 Node 列表重置模型数据
    void repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes);

    QList<MusicItem> m_musicList;

    // [关键] 建立 ID 到 PlaylistNode 指针的映射
    // Key: MusicItem.id (也就是 list 的 index)
    // Value: 对应的后端节点指针
    QMap<int, PlaylistNode *> m_nodeMap;

    // 记录当前所在的目录节点，用于“返回上一级”
    PlaylistNode *m_currentDirectoryNode = nullptr;

    // 2. 定义自定义角色
    enum MusicRoles
    {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        ImageRole,
        PlayingRole,
        IsFolderRole
    };
};
#endif // MUSICLISTMODEL_H