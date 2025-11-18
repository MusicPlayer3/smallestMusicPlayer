#ifndef PLAYLIST_HPP
#define PLAYLIST_HPP

#include "MetaData.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// 播放列表节点：对应一个目录，下面挂子目录 + 当前目录中的音频
class PlaylistNode
{
private:
    std::string name;                                    // 目录名（根节点可以是根目录名）
    std::string path;                                    // 完整路径（从根开始）
    std::vector<MetaData> tracks;                        // 这个目录下的音频文件
    std::vector<std::unique_ptr<PlaylistNode>> children; // 这个目录下的子目录

public:
    PlaylistNode(const std::string &name = std::string(),
                 const std::string &path = std::string()) : name(name), path(path)
    {
    }

    // 在已有 children 中查找指定目录名的子节点，没有就新建
    PlaylistNode &getOrCreateChild(const std::string &dirName,
                                   const std::string &dirPath)
    {
        for (auto &child : children)
        {
            if (child->name == dirName)
            {
                return *child;
            }
        }
        children.emplace_back(std::make_unique<PlaylistNode>(dirName, dirPath));
        return *children.back();
    }

    void addTrack(const MetaData &meta)
    {
        tracks.push_back(meta);
    }

    // getters
    const std::string &getName() const
    {
        return name;
    }
    const std::string &getPath() const
    {
        return path;
    }
    const std::vector<MetaData> &getTracks() const
    {
        return tracks;
    }
    const std::vector<std::unique_ptr<PlaylistNode>> &getChildren() const
    {
        return children;
    }
};

// 整个播放列表：根节点 + 构建函数
class Playlist
{
private:
    PlaylistNode root;
    std::string rootDir;

public:
    explicit Playlist(const std::string &rootDir);

    PlaylistNode &getRoot()
    {
        return root;
    }
    const PlaylistNode &getRoot() const
    {
        return root;
    }
    const std::string &getRootDir() const
    {
        return rootDir;
    }

    // 从一维的 MetaData 列表构建树形播放列表
    static Playlist fromFlatList(const std::string &rootDir,
                                 const std::vector<MetaData> &items);
};

#endif // PLAYLIST_HPP
