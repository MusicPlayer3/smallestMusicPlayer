#ifndef MUSICLISTMODEL_H
#define MUSICLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QUrl>
#include <QMap>
#include <memory>
#include <vector>

class PlaylistNode;

// 1. 定义数据结构
struct MusicItem
{
    int id; // 唯一编号，对应 Map 的 Key
    QString title;
    QString artist;
    QString imageSource;
    bool isPlaying = false;
    bool isFolder = false;

    // 保存原始 Node 指针，用于搜索匹配和后续操作
    PlaylistNode *nodePtr = nullptr;
};

class MusicListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString currentDirName READ currentDirName NOTIFY currentDirNameChanged FINAL)
public:
    explicit MusicListModel(QObject *parent = nullptr);

    // QAbstractItemModel 必需的接口
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // 关键：定义角色，让 QML 知道如何访问数据
    QHash<int, QByteArray> roleNames() const override;

    QString currentDirName() const
    {
        return m_currentDirName;
    }

    // 1. 加载根目录
    Q_INVOKABLE void loadRoot();

    // 2. 处理点击事件
    Q_INVOKABLE void handleClick(int index);

    // 3. 刷新播放状态
    Q_INVOKABLE void refreshPlayingState();

    // 4. 返回上一级
    Q_INVOKABLE void goBack();

    // 5. [新增] 搜索接口
    Q_INVOKABLE void search(const QString &query);

    // 用于在 C++ 内部设置新目录和发出信号
    void setCurrentDirectoryNode(PlaylistNode *node);

signals:
    void currentDirNameChanged();

private:
    // 内部函数：根据传入的 Node 列表重置模型数据
    void repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes);

    // [新增] 辅助函数：从 Node 创建 MusicItem
    MusicItem createItemFromNode(PlaylistNode *node, int id);

    // [新增] 递归搜索辅助函数
    void recursiveSearch(PlaylistNode *node, const QString &query, QList<MusicItem> &results, int &idCounter);

    // m_displayList 是当前显示给 View 的数据（可能是全量，也可能是搜索结果）
    QList<MusicItem> m_displayList;

    // [新增] m_fullList 用于备份当前目录的完整数据，以便搜索框清空时恢复
    QList<MusicItem> m_fullList;

    // 建立 ID 到 PlaylistNode 指针的映射
    QMap<int, PlaylistNode *> m_nodeMap;

    // 记录当前所在的目录节点
    PlaylistNode *m_currentDirectoryNode = nullptr;

    enum MusicRoles
    {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        ImageRole,
        PlayingRole,
        IsFolderRole
    };

    QString m_currentDirName;
};
#endif // MUSICLISTMODEL_H