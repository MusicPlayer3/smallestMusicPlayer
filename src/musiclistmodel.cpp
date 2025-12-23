#include "musiclistmodel.h"
#include "MediaController.hpp"
#include "PlaylistNode.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <QtConcurrent> // 需要包含此头文件进行异步操作

// ==========================================
// 搜索辅助逻辑
// ==========================================

namespace
{

// 搜索权重定义
enum SearchWeights
{
    SCORE_TITLE = 10,
    SCORE_ARTIST = 5,
    SCORE_ALBUM = 3,
    SCORE_FILENAME = 2
};

// 带有分数的中间结果
struct ScoredResult
{
    PlaylistNode *node;
    int score;
    // 用于排序：分数高的在前
    bool operator>(const ScoredResult &other) const
    {
        return score > other.score;
    }
};

// 辅助函数：计算单个字段的匹配分数
int calculateFieldScore(const std::string &fieldVal, const std::string &queryLower, int weight)
{
    if (fieldVal.empty())
        return 0;

    std::string fieldLower = fieldVal;
    // 转小写
    std::transform(fieldLower.begin(), fieldLower.end(), fieldLower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    size_t pos = fieldLower.find(queryLower);
    if (pos == std::string::npos)
        return 0;

    // 完全匹配
    if (fieldLower == queryLower)
        return weight * 10;
    // 前缀匹配
    if (pos == 0)
        return weight * 5;
    // 包含匹配
    return weight * 1;
}

} // namespace

// ==========================================
// MusicListModel 实现
// ==========================================

MusicListModel::MusicListModel(QObject *parent) : QAbstractListModel(parent)
{
    // 连接监听器：当异步任务完成时触发
    connect(&m_addWatcher, &QFutureWatcher<bool>::finished, this, [this]()
            {
        // 如果 m_isAdding 仍然为 true，说明没有被中途取消
        if (m_isAdding) {
            if (m_addWatcher.result()) {
                // 成功则刷新列表
                if (m_currentDirectoryNode) {
                    repopulateList(m_currentDirectoryNode->getChildren());
                } else {
                    loadRoot();
                }
            }
            m_isAdding = false;
            emit isAddingChanged();
        } });
}

int MusicListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_displayList.count();
}

QVariant MusicListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_displayList.size())
        return QVariant();

    const MusicItem &item = m_displayList.at(index.row());

    switch (role)
    {
    case TitleRole: return item.title;
    case ArtistRole: return item.artist;
    case AlbumRole: return item.album;
    case ExtraInfoRole: return item.extraInfo;
    case ParentDirRole: return item.parentDirName;
    case ImageRole: return item.imageSource;
    case PlayingRole: return item.isPlaying;
    case IsFolderRole: return item.isFolder;
    default: return QVariant();
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
    return QString("%1 | %2")
        .arg(node->getTotalSongs())
        .arg(formatDuration(node->getTotalDuration() * 1000000));
}

QString MusicListModel::formatSongInfo(PlaylistNode *node)
{
    const auto &meta = node->getMetaData();
    QStringList parts;
    parts << formatDuration(meta.getDuration());

    QString fmt = QString::fromStdString(meta.getFormatType()).toUpper();
    if (!fmt.isEmpty())
        parts << fmt;

    if (meta.getSampleRate() > 44100)
        parts << QString::number(meta.getSampleRate()) + " Hz";

    if (meta.getBitDepth() > 16)
        parts << QString::number(meta.getBitDepth()) + " bit";

    return parts.join(" | ");
}

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
        QString coverPath = QString::fromStdString("image://covercache/" + node->getThisDirCover());
        item.imageSource = coverPath;
        item.isPlaying = false;
        item.extraInfo = formatFolderInfo(node);

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
        if (item.title.isEmpty())
            item.title = QString::fromStdString(std::filesystem::path(node->getPath()).filename().string());

        item.artist = QString::fromStdString(meta.getArtist());
        item.album = QString::fromStdString(meta.getAlbum());
        item.extraInfo = formatSongInfo(node);

        if (!item.album.isEmpty())
            item.imageSource = "image://covercache/" + item.album;
        else
            item.imageSource = "image://covercache/" + item.title;

        item.isPlaying = false;
    }
    return item;
}

void MusicListModel::loadRoot()
{
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

// --------------------------------------------------------------------------
// 排序逻辑
// --------------------------------------------------------------------------

void MusicListModel::setSortMode(int type, bool reverse)
{
    if (m_sortType == type && m_sortReverse == reverse)
        return;

    m_sortType = type;
    m_sortReverse = reverse;

    emit sortTypeChanged();
    emit sortReverseChanged();

    applySort();
}

bool MusicListModel::lessThan(PlaylistNode *nodeA, PlaylistNode *nodeB) const
{
    // 1. 文件夹永远置顶
    // 如果 A 是文件夹，B 不是，A 在前 (True)
    if (nodeA->isDir() != nodeB->isDir())
    {
        return nodeA->isDir();
    }

    int compareResult = 0;
    const auto &metaA = nodeA->getMetaData();
    const auto &metaB = nodeB->getMetaData();

    switch (m_sortType)
    {
    case SortByTitle:
    {
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
    default:
        // 默认按标题
        QString tA = QString::fromStdString(metaA.getTitle());
        QString tB = QString::fromStdString(metaB.getTitle());
        compareResult = tA.compare(tB, Qt::CaseInsensitive);
    }

    if (compareResult == 0)
        return false;

    // 根据是否倒序返回结果
    if (m_sortReverse)
        return compareResult > 0;
    else
        return compareResult < 0;
}

void MusicListModel::performSort(bool syncBackend)
{
    // [新增功能] 搜索模式下禁用自定义排序，严格保持搜索结果的相关度顺序
    if (m_isSearching)
    {
        return;
    }

    // 1. 对 m_displayList 进行排序
    std::sort(m_displayList.begin(), m_displayList.end(), [&](const MusicItem &a, const MusicItem &b)
              { return lessThan(a.nodePtr, b.nodePtr); });

    // 2. 同步后端顺序 (这对 MediaController 的 Next/Prev 逻辑很重要)
    //    只有在浏览模式(非搜索)且 syncBackend 为 true 时才执行
    if (syncBackend && m_currentDirectoryNode)
    {
        m_currentDirectoryNode->reorderChildren([&](const std::shared_ptr<PlaylistNode> &a, const std::shared_ptr<PlaylistNode> &b)
                                                { return lessThan(a.get(), b.get()); });
    }
}

void MusicListModel::applySort()
{
    beginResetModel();
    performSort(!m_isSearching);
    endResetModel();
}

// 增加一些Delegate在ListView上
void MusicListModel::repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes)
{
    beginResetModel();
    m_isSearching = false;
    m_displayList.clear();

    m_displayList.reserve(nodes.size());

    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();
    int idCounter = 0;

    for (const auto &nodePtr : nodes)
    {
        PlaylistNode *node = nodePtr.get();
        MusicItem item = createItemFromNode(node, idCounter);
        if (playingNode && playingNode == node)
        {
            item.isPlaying = true;
        }
        m_displayList.append(item);
        idCounter++;
    }

    // 初始化时应用当前排序，并同步后端
    performSort(true);

    m_fullList = m_displayList;
    endResetModel();
}

void MusicListModel::search(const QString &query)
{
    QString trimmed = query.trimmed();

    if (trimmed.isEmpty())
    {
        if (m_isSearching)
        {
            beginResetModel();
            m_displayList = m_fullList;
            m_isSearching = false;
            // 取消搜索时恢复排序 (同步后端)
            performSort(true);
            endResetModel();
            refreshPlayingState();
        }
        return;
    }

    m_isSearching = true;

    std::string queryLower = trimmed.toStdString();
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    std::vector<ScoredResult> scoredResults;
    scoredResults.reserve(100);

    // 递归搜索辅助 lambda
    std::function<void(PlaylistNode *)> recursiveScoreSearch =
        [&](PlaylistNode *node)
    {
        if (!node)
            return;
        for (const auto &childPtr : node->getChildren())
        {
            PlaylistNode *child = childPtr.get();
            if (child->isDir())
            {
                recursiveScoreSearch(child);
            }
            else
            {
                const auto &meta = child->getMetaData();
                int totalScore = 0;

                totalScore += calculateFieldScore(meta.getTitle(), queryLower, SCORE_TITLE);
                totalScore += calculateFieldScore(meta.getArtist(), queryLower, SCORE_ARTIST);
                totalScore += calculateFieldScore(meta.getAlbum(), queryLower, SCORE_ALBUM);

                std::string filename = std::filesystem::path(child->getPath()).filename().string();
                totalScore += calculateFieldScore(filename, queryLower, SCORE_FILENAME);

                if (totalScore > 0)
                {
                    scoredResults.push_back({child, totalScore});
                }
            }
        }
    };

    if (m_currentDirectoryNode)
    {
        recursiveScoreSearch(m_currentDirectoryNode);
    }

    // 按分数排序 (分数高的在前)
    std::sort(scoredResults.begin(), scoredResults.end(), std::greater<ScoredResult>());

    beginResetModel();
    m_displayList.clear();

    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();
    int idCounter = 0;

    for (const auto &res : scoredResults)
    {
        MusicItem item = createItemFromNode(res.node, idCounter);
        if (playingNode && playingNode == res.node)
        {
            item.isPlaying = true;
        }
        m_displayList.append(item);
        idCounter++;
    }

    endResetModel();
}

void MusicListModel::handleClick(int index)
{
    if (index < 0 || index >= m_displayList.size())
    {
        return;
    }

    // 直接通过索引访问，无需 map
    const MusicItem &item = m_displayList.at(index);
    PlaylistNode *clickedNode = item.nodePtr;

    if (!clickedNode)
        return;

    if (clickedNode->isDir())
    {
        m_currentDirectoryNode = clickedNode;
        setCurrentDirectoryNode(clickedNode);
        repopulateList(clickedNode->getChildren());
        emit requestScrollTo(0);
    }
    else
    {
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
    std::filesystem::path p(node->getPath());
    QString newDirName = QString::fromStdString(p.filename().string());
    if (newDirName.isEmpty() || newDirName == ".")
        newDirName = "音乐库";

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
        PlaylistNode *oldDirNode = m_currentDirectoryNode;
        m_currentDirectoryNode = parentNode.get();
        setCurrentDirectoryNode(m_currentDirectoryNode);
        repopulateList(parentNode->getChildren());

        for (int i = 0; i < m_displayList.size(); ++i)
        {
            if (m_displayList[i].nodePtr == oldDirNode)
            {
                emit requestScrollTo(i);
                break;
            }
        }
    }
}

void MusicListModel::locateCurrentPlaying()
{
    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();
    if (!playingNode)
        return;

    for (int i = 0; i < m_displayList.size(); ++i)
    {
        if (m_displayList[i].nodePtr == playingNode)
        {
            emit requestScrollTo(i);
            return;
        }
    }

    auto parent = playingNode->getParent();
    if (parent)
    {
        m_currentDirectoryNode = parent.get();
        setCurrentDirectoryNode(m_currentDirectoryNode);
        repopulateList(parent->getChildren());

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

void MusicListModel::ListViewAddNewFolder(const QString &path)
{
    if (m_isAdding || path.isEmpty())
        return;

    m_isAdding = true;
    emit isAddingChanged();

    std::string stdPath = path.toStdString();
    // 确定父节点，如果当前没有目录，则使用根节点
    PlaylistNode *parentNode = m_currentDirectoryNode;

    // 启动异步任务
    auto future = QtConcurrent::run([this, stdPath, parentNode]()
                                    {
        auto &controller = MediaController::getInstance();
        // 调用 MediaController 的实际接口
        return controller.addFolder(stdPath, parentNode); });

    m_addWatcher.setFuture(future);
}

void MusicListModel::ListViewAddNewFile(const QString &path)
{
    if (m_isAdding || path.isEmpty())
        return;

    m_isAdding = true;
    emit isAddingChanged();

    std::string stdPath = path.toStdString();
    PlaylistNode *parentNode = m_currentDirectoryNode;

    auto future = QtConcurrent::run([this, stdPath, parentNode]()
                                    {
        auto &controller = MediaController::getInstance();
        // 调用 MediaController 的实际接口
        return controller.addSong(stdPath, parentNode); });

    m_addWatcher.setFuture(future);
}

void MusicListModel::cancelAdding()
{
    if (m_isAdding)
    {
        m_isAdding = false; // 重置标志位，完成时的回调将不再刷新 UI
        emit isAddingChanged();
    }
}

QVariantMap MusicListModel::getDetailInfo(int index)
{
    QVariantMap map;
    if (index < 0 || index >= m_displayList.size())
        return map;

    const MusicItem &item = m_displayList.at(index);
    PlaylistNode *node = item.nodePtr;
    if (!node)
        return map;

    // 通用信息
    map["isFolder"] = node->isDir();
    map["title"] = item.title; // 已经处理过显示名称
    map["path"] = QString::fromStdString(node->getPath());
    map["cover"] = item.imageSource;

    if (node->isDir())
    {
        // --- 文件夹信息 ---
        if (auto parent = node->getParent())
        {
            std::filesystem::path pp(parent->getPath());
            map["parentName"] = QString::fromStdString(pp.filename().string());
        }
        else
        {
            map["parentName"] = "Root";
        }
        map["songCount"] = QString::number(node->getTotalSongs());
        map["totalDuration"] = formatDuration(node->getTotalDuration() * 1000000);
    }
    else
    {
        // --- 歌曲信息 ---
        const auto &meta = node->getMetaData();
        map["artist"] = QString::fromStdString(meta.getArtist());
        map["album"] = QString::fromStdString(meta.getAlbum());
        map["year"] = QString::fromStdString(meta.getYear());
        map["sampleRate"] = QString::number(meta.getSampleRate()) + " Hz";
        map["bitDepth"] = QString::number(meta.getBitDepth()) + " bit";
        map["format"] = QString::fromStdString(meta.getFormatType()).toUpper();

        // 实时获取播放数据
        auto &controller = MediaController::getInstance();
        map["playCount"] = controller.getSongsPlayCount(node);
        map["rating"] = controller.getSongsRating(node);
    }

    return map;
}

void MusicListModel::deleteItem(int index, bool deletePhysicalFile)
{
    if (index < 0 || index >= m_displayList.size())
        return;

    const MusicItem &item = m_displayList.at(index);
    PlaylistNode *node = item.nodePtr;
    if (!node)
        return;

    auto &controller = MediaController::getInstance();

    // 1. 调用 Controller 执行删除逻辑 (同时处理数据库和文件系统)
    if (node->isDir())
    {
        controller.removeFolder(node, deletePhysicalFile);
    }
    else
    {
        controller.removeSong(node, deletePhysicalFile);
    }

    // 2. 刷新列表显示
    // 此时 node 指针已经被析构或失效，不能再使用。
    // 我们重新加载当前目录的子节点列表即可。
    if (m_currentDirectoryNode)
    {
        repopulateList(m_currentDirectoryNode->getChildren());
    }
    else
    {
        loadRoot();
    }
}

void MusicListModel::setItemRating(int index, int rating)
{
    if (index < 0 || index >= m_displayList.size())
        return;

    const MusicItem &item = m_displayList.at(index);
    PlaylistNode *node = item.nodePtr;

    // 只有文件（非目录）才能设置星级
    if (!node || node->isDir())
        return;

    // 调用 MediaController 更新数据（内存 + 数据库）
    MediaController::getInstance().setSongsRating(node, rating);

    // 注意：这里不需要手动 emit dataChanged，因为 InfoDialog 是模态的/独立的。
    // 如果需要在主列表显示星级，才需要触发 Model 刷新。
    // 目前逻辑主要服务于 InfoDialog 的即时显示。
}