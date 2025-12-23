#ifndef MUSICLISTMODEL_H
#define MUSICLISTMODEL_H

#include "PCH.h"

class PlaylistNode;

struct MusicItem
{
    int id; // 唯一ID (对应 vector index)
    QString title;
    QString artist;
    QString album;
    QString imageSource;
    QString extraInfo;
    QString parentDirName;
    bool isPlaying = false;
    bool isFolder = false;

    // 原始节点指针
    PlaylistNode *nodePtr = nullptr;
};

class MusicListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString currentDirName READ currentDirName NOTIFY currentDirNameChanged FINAL)
    Q_PROPERTY(int sortType READ sortType NOTIFY sortTypeChanged FINAL)
    Q_PROPERTY(bool sortReverse READ sortReverse NOTIFY sortReverseChanged FINAL)
    Q_PROPERTY(bool isAdding READ isAdding NOTIFY isAddingChanged FINAL)

public:
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

    // QAbstractItemModel interface
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
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

    // --- UI 调用接口 ---
    Q_INVOKABLE void loadRoot();
    Q_INVOKABLE void handleClick(int index);
    Q_INVOKABLE void refreshPlayingState();
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void setSortMode(int type, bool reverse);
    Q_INVOKABLE void locateCurrentPlaying();
    Q_INVOKABLE void ListViewAddNewFolder(const QString &path);
    Q_INVOKABLE void ListViewAddNewFile(const QString &path);
    Q_INVOKABLE void cancelAdding(); // 用于取消/重置状态

    // [新增] 获取指定索引的详细信息 (用于右键菜单)
    Q_INVOKABLE QVariantMap getDetailInfo(int index);

    // [新增] 删除指定索引的项目
    Q_INVOKABLE void deleteItem(int index, bool deletePhysicalFile);

    void setCurrentDirectoryNode(PlaylistNode *node);
    bool isAdding() const
    {
        return m_isAdding;
    }

signals:
    void currentDirNameChanged();
    void sortTypeChanged();
    void sortReverseChanged();
    void requestScrollTo(int index);
    void isAddingChanged();

private:
    bool m_isAdding = false;
    QFutureWatcher<bool> m_addWatcher; // 用于监视异步添加的结果
    bool internalAddPath(const std::string &path, bool isFolder);
    void repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes);
    MusicItem createItemFromNode(PlaylistNode *node, int id);
    void applySort();
    void performSort(bool syncBackend);
    bool lessThan(PlaylistNode *nodeA, PlaylistNode *nodeB) const;
    QString formatDuration(int64_t microsec);
    QString formatSongInfo(PlaylistNode *node);
    QString formatFolderInfo(PlaylistNode *node);

    QList<MusicItem> m_displayList; // View 显示的数据
    QList<MusicItem> m_fullList;    // 当前目录的全量备份 (用于取消搜索时恢复)

    PlaylistNode *m_currentDirectoryNode = nullptr;
    int m_sortType = SortByTitle;
    bool m_sortReverse = false;
    bool m_isSearching = false;
    QString m_currentDirName;

    enum MusicRoles
    {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        AlbumRole,
        ExtraInfoRole,
        ParentDirRole,
        ImageRole,
        PlayingRole,
        IsFolderRole
    };
};

#endif // MUSICLISTMODEL_H