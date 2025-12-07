#include "musiclistmodel.h"
#include "MediaController.hpp"
#include "PlaylistNode.hpp"
#include <QDebug>
#include <filesystem>
#include <qdebug.h>
#include <QFileInfo>

// 引入封面缓存测试 (如果不需要可以移除)
#include "CoverCache.hpp"
extern void run_cover_test();

MusicListModel::MusicListModel(QObject *parent) :
    QAbstractListModel(parent)
{
}

int MusicListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    // 使用 displayList 计数
    return m_displayList.count();
}

QVariant MusicListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_displayList.size())
        return QVariant();

    const MusicItem &item = m_displayList.at(index.row());

    switch (role)
    {
    case TitleRole:
        return item.title;
    case ArtistRole:
        return item.artist;
    case ImageRole:
        return item.imageSource;
    case PlayingRole:
        return item.isPlaying;
    case IsFolderRole:
        return item.isFolder;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> MusicListModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[TitleRole] = "title";
    roles[ArtistRole] = "artist";
    roles[ImageRole] = "imageSource";
    roles[PlayingRole] = "isPlaying";
    roles[IsFolderRole] = "isFolder";
    return roles;
}

// 辅助函数：提取创建 Item 的逻辑
MusicItem MusicListModel::createItemFromNode(PlaylistNode *node, int id)
{
    MusicItem item;
    item.id = id;
    item.nodePtr = node;

    if (node->isDir())
    {
        item.isFolder = true;
        item.artist = "";
        std::filesystem::path p(node->getPath());
        item.title = QString::fromStdString(p.filename().string());
        // 获取文件夹封面 Key
        QString coverPath = QString::fromStdString("image://covercache/" + node->getThisDirCover());
        item.imageSource = coverPath;
        item.isPlaying = false;
    }
    else
    {
        item.isFolder = false;
        const auto &meta = node->getMetaData();
        item.title = QString::fromStdString(meta.getTitle());
        // 如果 Title 为空，使用文件名
        if (item.title.isEmpty())
            item.title = QString::fromStdString(std::filesystem::path(node->getPath()).filename().string());

        item.artist = QString::fromStdString(meta.getArtist());

        std::string coverPath = meta.getAlbum();
        if (!coverPath.empty())
        {
            item.imageSource = "image://covercache/" + QString::fromStdString(coverPath);
        }
        else
        {
            item.imageSource = "";
        }
        item.isPlaying = false;
    }
    return item;
}

void MusicListModel::loadRoot()
{
    run_cover_test(); // 测试代码

    auto &controller = MediaController::getInstance();
    auto rootNode = controller.getRootNode();

    if (!rootNode)
    {
        qWarning() << "Root node is null. Did you scan?";
        return;
    }

    m_currentDirectoryNode = rootNode.get();
    setCurrentDirectoryNode(m_currentDirectoryNode);
    repopulateList(rootNode->getChildren());
}

void MusicListModel::repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes)
{
    beginResetModel();

    m_displayList.clear();
    m_fullList.clear(); // 同时清空备份
    m_nodeMap.clear();

    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();

    int idCounter = 0;

    for (const auto &nodePtr : nodes)
    {
        PlaylistNode *node = nodePtr.get();

        MusicItem item = createItemFromNode(node, idCounter);

        // 初始化播放状态
        if (playingNode && playingNode == node)
        {
            item.isPlaying = true;
        }

        // 同时填充显示列表和全量备份列表
        m_displayList.append(item);
        m_fullList.append(item);

        // 建立映射
        m_nodeMap[idCounter] = node;

        idCounter++;
    }

    endResetModel();
}

// [新增] 搜索功能实现
void MusicListModel::search(const QString &query)
{
    QString trimmed = query.trimmed();

    // 1. 如果查询为空，恢复全量列表
    if (trimmed.isEmpty())
    {
        // 只有当当前显示的不完整时才恢复，避免重复刷新
        if (m_displayList.size() != m_fullList.size())
        {
            beginResetModel();
            m_displayList = m_fullList; // 恢复数据

            // 恢复 Map：全量列表的 Map 也是线性的，重建一下比较安全
            m_nodeMap.clear();
            for (const auto &item : m_displayList)
            {
                m_nodeMap[item.id] = item.nodePtr;
            }

            endResetModel();
            refreshPlayingState(); // 恢复后刷新高亮
        }
        return;
    }

    // 2. 执行递归搜索
    beginResetModel();
    m_displayList.clear();
    m_nodeMap.clear(); // 搜索结果需要新的映射

    int idCounter = 0;

    if (m_currentDirectoryNode)
    {
        QList<MusicItem> results;
        recursiveSearch(m_currentDirectoryNode, trimmed, results, idCounter);
        m_displayList = results;
    }

    endResetModel();
}

void MusicListModel::recursiveSearch(PlaylistNode *node, const QString &query, QList<MusicItem> &results, int &idCounter)
{
    if (!node)
        return;

    for (const auto &childPtr : node->getChildren())
    {
        PlaylistNode *child = childPtr.get();

        bool match = false;
        if (child->isDir())
        {
            // 递归进入子文件夹查找
            recursiveSearch(child, query, results, idCounter);
        }
        else
        {
            // 文件匹配逻辑
            QString title = QString::fromStdString(child->getMetaData().getTitle());
            QString artist = QString::fromStdString(child->getMetaData().getArtist());
            // 如果 Meta 为空，匹配文件名
            if (title.isEmpty())
                title = QString::fromStdString(std::filesystem::path(child->getPath()).filename().string());

            // 模糊匹配
            if (title.contains(query, Qt::CaseInsensitive) || artist.contains(query, Qt::CaseInsensitive))
            {
                match = true;
            }
        }

        // 只添加匹配的文件 (文件夹本身不作为搜索结果，除非你需要)
        if (match && !child->isDir())
        {
            MusicItem item = createItemFromNode(child, idCounter);

            // 检查播放状态
            PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();
            if (playingNode && playingNode == child)
                item.isPlaying = true;

            results.append(item);
            m_nodeMap[idCounter] = child; // 建立新的 ID 映射
            idCounter++;
        }
    }
}

void MusicListModel::handleClick(int index)
{
    if (!m_nodeMap.contains(index))
    {
        qWarning() << "Clicked invalid index:" << index;
        return;
    }

    PlaylistNode *clickedNode = m_nodeMap[index];

    if (clickedNode->isDir())
    {
        // 进入文件夹
        m_currentDirectoryNode = clickedNode;
        repopulateList(clickedNode->getChildren());
        setCurrentDirectoryNode(clickedNode);
    }
    else
    {
        // 播放歌曲
        MediaController::getInstance().setNowPlayingSong(clickedNode);
        refreshPlayingState();
    }
}

void MusicListModel::refreshPlayingState()
{
    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();

    // 遍历 displayList 而不是 musicList
    for (int i = 0; i < m_displayList.count(); ++i)
    {
        MusicItem &item = m_displayList[i];
        if (item.isFolder)
            continue;

        // 在搜索模式下，m_nodeMap 也是对应的，所以可以安全查找
        if (!m_nodeMap.contains(item.id))
            continue;

        PlaylistNode *nodeInMap = m_nodeMap[item.id];
        bool isNowPlaying = (nodeInMap == playingNode);

        if (item.isPlaying != isNowPlaying)
        {
            item.isPlaying = isNowPlaying;
            QModelIndex idx = index(i);
            emit dataChanged(idx, idx, {PlayingRole});
        }
    }
}

void MusicListModel::setCurrentDirectoryNode(PlaylistNode *node)
{
    if (!node)
    {
        if (m_currentDirName != "播放列表")
        {
            m_currentDirName = "播放列表";
            emit currentDirNameChanged();
        }
        return;
    }

    QString newPath = QString::fromStdString(node->getPath());
    QFileInfo info(newPath);
    QString newDirName = info.fileName();

    if (newDirName.isEmpty() || newDirName == ".")
    {
        newDirName = "音乐库";
    }

    if (m_currentDirName != newDirName)
    {
        m_currentDirName = newDirName;
        emit currentDirNameChanged();
    }
    m_currentDirectoryNode = node;
}

void MusicListModel::goBack()
{
    if (!m_currentDirectoryNode)
        return;
    auto parentNode = m_currentDirectoryNode->getParent();

    if (parentNode)
    {
        m_currentDirectoryNode = parentNode.get();
        setCurrentDirectoryNode(m_currentDirectoryNode);
        repopulateList(parentNode->getChildren());
    }
}