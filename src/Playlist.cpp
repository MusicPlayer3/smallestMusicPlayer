#include "Playlist.hpp"
#include <system_error>

Playlist::Playlist(const std::string &rootDir) : rootDir(rootDir)
{
    std::error_code ec;
    fs::path canonicalRoot = fs::weakly_canonical(rootDir, ec);
    if (ec)//若出错则使用原给出路径
    {
        canonicalRoot = fs::path(rootDir);
    }

    // 根节点名用目录最后一节，比如 "Music"
    root = PlaylistNode(canonicalRoot.filename().string(),
                        canonicalRoot.string());
}

Playlist Playlist::fromFlatList(const std::string &rootDir,
                                const std::vector<MetaData> &items)
{
    Playlist playlist(rootDir);
    PlaylistNode &rootNode = playlist.getRoot();

    std::error_code ec;
    fs::path rootPath = fs::weakly_canonical(rootDir, ec);
    if (ec) // 若出错则使用原给出路径
    {
        rootPath = fs::path(rootDir);
    }

    for (const auto &meta : items)
    {
        fs::path filePath(meta.getFilePath());
        fs::path fileDir = filePath.parent_path();

        // 计算 fileDir 相对于 rootPath 的相对路径
        std::error_code ecRel;
        fs::path relDir = fs::relative(fileDir, rootPath, ecRel);

        PlaylistNode *current = &rootNode;

        if (!ecRel && !relDir.empty())
        {
            fs::path cumulative = rootPath;

            for (const auto &part : relDir)
            {
                cumulative /= part;
                current = &current->getOrCreateChild(part.string(),
                                                     cumulative.string());
            }
        }

        // 把当前文件挂到最终目录节点的 tracks 下
        current->addTrack(meta);
    }

    return playlist;
}
