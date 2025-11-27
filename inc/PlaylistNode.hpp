#ifndef PLAYLISTNODE_HPP
#define PLAYLISTNODE_HPP

#include "MetaData.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// 播放列表节点：对应一个目录，下面挂子目录和子音频文件
class PlaylistNode
{
private:
    bool _isDir;                                         // 是否是目录
    std::string path;                                    // 完整路径（从根开始）
    MetaData metaData;                                   // 音频文件元数据(如果不是目录的话)
    std::vector<std::shared_ptr<PlaylistNode>> children; // 这个目录下的子目录+音频文件
    std::weak_ptr<PlaylistNode> parent;                  // 父节点   (如果是根节点则为空)
public:
    PlaylistNode(const std::string &path = std::string(), bool isDir = false) : _isDir(isDir), path(path)
    {
    }
    const bool &isDir() const
    {
        return _isDir;
    }
    const std::string &getPath() const
    {
        return path;
    }
    const std::vector<std::shared_ptr<PlaylistNode>> &getChildren() const
    {
        return children;
    }
    const MetaData &getMetaData() const
    {
        return metaData;
    }
    void setIsDir(const bool &isDir)
    {
        this->_isDir = isDir;
    }
    void setMetaData(const MetaData &metaData)
    {
        this->metaData = metaData;
    }
    void addChild(const std::shared_ptr<PlaylistNode> &child)
    {
        children.push_back(child);
    }
    void setParent(const std::shared_ptr<PlaylistNode> &parent)
    {
        this->parent = parent;
    }
    std::shared_ptr<PlaylistNode> getParent()
    {
        return parent.lock();
    }
};

// 整个播放列表：根节点 + 构建函数
// class Playlist
// {
// private:
//     PlaylistNode root;
//     std::string rootDir;

// public:
//     explicit Playlist(const std::string &rootDir);

//     PlaylistNode &getRoot()
//     {
//         return root;
//     }
//     const PlaylistNode &getRoot() const
//     {
//         return root;
//     }
//     const std::string &getRootDir() const
//     {
//         return rootDir;
//     }

// };

#endif // PLAYLISTNODE_HPP
