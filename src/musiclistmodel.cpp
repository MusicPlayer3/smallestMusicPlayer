#include "musiclistmodel.h"
#include "MediaController.hpp"
#include "PlaylistNode.hpp"


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
    case AlbumRole:
        return item.album;
    case ExtraInfoRole:
        return item.extraInfo;
    case ParentDirRole:
        return item.parentDirName;
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
    roles[AlbumRole] = "albumName";
    roles[ExtraInfoRole] = "extraInfo";
    roles[ParentDirRole] = "parentDirName";
    roles[ImageRole] = "imageSource";
    roles[PlayingRole] = "isPlaying";
    roles[IsFolderRole] = "isFolder";
    return roles;
}

// --- 格式化辅助函数 ---
QString MusicListModel::formatDuration(int64_t microsec)
{
    qint64 secs = microsec / 1000000;
    qint64 hours = secs / 3600;
    qint64 minutes = (secs % 3600) / 60;
    qint64 seconds = secs % 60;

    if (hours > 0)
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    else
        return QString("%1:%2")
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
}

QString MusicListModel::formatFolderInfo(PlaylistNode *node)
{
    // 歌曲数量 | 歌曲总时长
    // 注意：PlaylistNode 需要确保 totalSongs 和 totalDuration 已正确统计(单位:秒)
    return QString("%1 | %2")
        .arg(node->getTotalSongs())
        .arg(formatDuration(node->getTotalDuration() * 1000000));
}

QString MusicListModel::formatSongInfo(PlaylistNode *node)
{
    // 时长 | 音频格式 | 音频采样率（>44100） | 音频采样深度（>16bit）
    const auto &meta = node->getMetaData();
    QStringList parts;

    // 1. 时长
    parts << formatDuration(meta.getDuration());

    // 2. 格式
    QString fmt = QString::fromStdString(meta.getFormatType()).toUpper();
    if (!fmt.isEmpty())
        parts << fmt;

    // 3. 采样率
    if (meta.getSampleRate() > 44100)
    {
        parts << QString::number(meta.getSampleRate()) + " Hz";
    }

    // 4. 位深
    if (meta.getBitDepth() > 16)
    {
        parts << QString::number(meta.getBitDepth()) + " bit";
    }

    return parts.join(" | ");
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
        std::filesystem::path p(node->getPath());
        item.title = QString::fromStdString(p.filename().string());
        // 获取文件夹封面 Key
        QString coverPath = QString::fromStdString("image://covercache/" + node->getThisDirCover());
        item.imageSource = coverPath;
        item.isPlaying = false;

        // [新增] 文件夹额外信息
        item.extraInfo = formatFolderInfo(node);

        // [新增] 上级目录名
        if (auto parent = node->getParent())
        {
            std::filesystem::path pp(parent->getPath());
            item.parentDirName = QString::fromStdString(pp.filename().string());
        }
        else
        {
            item.parentDirName = "Root";
        }
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
        item.album = QString::fromStdString(meta.getAlbum());

        // [新增] 歌曲额外信息
        item.extraInfo = formatSongInfo(node);

        if (!item.album.isEmpty())
        {
            item.imageSource = "image://covercache/" + item.album;
        }
        else
        {
            item.imageSource = "image://covercache/" + item.title;
        }
        item.isPlaying = false;
    }
    return item;
}

void MusicListModel::loadRoot()
{
#ifdef DEBUG
    run_cover_test(); // 测试代码
#endif

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

// [新增] 排序设置
void MusicListModel::setSortMode(int type, bool reverse)
{
    if (m_sortType == type && m_sortReverse == reverse)
        return;

    m_sortType = type;
    m_sortReverse = reverse;

    emit sortTypeChanged();
    emit sortReverseChanged();

    // 重新排序并刷新
    applySort();
}

// [修复] 排序逻辑：修复倒序时的 strict weak ordering 崩溃问题
void MusicListModel::applySort()
{
    if (m_displayList.isEmpty())
        return;

    // 1. 定义核心比较逻辑
    // 为了同时复用于 UI(MusicItem) 和 后端(PlaylistNode)，我们把逻辑抽离出来
    auto comparator = [this](PlaylistNode *nodeA, PlaylistNode *nodeB) -> bool
    {
        // 规则0：文件夹永远在最前面
        if (nodeA->isDir() != nodeB->isDir())
        {
            return nodeA->isDir() > nodeB->isDir(); // true(1) > false(0) -> 文件夹在前
        }

        int compareResult = 0;
        const auto &metaA = nodeA->getMetaData();
        const auto &metaB = nodeB->getMetaData();

        switch (m_sortType)
        {
        case SortByTitle:
        {
            // 优先用 Meta 标题，没有则用文件名
            QString tA = QString::fromStdString(metaA.getTitle());
            if (tA.isEmpty())
                tA = QString::fromStdString(std::filesystem::path(nodeA->getPath()).filename().string());
            QString tB = QString::fromStdString(metaB.getTitle());
            if (tB.isEmpty())
                tB = QString::fromStdString(std::filesystem::path(nodeB->getPath()).filename().string());
            compareResult = tA.compare(tB, Qt::CaseInsensitive);
            break;
        }
        case SortByFilename:
        {
            QString fa = QString::fromStdString(std::filesystem::path(nodeA->getPath()).filename().string());
            QString fb = QString::fromStdString(std::filesystem::path(nodeB->getPath()).filename().string());
            compareResult = fa.compare(fb, Qt::CaseInsensitive);
            break;
        }
        case SortByPath:
        {
            QString pa = QString::fromStdString(nodeA->getPath());
            QString pb = QString::fromStdString(nodeB->getPath());
            compareResult = pa.compare(pb, Qt::CaseInsensitive);
            break;
        }
        case SortByArtist:
        {
            QString aa = QString::fromStdString(metaA.getArtist());
            QString ab = QString::fromStdString(metaB.getArtist());
            compareResult = aa.compare(ab, Qt::CaseInsensitive);
            break;
        }
        case SortByAlbum:
        {
            QString aa = QString::fromStdString(metaA.getAlbum());
            QString ab = QString::fromStdString(metaB.getAlbum());
            compareResult = aa.compare(ab, Qt::CaseInsensitive);
            break;
        }
        case SortByYear:
        {
            QString ya = QString::fromStdString(metaA.getYear());
            QString yb = QString::fromStdString(metaB.getYear());
            compareResult = ya.compare(yb, Qt::CaseInsensitive);
            break;
        }
        case SortByDuration:
            if (metaA.getDuration() < metaB.getDuration())
                compareResult = -1;
            else if (metaA.getDuration() > metaB.getDuration())
                compareResult = 1;
            break;
        case SortByDate:
            if (metaA.getLastWriteTime() < metaB.getLastWriteTime())
                compareResult = -1;
            else if (metaA.getLastWriteTime() > metaB.getLastWriteTime())
                compareResult = 1;
            break;
        default: // 默认按标题
            QString tA = QString::fromStdString(metaA.getTitle());
            QString tB = QString::fromStdString(metaB.getTitle());
            compareResult = tA.compare(tB, Qt::CaseInsensitive);
        }

        // 只有当不相等时才考虑顺序，相等时必须返回 false (Strict Weak Ordering)
        if (compareResult == 0)
            return false;

        if (m_sortReverse)
        {
            return compareResult > 0;
        }
        else
        {
            return compareResult < 0;
        }
    };

    // 2. 排序 UI 列表 (MusicItem)
    beginResetModel();
    std::sort(m_displayList.begin(), m_displayList.end(), [&](const MusicItem &a, const MusicItem &b)
              { return comparator(a.nodePtr, b.nodePtr); });

    // 重建映射
    m_nodeMap.clear();
    for (const auto &item : m_displayList)
    {
        m_nodeMap[item.id] = item.nodePtr;
    }
    endResetModel();

    // 3. [核心修复] 同步排序后端数据 (PlaylistNode Children)
    // 只有在浏览目录模式下（非搜索模式），且当前目录有效时，才同步后端
    // 通过对比当前列表数量与备份列表数量粗略判断是否在搜索模式，或者直接信任当前目录
    if (m_currentDirectoryNode)
    {
        // 我们总是对当前的文件夹进行排序，以确保逻辑一致性
        // 注意：reorderChildren 接收的是 shared_ptr，我们需要适配一下 comparator
        m_currentDirectoryNode->reorderChildren([&](const std::shared_ptr<PlaylistNode> &a, const std::shared_ptr<PlaylistNode> &b)
                                                { return comparator(a.get(), b.get()); });
    }
}

void MusicListModel::repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes)
{
    m_displayList.clear();
    m_fullList.clear();
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

        m_displayList.append(item);
        idCounter++;
    }

    // 生成完数据后，进行排序并刷新 UI
    applySort();

    // 备份排序后的完整列表
    m_fullList = m_displayList;
}

// 搜索功能
void MusicListModel::search(const QString &query)
{
    QString trimmed = query.trimmed();

    // 1. 如果查询为空，恢复全量列表
    if (trimmed.isEmpty())
    {
        if (m_displayList.size() != m_fullList.size())
        {
            beginResetModel();
            m_displayList = m_fullList; // 恢复数据

            // 恢复 Map
            m_nodeMap.clear();
            for (const auto &item : m_displayList)
            {
                m_nodeMap[item.id] = item.nodePtr;
            }

            endResetModel();
            refreshPlayingState();
            // 恢复后最好确保排序也是应用的 (虽然 m_fullList 应该是排过序的)
            applySort();
        }
        return;
    }

    // 2. 执行递归搜索
    // 搜索时暂时清除显示列表，收集结果后再显示
    m_displayList.clear();
    m_nodeMap.clear();

    int idCounter = 0;

    if (m_currentDirectoryNode)
    {
        QList<MusicItem> results;
        recursiveSearch(m_currentDirectoryNode, trimmed, results, idCounter);
        m_displayList = results;
    }

    // 搜索结果也应用当前排序
    applySort();
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

        if (match && !child->isDir())
        {
            MusicItem item = createItemFromNode(child, idCounter);

            // 检查播放状态
            PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();
            if (playingNode && playingNode == child)
                item.isPlaying = true;

            results.append(item);
            // 这里不设置 m_nodeMap，因为 applySort 会统一设置
            idCounter++;
        }
    }
}

void MusicListModel::handleClick(int index)
{
    // [修复 Bug]：
    // 之前的逻辑使用了 m_nodeMap[index]，这是把“行号”当成了“ID”用。
    // 排序后，第0行的数据 ID 可能并不是0。
    // 现在的逻辑：直接根据行号(index)从 m_displayList 中取数据，保证视觉和逻辑一致。

    if (index < 0 || index >= m_displayList.size())
    {
        qWarning() << "Clicked invalid index:" << index;
        return;
    }

    // 直接获取对应行的数据项
    const MusicItem &item = m_displayList.at(index);
    PlaylistNode *clickedNode = item.nodePtr;

    if (!clickedNode)
    {
        return;
    }

    if (clickedNode->isDir())
    {
        // 进入文件夹
        m_currentDirectoryNode = clickedNode;
        setCurrentDirectoryNode(clickedNode);
        repopulateList(clickedNode->getChildren());
        // 进入新目录，滚动到顶部
        emit requestScrollTo(0);
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

    for (int i = 0; i < m_displayList.count(); ++i)
    {
        MusicItem &item = m_displayList[i];
        if (item.isFolder)
            continue;

        // [优化]：
        // 这里也不需要再查 m_nodeMap 了，直接对比 item 里的指针即可。
        // 这样消除了对 ID 映射的所有依赖，更加稳健。

        bool isNowPlaying = (item.nodePtr == playingNode);

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
        // [新增] 记录返回前的目录节点
        PlaylistNode *oldDirNode = m_currentDirectoryNode;

        m_currentDirectoryNode = parentNode.get();
        setCurrentDirectoryNode(m_currentDirectoryNode);
        repopulateList(parentNode->getChildren());

        // [新增] 在新列表中查找之前进入的文件夹，并请求滚动
        int targetIndex = -1;
        for (int i = 0; i < m_displayList.size(); ++i)
        {
            if (m_displayList[i].nodePtr == oldDirNode)
            {
                targetIndex = i;
                break;
            }
        }

        if (targetIndex != -1)
        {
            emit requestScrollTo(targetIndex);
        }
    }
}

// [新增] 定位当前播放歌曲
void MusicListModel::locateCurrentPlaying()
{
    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();
    if (!playingNode)
        return;

    // 1. 检查当前列表是否已经包含该歌曲
    for (int i = 0; i < m_displayList.size(); ++i)
    {
        if (m_displayList[i].nodePtr == playingNode)
        {
            emit requestScrollTo(i);
            return;
        }
    }

    // 2. 如果不在，则切换到该歌曲所在的父目录
    auto parent = playingNode->getParent();
    if (parent)
    {
        m_currentDirectoryNode = parent.get();
        setCurrentDirectoryNode(m_currentDirectoryNode);
        repopulateList(parent->getChildren());

        // 重新查找索引
        for (int i = 0; i < m_displayList.size(); ++i)
        {
            if (m_displayList[i].nodePtr == playingNode)
            {
                emit requestScrollTo(i);
                break;
            }
        }
    }
}