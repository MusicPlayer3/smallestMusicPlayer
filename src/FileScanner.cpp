#include "FileScanner.hpp"
#include "MetaData.hpp"
#include "Precompiled.h"

namespace fs = std::filesystem;
bool isffmpeg(const std::string &route)
{
    AVFormatContext *fmtCtx = nullptr;
    // avformat_open_input 返回 0 表示成功
    if (avformat_open_input(&fmtCtx, route.c_str(), nullptr, nullptr) < 0)
    {
        return false;
    }
    avformat_close_input(&fmtCtx);
    return true;
}
void getinfo(const std::string &route, std::vector<MetaData> &items) // 读取并存储该路径下的音频文件信息
{
    fs::path r(route);
    MetaData music;
    TagLib::FileRef f(route.c_str());

    if (f.isNull() || f.tag() == nullptr)
    {
        return;
    }
    TagLib::Tag *tag = f.tag();
    music.setFilePath(route);
    music.setParentDir(r.parent_path().string());
    music.setTitle(tag->title().toCString());
    music.setArtist(tag->artist().toCString());
    music.setAlbum(tag->album().toCString());
    music.setYear(tag->year() > 0 ? std::to_string(tag->year()) : "");
    items.push_back(music);
}
void FileScanner::scanDir()
{ // 扫描路径并获取路径下所有音频文件信息
    if (!fs::exists(rootDir))
    {
        hasScanCpld = true;
        return;
    }
    if (!(fs::is_directory(rootDir)))
    {
        if (isffmpeg(rootDir))
        {
            getinfo(rootDir, items);
        }
        hasScanCpld = true;
        return;
    }

    std::stack<std::string> dirStack;
    dirStack.push(rootDir);
    while (!dirStack.empty()) // dfs层次遍历文件夹或读入音频文件信息
    {
        std::string currentDir = dirStack.top();
        dirStack.pop();
        for (const auto &entry : fs::directory_iterator(currentDir))
        {
            if (entry.is_directory())
            {
                dirStack.push(entry.path().string());
            }
            else if (isffmpeg(entry.path().string()))
            {
                getinfo(entry.path().string(), items);
            }
        }
    }
    hasScanCpld = true;
    return;
}