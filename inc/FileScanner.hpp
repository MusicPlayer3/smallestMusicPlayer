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
    FileScanner(std::string rootDir) : rootDir(rootDir)
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
    static void extractCoverToTempFile(const std::string &musicPath, MetaData &data);

    void startScan()
    {
        scanThread = std::thread(&FileScanner::scanDir, this);
        scanThread.detach();
    }

    bool isScanCompleted() const
    {
        return hasScanCpld.load();
    }

    std::shared_ptr<PlaylistNode> getPlaylistTree() const
    {
        return rootNode;
    }

    // [新增] 初始化支持的音频后缀列表 (通常自动调用，也可手动调用)
    static void initSupportedExtensions();
};

#endif