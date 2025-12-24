#ifndef _FILE_SCANNER_HPP_
#define _FILE_SCANNER_HPP_

#include "MetaData.hpp"
#include "PlaylistNode.hpp"
#include "PCH.h"

/**
 * @class FileScanner
 * @brief 文件扫描器
 *
 * 负责扫描本地文件系统，解析音频文件元数据（TagLib + FFmpeg），
 * 处理 CUE 分轨文件，并构建播放列表树。
 * 使用 C++20 std::jthread 进行线程生命周期管理。
 */
class FileScanner
{
private:
    std::string rootDir;

    // C++20: 使用 jthread 自动处理线程汇合（join），避免析构时崩溃
    std::jthread scanThread;

    std::shared_ptr<PlaylistNode> rootNode;

    // 扫描完成标志
    std::atomic<bool> hasScanCpld{false};

    /**
     * @brief 后台扫描执行函数
     * @param stoken C++20 停止令牌，用于协作式中断扫描
     */
    void scanDir(std::stop_token stoken);

    std::function<void(std::shared_ptr<PlaylistNode>)> m_callback;

public:
    /**
     * @brief 构造函数
     * @param rootDir 扫描的根目录路径
     */
    FileScanner(std::string rootDir);

    /**
     * @brief 默认构造函数
     */
    FileScanner() = default;

    /**
     * @brief 析构函数
     * jthread 会自动请求停止并等待线程结束，因此使用默认析构即可安全退出。
     */
    ~FileScanner() = default;

    /**
     * @brief 设置扫描根目录
     * @param rootDir 路径字符串
     */
    void setRootDir(const std::string &rootDir);

    /**
     * @brief 获取扫描根目录
     * @return 路径字符串
     */
    const std::string getRootDir() const;

    // --- 静态工具方法 ---

    /**
     * @brief 获取指定音频文件的元数据
     * 结合了 TagLib (标签) 和 FFmpeg (技术参数)
     * @param musicPath 文件绝对路径
     * @return MetaData 对象
     */
    static MetaData getMetaData(const std::string &musicPath);

    /**
     * @brief 从元数据中提取封面图片到临时文件夹
     * @param metadata 元数据对象（如果成功提取，会更新其中的 coverPath）
     * @return 提取出的临时文件路径，失败返回空字符串
     */
    static std::string extractCoverToTempFile(MetaData &metadata);

    /**
     * @brief 启动异步扫描任务
     * 如果已有任务在运行，会先中断并等待其结束，再启动新任务。
     */
    void startScan();

    /**
     * @brief 手动停止当前扫描任务
     */
    void stopScan();

    /**
     * @brief 检查扫描是否已完成
     * @return true 已完成, false 未完成或正在进行
     */
    bool isScanCompleted() const;

    /**
     * @brief 获取构建好的播放列表树根节点
     * @return 根节点智能指针
     */
    std::shared_ptr<PlaylistNode> getPlaylistTree() const;

    /**
     * @brief 初始化支持的音频扩展名列表
     * 线程安全，利用 std::call_once 实现
     */
    static void initSupportedExtensions();

    /**
     * @brief 友元函数：打印播放列表树结构 (调试用)
     * @param root 树根节点
     */
    friend void printPlaylistTree(const std::shared_ptr<PlaylistNode> &root);

    /**
     * @brief 静态方法：同步扫描单个音频文件并构建节点
     * @param path 文件绝对路径
     * @return 构建好的节点，如果文件无效返回 nullptr
     */
    static std::shared_ptr<PlaylistNode> scanFile(const std::string &path);

    /**
     * @brief 静态方法：同步递归扫描文件夹并构建树
     * @param path 文件夹绝对路径
     * @return 构建好的文件夹节点（包含统计数据），路径无效返回 nullptr
     */
    static std::shared_ptr<PlaylistNode> scanDirectory(const std::string &path);

    void setScanFinishedCallback(std::function<void(std::shared_ptr<PlaylistNode>)> cb);
};

#endif