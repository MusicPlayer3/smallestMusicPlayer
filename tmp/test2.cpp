#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>

#include "FileScanner.hpp"

// 树形打印函数，全塞在这个文件里
void printTree(const std::shared_ptr<PlaylistNode> &node,
               const std::string &prefix = "",
               bool isLast = true)
{
    if (!node)
        return;

    // 打印前缀
    std::cout << prefix;
    std::cout << (isLast ? "└── " : "├── ");

    // 只打印文件名，不打整条路径
    std::filesystem::path p(node->getPath());
    std::string name = p.filename().string();

    // 如果根节点就是目录本身，也可以特殊处理一下
    if (name.empty())
        name = node->getPath();

    std::cout << name;
    if (node->getIsDir())
        std::cout << "/";

    std::cout << "\n";

    // 递归打印子节点
    const auto &children = node->getChildren();
    std::string newPrefix = prefix + (isLast ? "    " : "│   ");

    for (size_t i = 0; i < children.size(); ++i)
    {
        bool childIsLast = (i + 1 == children.size());
        printTree(children[i], newPrefix, childIsLast);
    }
}

int main()
{
    // 换成你真实存在的目录
    std::string root = "/home/WhiteBread/Music";

    FileScanner scanner(root);
    scanner.startScan();

    // 等扫描结束
    while (!scanner.isScanCompleted())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    auto rootNode = scanner.buildPlaylistTree();
    if (!rootNode)
    {
        std::cerr << "rootNode is null\n";
        return 1;
    }

    std::cout << "Root: " << rootNode->getPath() << "\n\n";
    printTree(rootNode);

    return 0;
}
