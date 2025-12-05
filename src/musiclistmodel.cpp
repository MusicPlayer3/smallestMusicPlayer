#include "musiclistmodel.h"
#include "MediaController.hpp"
#include "PlaylistNode.hpp"
#include <QDebug>
#include <filesystem> // 用于获取文件夹名称
#include <qdebug.h>

// ---TODO:这里未来是给我们的MusicNode读取一下，然后把里面的数据暴露出来
MusicListModel::MusicListModel(QObject *parent) :
    QAbstractListModel(parent)
{
}

// --- 必需的 QAbstractListModel 实现 ---

int MusicListModel::rowCount(const QModelIndex &parent) const
{
    // 如果 parent 有效，则返回 0 (非树形结构)
    if (parent.isValid())
        return 0;

    return m_musicList.count();
}

QVariant MusicListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_musicList.size())
        return QVariant();

    const MusicItem &item = m_musicList.at(index.row());

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
    // 关键：定义 QML 中使用的属性名 (roleName)
    roles[TitleRole] = "title";
    roles[ArtistRole] = "artist";
    roles[ImageRole] = "imageSource";
    roles[PlayingRole] = "isPlaying";
    roles[IsFolderRole] = "isFolder";
    return roles;
}

// --- 数据操作方法 ---
#include "CoverCache.hpp"     // TODO:测试记得删除
extern void run_cover_test(); // TODO:测试记得删除
// 1. 加载根目录：从 MediaController 获取 Root
void MusicListModel::loadRoot()
{
    // ===================================================
    // ⭐️ 最佳调用位置：在模型开始加载数据之前，检查缓存
    // ===================================================
    run_cover_test();

    // 获取后端单例
    auto &controller = MediaController::getInstance();
    // 获取根节点
    auto rootNode = controller.getRootNode(); // 假设 MediaController 有 getRoot()，如果没有需添加或用 getCurrentDirectory()

    // 如果没有根节点（比如未扫描），直接返回
    if (!rootNode)
    {
        qWarning() << "Root node is null. Did you scan?";
        return;
    }

    m_currentDirectoryNode = rootNode.get(); // 记录当前位置
    repopulateList(rootNode->getChildren());
}

// 2. 核心：重置列表数据，建立映射
void MusicListModel::repopulateList(const std::vector<std::shared_ptr<PlaylistNode>> &nodes)
{
    // 告知 View 模型将彻底重置 (比 insertRows 更适合全量刷新)
    beginResetModel();

    // 1. 清空旧数据
    m_musicList.clear();
    m_nodeMap.clear();

    // 2. 获取当前正在播放的节点 (用于初始化 isPlaying 状态)
    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();

    int idCounter = 0; // 编号生成器

    // 3. 遍历 Vector
    for (const auto &nodePtr : nodes)
    {
        PlaylistNode *node = nodePtr.get();
        MusicItem item;

        // 设置编号
        item.id = idCounter;

        // 存入 Map <编号, 指针>
        m_nodeMap[idCounter] = node;

        // --- 文件夹处理 ---
        if (node->isDir())
        {
            item.isFolder = true;
            item.artist = ""; // 文件夹作者置空

            // 使用 filesystem 获取纯文件夹名
            std::filesystem::path p(node->getPath());
            item.title = QString::fromStdString(p.filename().string());

            // 文件夹图标
            // 这里改成获取他的一个文件夹封面的Key,然后再传过去就好了
            QString coverPath = QString::fromStdString("image://covercache/" + node->getThisDirCover());
            item.imageSource = coverPath; // 请确保你有这个资源，或者先留空
            item.isPlaying = false;
        }
        // --- 音乐文件处理 ---
        else
        {
            item.isFolder = false;
            const auto &meta = node->getMetaData();

            item.title = QString::fromStdString(meta.getTitle());
            // 如果为空，设置成为他的文件名称
            if (item.title.isEmpty())
                item.title = QString::fromStdString(std::filesystem::path(node->getPath()).filename().string());

            item.artist = QString::fromStdString(meta.getArtist());

            // 封面处理：使用 image:// 协议 + 封面路径/Key（专辑名称）
            std::string coverPath = meta.getAlbum();
            if (!coverPath.empty())
            {
                item.imageSource = "image://covercache/" + QString::fromStdString(coverPath);
            }
            else
            {
                item.imageSource = ""; // 默认图逻辑交给 UI
            }

            // 初始化时检查是否正在播放
            if (playingNode && playingNode == node)
            {
                item.isPlaying = true;
            }
            else
            {
                item.isPlaying = false;
            }
        }

        m_musicList.append(item);
        idCounter++;
    }

    endResetModel();
}

// 3. 处理点击事件
void MusicListModel::handleClick(int index)
{
    // 1. 安全检查
    if (!m_nodeMap.contains(index))
    {
        qWarning() << "Clicked invalid index:" << index;
        return;
    }

    PlaylistNode *clickedNode = m_nodeMap[index];

    // 2. 判断类型
    if (clickedNode->isDir())
    {
        // --- 点击了文件夹 ---
        // qDebug() << "Enter Folder:" << QString::fromStdString(clickedNode->getPath());

        // 更新当前目录记录
        m_currentDirectoryNode = clickedNode;

        // 递归加载：取出子节点，刷新列表
        repopulateList(clickedNode->getChildren());

        // 可选：通知 MediaController 我们进入了新目录 (如果后端需要知道浏览状态)
        // 后端当然不需要知道我们去哪里了，这个游标只由我们管理
        // MediaController::getInstance().enterDirectory(clickedNode);
    }
    else
    {
        // --- 点击了音乐文件 ---
        // qDebug() << "Play Song:" << QString::fromStdString(clickedNode->getMetaData().getTitle());

        // 调用后端播放
        MediaController::getInstance().setNowPlayingSong(clickedNode);

        // 立即刷新 UI 状态
        refreshPlayingState();
    }
}

// 4. 刷新高亮状态 (供轮询或点击调用)
void MusicListModel::refreshPlayingState()
{
    PlaylistNode *playingNode = MediaController::getInstance().getCurrentPlayingNode();

    // 遍历当前列表的所有项
    for (int i = 0; i < m_musicList.count(); ++i)
    {
        MusicItem &item = m_musicList[i];

        // 如果是文件夹，跳过
        if (item.isFolder)
            continue;

        // 通过 Map 找到对应的 Node
        PlaylistNode *nodeInMap = m_nodeMap[item.id];

        bool isNowPlaying = (nodeInMap == playingNode);

        // 只有状态改变时才触发更新信号
        if (item.isPlaying != isNowPlaying)
        {
            item.isPlaying = isNowPlaying;
            // 通知 View 这一行的 PlayingRole 数据变了
            QModelIndex idx = index(i);
            emit dataChanged(idx, idx, {PlayingRole});
        }
    }
}

// 返回上一级 (可选实现) 后面来实现,暂时是没有的
// TODO:返回上一级
void MusicListModel::goBack()
{
    auto parentNode = m_currentDirectoryNode->getParent();

    if (parentNode)
    {
        qDebug() << "Yes have a parent";
        // 3. 更新 Model 内部记录的当前目录
        m_currentDirectoryNode = parentNode.get(); // 假设你有这个成员变量

        // 4. 使用新目录的子项来刷新 QML 视图
        repopulateList(parentNode->getChildren()); // 假设 repopulateList 已经存在
    }
    // auto &controller = MediaController::getInstance();
    // qDebug() << "in goBack()";
    // 1. 调用 MediaController 的核心业务逻辑
    // controller.returnParentDirectory();

    // 2. 获取返回后的新目录节点
    // PlaylistNode *newDirNode = controller.getCurrentDirectory();

    // 注意：如果 newDirNode 为空，说明已经在根目录，MediaController 已经阻止了返回
}

// --- 测试版本的加载Model歌曲列表信息 ---

// 1. const std::vector<std::shared_ptr<PlaylistNode>> &getChildren() const把Vector拿到
// 2. 遍历PlaylistNode *getCurrentDirectory()里面的Vecter
// 3. 把Vector里面的数据，放到*QList<MusicItem>里面
// 4.
// void MusicListModel::loadInitialData()
// {
//     QList<MusicItem> initialData = {
//         {"これくらいで", "蓝月なくる", QUrl("qrc:/cover1.png"), false},
//         {"夢で逢いましょう", "SARD UNDERGROUND", QUrl("qrc:/cover2.jpg"), true}, // isPlaying: true
//         {"いのちの名前 (Cover...)", "Akie秋绘", QUrl("qrc:/cover3.png"), false},
//         {"君に最後のログづけを", "ま.ゴ娘", QUrl("qrc:/cover4.png"), false},
//         {"Fotizo", "nayuta/ARForest", QUrl("qrc:/cover5.png"), false},
//         {"My Sweet Maiden", "Mia REGINA", QUrl("qrc:/cover6.png"), false}};
//     // int cnt;
//     // for (//vector)
//     // {
//     //     MusicItem item;
//     //     vector->item; // 从vector里面拿到的PlaylistNode
//     //                   //
//     //     if(//先判断是不是一个文件夹){
//     //         //是 的话,把他 artist写成Dir

//     //         //否 的话,就不管了
//     //     }else{
//     //         map<*MusicItem, PlaylistNode>;
//     //         initialData.push_back(std::move(item));
//     //         map[&initialData[cnt]] = &vector->metaData;
//     //     }
//     // }
//     // 后面还需要再写一个clean函数把这里的 map 还有musicList里面的数据都清空
//     beginInsertRows(QModelIndex(), 0, initialData.count() - 1);
//     m_musicList.append(initialData);
//     endInsertRows();
// }
