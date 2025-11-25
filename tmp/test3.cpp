#include "Cover.hpp"
#include "FileScanner.hpp"
#include "Playlist.hpp"
#include <iostream>
#include <filesystem>

#include "stb_image_write.h"

void printTree(const std::shared_ptr<PlaylistNode> &node, int depth = 0)
{
    if (!node)
        return;

    for (int i = 0; i < depth; ++i)
        std::cout << "  ";

    if (node->getIsDir())
        std::cout << "[DIR]  " << node->getPath() << "\n";
    else
        std::cout << "[FILE] " << node->getPath() << "\n";

    for (auto &child : node->getChildren())
        printTree(child, depth + 1);
}

void run_cover_test()
{
    // 扫描路径
    std::string root = "/home/WhiteBread/Music";
    FileScanner scanner(root);
    scanner.startScan();

    // 等待扫描结束
    while (!scanner.isScanCompleted())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 输出树结构
    auto rootNode = scanner.buildPlaylistTree();
    printTree(rootNode);

    // 输出所有封面缩略图到 tmp/test_covers
    auto &cache = CoverCache::instance().covercache;
    std::filesystem::create_directories("./tmp/test_covers");

    for (auto &kv : cache)
    {
        const std::string &album = kv.first;
        auto img = kv.second;

        if (!img)
            continue;

        std::string out = "./tmp/test_covers/" + album + ".png";

        stbi_write_png(
            out.c_str(),
            img->width,
            img->height,
            img->channels,
            img->pixels.data(),
            img->width * img->channels);

        std::cout << "Wrote " << out << "\n";
    }
}

int main()
{
    run_cover_test();
    return 0;
}
