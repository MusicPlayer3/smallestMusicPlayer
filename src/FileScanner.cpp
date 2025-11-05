#include"FileScanner.hpp"
#include "MetaData.hpp"
#include <filesystem>
#include <unordered_set>
#include <taglib/tag.h>
#include <taglib/fileref.h>
#include <taglib/tstring.h>
#include <vector>
#include<stack>
namespace fs = std::filesystem;
bool isAudioFile(const std::string &route) //初步确定文件是否为音频文件
{
    fs::path r(route);
    std::string ext = r.extension().string();
    if (ext.empty())
        return false;
    static const std::unordered_set<std::string> audio_extensions = {
        ".mp3", ".wav", ".flac", ".aac", ".ogg", ".m4a", ".wma", ".aiff"};
    for (auto &i : ext)
    {
        i = (char)std::tolower(i);
    }
    return audio_extensions.count(ext) > 0;
}
void getinfo(const std::string &route,std::vector<MetaData>&items) //读取并存储该路径下的音频文件信息
{
    fs::path r(route);
    MetaData music;
    TagLib::FileRef f(route.c_str());

    if (f.isNull() ||f.tag() == nullptr)
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
void FileScanner::scanDir(){ //扫描路径并获取路径下所有音频文件信息
    if(!fs::exists(rootDir))
    {
        hasScanCpld=true;return;
    }
    if (!(fs::is_directory(rootDir)))
    {
        if (isAudioFile(rootDir))
        {
            getinfo(rootDir,items);
        }
        hasScanCpld=true;return;
    }

    std::stack<std::string> dirStack;
    dirStack.push(rootDir);
    while (!dirStack.empty())//bfs层次遍历文件夹或读入音频文件信息
    {
        std::string currentDir = dirStack.top();
        dirStack.pop();
        for(const auto &entry : fs::directory_iterator(currentDir))
        {
            if (entry.is_directory())
            {
                dirStack.push(entry.path().string());
            }
            else if (isAudioFile(entry.path().string()))
            {
                getinfo(entry.path().string(),items);
            }
        }
    }
    hasScanCpld=true;return;
}