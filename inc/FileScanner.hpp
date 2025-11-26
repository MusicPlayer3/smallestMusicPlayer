#ifndef _FILE_SCANNER_HPP_
#define _FILE_SCANNER_HPP_

#include "MetaData.hpp"
#include "PlaylistNode.hpp"

class FileScanner
{
private:
    std::string rootDir;

    std::thread scanThread;

    std::shared_ptr<PlaylistNode> rootNode;

    std::atomic<bool> hasScanCpld{false};

    void scanDir();

public:
    FileScanner(std::string rootDir) :
        rootDir(rootDir)
    {
    }
    FileScanner() = default;

    void setRootDir(const std::string &rootDir)
    {
        this->rootDir = rootDir;
    }

    const std::string getRootDir() const
    {
        return rootDir;
    }

    static MetaData getMetaData(const std::string &musicPath);

    void startScan() // 开始扫描
    {
        scanThread = std::thread(&FileScanner::scanDir, this);
        scanThread.detach();
    }

    bool isScanCompleted() const // 是否遍历完成
    {
        return hasScanCpld.load();
    }

    std::shared_ptr<PlaylistNode> getPlaylistTree() const // 构建播放列表
    {
        return rootNode;
    }
};

#endif