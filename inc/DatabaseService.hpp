#ifndef DATABASESERVICE_HPP
#define DATABASESERVICE_HPP

#include "PCH.h"
#include "MetaData.hpp"
#include "PlaylistNode.hpp"

#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>
#include <mutex>
#include <vector>
#include <string>
#include <filesystem>
#include <memory>
#include <unordered_set>

/**
 * @class DatabaseService
 * @brief 数据库核心服务类 (单例模式)
 */
class DatabaseService
{
public:
    static DatabaseService &instance();

    // ==========================================
    // 1. 连接与初始化
    // ==========================================

    bool connect(const std::string &host, int port,
                 const std::string &user, const std::string &password,
                 const std::string &dbName);

    bool isPopulated();

    // ==========================================
    // 2. 核心：整树存取 (Tree Persistence)
    // ==========================================

    // [优化] 采用了内存扁平化 + 批量插入 + 线程池并发写入
    void saveFullTree(const std::shared_ptr<PlaylistNode> &root);

    std::shared_ptr<PlaylistNode> loadFullTree();

    // ==========================================
    // 3. 封面数据服务
    // ==========================================

    std::vector<uint8_t> getCoverBlob(const std::string &cacheKey);
    void saveCoverBlob(const std::string &key, const std::vector<uint8_t> &pngData);

    // ==========================================
    // 4. 业务功能 (CRUD & Advanced Features)
    // ==========================================

    bool recordPlay(const std::string &filePath);
    std::vector<MetaData> searchSongs(const std::string &keyword);
    bool updateRating(const std::string &filePath, int rating);

    int getPlayCount(const std::string &filePath);
    int getRating(const std::string &filePath);

    bool addSong(const MetaData &meta, const std::string &coverKey);
    bool addDirectory(const std::string &path, const std::string &name,
                      const std::string &parentPath, const std::string &coverKey);
    bool removeSong(const std::string &filePath);
    bool removeDirectory(const std::string &dirPath);

private:
    DatabaseService() = default;
    ~DatabaseService();
    DatabaseService(const DatabaseService &) = delete;
    DatabaseService &operator=(const DatabaseService &) = delete;

    QSqlDatabase m_db;
    std::mutex m_mutex;

    // 保存连接参数，供多线程 Worker 使用
    struct ConnectionParams
    {
        QString host;
        int port;
        QString user;
        QString password;
        QString dbName;
    } m_connParams;

    // --- 内部辅助函数 ---

    bool initSchema();
    int getDirectoryId(const std::string &fullPath);

    // 检查并保存封面 (优化版：可传入已存在集合)
    void checkAndSaveCover(const std::string &key, std::unordered_set<std::string> *existingKeys = nullptr);

    void cleanupOrphanedCovers();

    static int64_t timeToDb(const std::filesystem::file_time_type &t);
    static std::filesystem::file_time_type dbToTime(int64_t t);
};

#endif // DATABASESERVICE_HPP