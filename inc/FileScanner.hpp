#ifndef _FILE_SCANNER_HPP_
#define _FILE_SCANNER_HPP_

#include "MetaData.hpp"

class FileScanner
{
private:
    std::string rootDir;
    std::vector<MetaData> items;

    std::thread scanThread;

    std::atomic<bool> hasScanCpld{false};

    void scanDir();

public:
    FileScanner(std::string rootDir) :
        rootDir(rootDir)
    {
    }

    static MetaData getMetaData(const std::string &musicPath);

    void startScan()
    {
        scanThread = std::thread(&FileScanner::scanDir, this);
        scanThread.detach();
    }

    bool isScanCompleted() const
    {
        return hasScanCpld.load();
    }

    const std::vector<MetaData> &getItems() const
    {
        if (hasScanCpld.load())
        {
            return items;
        }
        else
        {
            static const std::vector<MetaData> empty;
            return empty;
        }
    }
};

#endif