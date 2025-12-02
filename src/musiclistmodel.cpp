#include "musiclistmodel.h"
#include <QDebug>

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
    return roles;
}

// --- 数据操作方法 ---

void MusicListModel::addMusicItem(const MusicItem &item)
{
    // 告知视图：将要插入数据
    beginInsertRows(QModelIndex(), m_musicList.count(), m_musicList.count());

    // 插入数据
    m_musicList.append(item);

    // 告知视图：插入完成
    endInsertRows();
}

// --- 测试版本的加载Model歌曲列表信息 ---

// 1. const std::vector<std::shared_ptr<PlaylistNode>> &getChildren() const把Vector拿到
// 2. 遍历PlaylistNode *getCurrentDirectory()里面的Vecter
// 3. 把Vector里面的数据，放到*QList<MusicItem>里面
// 4.
void MusicListModel::loadInitialData()
{
    QList<MusicItem> initialData = {
        {"これくらいで", "蓝月なくる", QUrl("qrc:/cover1.png"), false},
        {"夢で逢いましょう", "SARD UNDERGROUND", QUrl("qrc:/cover2.jpg"), true}, // isPlaying: true
        {"いのちの名前 (Cover...)", "Akie秋绘", QUrl("qrc:/cover3.png"), false},
        {"君に最後のログづけを", "ま.ゴ娘", QUrl("qrc:/cover4.png"), false},
        {"Fotizo", "nayuta/ARForest", QUrl("qrc:/cover5.png"), false},
        {"My Sweet Maiden", "Mia REGINA", QUrl("qrc:/cover6.png"), false}};
    // int cnt;
    // for (//vector)
    // {
    //     MusicItem item;
    //     vector->item; // 从vector里面拿到的PlaylistNode
    //                   //
    //     if(//先判断是不是一个文件夹){
    //         //是 的话,把他 artist写成Dir

    //         //否 的话,就不管了
    //     }else{
    //         map<*MusicItem, PlaylistNode>;
    //         initialData.push_back(std::move(item));
    //         map[&initialData[cnt]] = &vector->metaData;
    //     }
    // }
    // 后面还需要再写一个clean函数把这里的 map 还有musicList里面的数据都清空
    beginInsertRows(QModelIndex(), 0, initialData.count() - 1);
    m_musicList.append(initialData);
    endInsertRows();
}