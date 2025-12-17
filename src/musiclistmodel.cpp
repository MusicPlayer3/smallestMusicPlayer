#include "musiclistmodel.h"
#include "MediaController.hpp"
#include "PlaylistNode.hpp"
#include <algorithm> // for std::sort, std::transform
#include <cctype>    // for std::tolower

// 引入封面缓存测试 (如果不需要可以移除)
#include "CoverCache.hpp"
extern void run_cover_test();

// ==========================================
// [新增] 移植自 MediaController 的搜索算法辅助结构
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

    // 转小写进行比较
    std::string fieldLower = fieldVal;
    std::transform(fieldLower.begin(), fieldLower.end(), fieldLower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    size_t pos = fieldLower.find(queryLower);
    if (pos == std::string::npos)
    {
        return 0;
    }

    // 策略：
    // 1. 完全匹配：最高分
    if (fieldLower == queryLower)
    {
        return weight * 10;
    }
    // 2. 前缀匹配（从头开始）：中等分
    if (pos == 0)
    {
        return weight * 5;
    }
    // 3. 包含匹配：基础分
    return weight * 1;
}

} // namespace

// ==========================================
// MusicListModel 实现
// ==========================================

MusicListModel::MusicListModel(QObject *parent) :
    QAbstractListModel(parent)
{
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
#ifdef DEBUG
    run_cover_test();
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
    if (nodeA->isDir() != nodeB->isDir())
    {
        return nodeA->isDir() > nodeB->isDir();
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
        QString tA = QString::fromStdString(metaA.getTitle());
        QString tB = QString::fromStdString(metaB.getTitle());
        compareResult = tA.compare(tB, Qt::CaseInsensitive);
    }

    if (compareResult == 0)
        return false;

    if (m_sortReverse)
        return compareResult > 0;
    else
        return compareResult < 0;
}

void MusicListModel::performSort(bool syncBackend)
{
    std::sort(m_displayList.begin(), m_displayList.end(), [&](const MusicItem &a, const MusicItem &b)
              { return lessThan(a.nodePtr, b.nodePtr); });

    m_nodeMap.clear();
    for (const auto &item : m_displayList)
    {
        m_nodeMap[item.id] = item.nodePtr;
    }

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

void MusicListModel::repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes)
{
    beginResetModel();
    m_isSearching = false;
    m_displayList.clear();
    m_nodeMap.clear();

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

    performSort(true);
    m_fullList = m_displayList;
    endResetModel();
}

// --------------------------------------------------------------------------
// [修改] 搜索功能：使用加权打分算法 (移植自 MediaController)
// --------------------------------------------------------------------------
void MusicListModel::search(const QString &query)
{
    QString trimmed = query.trimmed();

    // 1. 如果查询为空，恢复全量列表
    if (trimmed.isEmpty())
    {
        if (m_displayList.size() != m_fullList.size() || m_isSearching)
        {
            beginResetModel();
            m_displayList = m_fullList;
            m_isSearching = false;

            m_nodeMap.clear();
            for (const auto &item : m_displayList)
            {
                m_nodeMap[item.id] = item.nodePtr;
            }
            performSort(true);
            endResetModel();
            refreshPlayingState();
        }
        return;
    }

    // 2. 准备搜索
    m_isSearching = true;

    // 将查询转为小写 std::string，供算法使用
    std::string queryLower = trimmed.toStdString();
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    std::vector<ScoredResult> scoredResults;

    // 3. 执行递归搜索并打分
    if (m_currentDirectoryNode)
    {
        // 这里的递归函数需要稍微调整逻辑以支持打分收集，
        // 或者我们为了不修改头文件，在 .cpp 里单独定义一个递归 lambda 或 helper。
        // 为了方便，这里直接定义一个 lambda 递归。

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

                    // 应用 MediaController 的打分逻辑
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

        recursiveScoreSearch(m_currentDirectoryNode);
    }

    // 4. 对结果按分数排序 (分数高的在前)
    std::sort(scoredResults.begin(), scoredResults.end(), std::greater<ScoredResult>());

    // 5. 更新 View
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

    // 注意：搜索结果是按相关度排序的，这里不应该再调用 performSort
    // 除非用户强制要求按“标题”排序覆盖“相关度”。
    // 通常搜索结果按相关度展示最好。
    // 但为了保证数据完整性（如 m_nodeMap），我们手动建立映射，不调用 performSort。
    m_nodeMap.clear();
    for (const auto &item : m_displayList)
    {
        m_nodeMap[item.id] = item.nodePtr;
    }

    endResetModel();
}

// 移除旧的 recursiveSearch 实现（如果头文件里声明了 private，可以留着空函数体或者在头文件里删掉）
// 为了保证编译通过，这里保留旧函数的实现，但实际上 search 函数已经不再调用它了。
void MusicListModel::recursiveSearch(PlaylistNode *node, const QString &query, QList<MusicItem> &results, int &idCounter)
{
    // 此函数已被新的 search 逻辑内部的 lambda 替代，保留它是为了不修改头文件的声明。
    Q_UNUSED(node);
    Q_UNUSED(query);
    Q_UNUSED(results);
    Q_UNUSED(idCounter);
}

void MusicListModel::handleClick(int index)
{
    if (index < 0 || index >= m_displayList.size())
    {
        qWarning() << "Clicked invalid index:" << index << "List size:" << m_displayList.size();
        return;
    }

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
    QFileInfo info(newPath);
    QString newDirName = info.fileName();
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
            emit requestScrollTo(targetIndex);
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