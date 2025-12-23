#ifndef PLAYLISTNODE_HPP
#define PLAYLISTNODE_HPP

#include "MetaData.hpp"
#include "PCH.h"

// 播放列表节点：对应一个目录，下面挂子目录和子音频文件
class PlaylistNode : public std::enable_shared_from_this<PlaylistNode>
{
private:
    bool _isDir;                                         // 是否是目录
    std::string path;                                    // 完整路径（从根开始）
    std::string _coverKey;                               // 文件夹封面的缓存Key（通常为文件夹路径）
    MetaData metaData;                                   // 音频文件元数据 (对于目录节点，利用其中的 coverPath 存储扫描到的封面路径)
    std::vector<std::shared_ptr<PlaylistNode>> children; // 这个目录下的子目录+音频文件
    std::weak_ptr<PlaylistNode> parent;                  // 父节点 (如果是根节点则为空)
    std::uint64_t totalSongs = 0;                        // 总歌曲数
    std::uint64_t totalDuration = 0;                     // 总时长(单位为秒)

public:
    PlaylistNode(const std::string &path = std::string(), bool isDir = false) : _isDir(isDir), path(path)
    {
    }

    // 允许外部传入比较器对子节点进行重新排序
    void reorderChildren(std::function<bool(const std::shared_ptr<PlaylistNode> &, const std::shared_ptr<PlaylistNode> &)> comparator)
    {
        std::sort(children.begin(), children.end(), comparator);
    }

    // 默认排序逻辑：目录在前，然后按路径名称排序；如果是同目录下的文件，按音轨偏移量排序
    void sortChildren()
    {
        std::sort(children.begin(), children.end(), [](const std::shared_ptr<PlaylistNode> &a, const std::shared_ptr<PlaylistNode> &b)
                  {
            if (a->isDir() != b->isDir()) return a->_isDir; // 目录在前
            if (a->path != b->path) return a->path < b->path;
            return a->metaData.getOffset() < b->metaData.getOffset(); });
    }

    const bool isDir() const
    {
        return _isDir;
    }

    std::string getPath() const
    {
        return path;
    }

    // 获取当前文件夹封面的Key (兼容旧接口命名)
    const std::string getThisDirCover() const
    {
        return _coverKey;
    }

    const std::vector<std::shared_ptr<PlaylistNode>> &getChildren() const
    {
        return children;
    }

    const MetaData &getMetaData() const
    {
        return metaData;
    }

    void setIsDir(const bool isDir)
    {
        this->_isDir = isDir;
    }

    // 获取用于 CoverCache 查找的 Key
    const std::string getCoverKey() const
    {
        return _coverKey;
    }

    // 设置文件夹封面的Key
    void setCoverKey(const std::string &key)
    {
        this->_coverKey = key;
    }

    void setMetaData(const MetaData &metaData)
    {
        this->metaData = metaData;
    }

    // [优化] 快捷访问/设置封面路径 (直接操作 MetaData)
    // 用于 FileScanner 在扫描目录时直接把发现的 cover.jpg 存入节点，避免后续再次 IO
    void setCoverPath(const std::string &path)
    {
        this->metaData.setCoverPath(path);
    }

    /**
     * @brief 从子节点列表中移除指定指针的节点
     * 使用 std::erase_if (C++20) 或 remove_if 移除 shared_ptr
     */
    void removeChild(PlaylistNode *childPtr)
    {
        if (!childPtr)
            return;
        // 移除所有原始指针匹配的 shared_ptr
        std::erase_if(children, [childPtr](const std::shared_ptr<PlaylistNode> &ptr)
                      { return ptr.get() == childPtr; });
    }

    const std::string getCoverPath() const
    {
        return this->metaData.getCoverPath();
    }

    void addChild(const std::shared_ptr<PlaylistNode> &child)
    {
        children.push_back(child);
        child->setParent(shared_from_this()); // 自动认亲
    }

    void setParent(const std::shared_ptr<PlaylistNode> &parent)
    {
        this->parent = parent;
    }

    std::shared_ptr<PlaylistNode> getParent()
    {
        return parent.lock();
    }

    void setTotalSongs(const std::uint64_t &totalSongs)
    {
        this->totalSongs = totalSongs;
    }

    void setTotalDuration(const std::uint64_t &totalDuration)
    {
        this->totalDuration = totalDuration;
    }

    std::uint64_t getTotalSongs() const
    {
        return totalSongs;
    }

    std::uint64_t getTotalDuration() const
    {
        return totalDuration;
    }
};

#endif // PLAYLISTNODE_HPP