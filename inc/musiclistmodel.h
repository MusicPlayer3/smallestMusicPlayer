#ifndef MUSICLISTMODEL_H
#define MUSICLISTMODEL_H

#include "PCH.h"

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

    // [新增] 扩展信息用于显示
    QString album;
    QString extraInfo;     // 歌曲：时长|格式|采样率... | 文件夹：歌曲数量|时长
    QString parentDirName; // 文件夹显示上级目录名

    // 保存原始 Node 指针，用于搜索匹配和后续操作
    PlaylistNode *nodePtr = nullptr;
};

class MusicListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString currentDirName READ currentDirName NOTIFY currentDirNameChanged FINAL)
    // [新增] 暴露排序状态给 UI
    Q_PROPERTY(int sortType READ sortType NOTIFY sortTypeChanged FINAL)
    Q_PROPERTY(bool sortReverse READ sortReverse NOTIFY sortReverseChanged FINAL)

public:
    // [新增] 排序枚举
    enum SortType
    {
        SortByTitle = 0,
        SortByFilename,
        SortByPath,
        SortByArtist,
        SortByAlbum,
        SortByYear,
        SortByDuration,
        SortByDate
    };
    Q_ENUM(SortType)

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

    int sortType() const
    {
        return m_sortType;
    }

    bool sortReverse() const
    {
        return m_sortReverse;
    }

    // 1. 加载根目录
    Q_INVOKABLE void loadRoot();

    // 2. 处理点击事件
    Q_INVOKABLE void handleClick(int index);

    // 3. 刷新播放状态
    Q_INVOKABLE void refreshPlayingState();

    // 4. 返回上一级
    Q_INVOKABLE void goBack();

    // 5. 搜索接口
    Q_INVOKABLE void search(const QString &query);

    // 6. [新增] 排序接口
    Q_INVOKABLE void setSortMode(int type, bool reverse);

    // 7. [新增] 定位当前播放歌曲
    Q_INVOKABLE void locateCurrentPlaying();

    // 用于在 C++ 内部设置新目录和发出信号
    void setCurrentDirectoryNode(PlaylistNode *node);

signals:
    void currentDirNameChanged();
    void sortTypeChanged();
    void sortReverseChanged();
    // [新增] 请求 UI 滚动到指定位置
    void requestScrollTo(int index);

private:
    // 内部函数：根据传入的 Node 列表重置模型数据
    void repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes);

    // 辅助函数：从 Node 创建 MusicItem
    MusicItem createItemFromNode(PlaylistNode *node, int id);

    // 递归搜索辅助函数
    void recursiveSearch(PlaylistNode *node, const QString &query, QList<MusicItem> &results, int &idCounter);

    // [修改] 应用排序 (对外接口，发送信号)
    void applySort();

    // [新增] 执行排序逻辑 (内部函数，不发送信号)
    void performSort(bool syncBackend);

    // [新增] 比较逻辑提取
    bool lessThan(PlaylistNode *nodeA, PlaylistNode *nodeB) const;

    // [新增] 格式化辅助函数
    QString formatDuration(int64_t microsec);
    QString formatSongInfo(PlaylistNode *node);
    QString formatFolderInfo(PlaylistNode *node);

    // m_displayList 是当前显示给 View 的数据
    QList<MusicItem> m_displayList;

    // m_fullList 用于备份当前目录的完整数据，以便搜索框清空时恢复
    QList<MusicItem> m_fullList;

    // 建立 ID 到 PlaylistNode 指针的映射
    QMap<int, PlaylistNode *> m_nodeMap;

    // 记录当前所在的目录节点
    PlaylistNode *m_currentDirectoryNode = nullptr;

    // 排序状态
    int m_sortType = SortByTitle;
    bool m_sortReverse = false;

    // [新增] 标记当前是否处于搜索模式
    bool m_isSearching = false;

    enum MusicRoles
    {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        AlbumRole,     // [新增]
        ExtraInfoRole, // [新增]
        ParentDirRole, // [新增]
        ImageRole,
        PlayingRole,
        IsFolderRole
    };

    QString m_currentDirName;
};
#endif // MUSICLISTMODEL_H