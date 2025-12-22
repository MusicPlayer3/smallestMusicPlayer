#include "DatabaseService.hpp"
#include "CoverCache.hpp"
#include "FileScanner.hpp"
#include "CoverImage.hpp"
#include "SimpleThreadPool.hpp" // 引入线程池

// OpenCV 用于图片编解码 (PNG Compression)
#include <opencv2/opencv.hpp>

// Qt & Utils
#include <QVariant>
#include <QVariantList> // 批量插入必备
#include <QUuid>        // 生成唯一连接名
#include <QDebug>
#include <spdlog/spdlog.h>
#include <future> // std::future
#include <unordered_set>

#define TO_QSTR(s) QString::fromStdString(s)
#define TO_STD(s) (s).toStdString()

namespace fs = std::filesystem;

// ==========================================
//  内部数据结构 (用于扁平化存储)
// ==========================================
struct DirData
{
    int id;
    int parentId;
    QString name;
    QString fullPath;
    QString coverKey;
};

struct SongData
{
    int dirId;
    QString title;
    QString artist;
    QString album;
    QString year;
    QString filePath;
    qint64 duration;
    qint64 offset;
    qint64 lastWriteTime;
    std::uint32_t sampleRate;
    std::uint16_t bitDepth;
    QString format;
};

// ==========================================
//  静态工具与单例
// ==========================================

DatabaseService &DatabaseService::instance()
{
    static DatabaseService s;
    return s;
}

DatabaseService::~DatabaseService()
{
    if (m_db.isOpen())
        m_db.close();
}

int64_t DatabaseService::timeToDb(const fs::file_time_type &t)
{
    return t.time_since_epoch().count();
}

fs::file_time_type DatabaseService::dbToTime(int64_t t)
{
    using namespace std::chrono;
    return fs::file_time_type(fs::file_time_type::duration(t));
}

// ==========================================
//  连接与初始化
// ==========================================

bool DatabaseService::connect(const std::string &host, int port,
                              const std::string &user, const std::string &password,
                              const std::string &dbName)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 保存连接参数供线程池使用
    m_connParams.host = TO_QSTR(host);
    m_connParams.port = port;
    m_connParams.user = TO_QSTR(user);
    m_connParams.password = TO_QSTR(password);
    m_connParams.dbName = TO_QSTR(dbName);

    if (!QSqlDatabase::isDriverAvailable("QMYSQL"))
    {
        spdlog::critical("[DB] QMYSQL 驱动在当前系统中不可用！请检查是否安装了 mariadb-libs / libmysqlclient");
        return false;
    }

    // 1. 加载驱动
    if (QSqlDatabase::contains("qt_sql_default_connection"))
        m_db = QSqlDatabase::database("qt_sql_default_connection");
    else
        m_db = QSqlDatabase::addDatabase("QMYSQL");

    m_db.setHostName(m_connParams.host);
    m_db.setPort(m_connParams.port);
    m_db.setUserName(m_connParams.user);
    m_db.setPassword(m_connParams.password);
    m_db.setConnectOptions("MYSQL_OPT_RECONNECT=1");

    // 2. 尝试连接目标数据库
    m_db.setDatabaseName(m_connParams.dbName);

    if (!m_db.open())
    {
        QSqlError err = m_db.lastError();
        // Error 1049: Unknown database -> 需要创建
        if (err.nativeErrorCode() == "1049" || err.text().contains("Unknown database"))
        {
            spdlog::info("[DB] Database '{}' not found. Creating...", dbName);

            // 临时连接到 Server
            m_db.setDatabaseName("");
            if (!m_db.open())
            {
                spdlog::error("[DB] Failed to connect to server: {}", m_db.lastError().text().toStdString());
                return false;
            }

            // 创建数据库: 必须使用 utf8mb4_bin 以支持 C++ string 的二进制严格匹配
            QSqlQuery q(m_db);
            QString createSql = QString("CREATE DATABASE IF NOT EXISTS %1 CHARACTER SET utf8mb4 COLLATE utf8mb4_bin").arg(m_connParams.dbName);
            if (!q.exec(createSql))
            {
                spdlog::error("[DB] Create DB failed: {}", q.lastError().text().toStdString());
                return false;
            }

            // 重连到新库
            m_db.close();
            m_db.setDatabaseName(m_connParams.dbName);
            if (!m_db.open())
            {
                spdlog::error("[DB] Failed to open newly created database.");
                return false;
            }
            spdlog::info("[DB] Database created successfully.");
        }
        else
        {
            spdlog::error("[DB] Connection failed: {}", err.text().toStdString());
            return false;
        }
    }

    // 3. 强制设置会话字符集
    QSqlQuery q(m_db);
    q.exec("SET NAMES 'utf8mb4' COLLATE 'utf8mb4_bin'");

    // 4. 初始化所有数据库对象
    return initSchema();
}

bool DatabaseService::initSchema()
{
    QSqlQuery q(m_db);
    bool ok = true;

    // --- 1. 创建表 (Tables) ---

    // 目录表
    ok &= q.exec(R"(
        CREATE TABLE IF NOT EXISTS table_directories (
            id INT AUTO_INCREMENT PRIMARY KEY,
            parent_id INT DEFAULT NULL,
            name VARCHAR(255) NOT NULL,
            full_path VARCHAR(768) NOT NULL, 
            cover_key VARCHAR(255) DEFAULT NULL,
            CONSTRAINT fk_dir_parent FOREIGN KEY (parent_id) REFERENCES table_directories(id) ON DELETE CASCADE,
            UNIQUE INDEX idx_path (full_path),
            INDEX idx_cover_key (cover_key)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
    )");

    // 歌曲表
    // [Fixed] 修改 UNIQUE 索引：使用 (file_path, offset_val) 组合约束
    // 解决了 CUE 分轨文件物理路径相同导致无法插入多个音轨的问题
    ok &= q.exec(R"(
        CREATE TABLE IF NOT EXISTS table_songs (
            id INT AUTO_INCREMENT PRIMARY KEY,
            directory_id INT NOT NULL,
            title VARCHAR(255),
            artist VARCHAR(255),
            album VARCHAR(255),
            year VARCHAR(32),
            file_path VARCHAR(768) NOT NULL,
            duration BIGINT DEFAULT 0,
            offset_val BIGINT DEFAULT 0,
            last_write_time BIGINT DEFAULT 0,
            sample_rate INT UNSIGNED DEFAULT 0,
            bit_depth SMALLINT UNSIGNED DEFAULT 0,
            format_type VARCHAR(32),
            play_count INT DEFAULT 0,
            rating TINYINT DEFAULT 0,
            last_played_at BIGINT DEFAULT 0,
            CONSTRAINT fk_song_dir FOREIGN KEY (directory_id) REFERENCES table_directories(id) ON DELETE CASCADE,
            UNIQUE INDEX idx_file_path_offset (file_path, offset_val),
            INDEX idx_album (album),
            INDEX idx_artist (artist),
            INDEX idx_title (title)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
    )");

    // 封面表
    ok &= q.exec(R"(
        CREATE TABLE IF NOT EXISTS table_covers (
            cache_key VARCHAR(255) NOT NULL PRIMARY KEY,
            thumbnail_data MEDIUMBLOB NOT NULL,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
    )");

    if (!ok)
    {
        spdlog::error("[DB] Table creation failed: {}", q.lastError().text().toStdString());
        return false;
    }

    // --- 2. 创建视图 (View) ---
    q.exec("DROP VIEW IF EXISTS view_library_search");
    ok = q.exec(R"(
        CREATE VIEW view_library_search AS
        SELECT 
            s.id, s.title, s.artist, s.album, s.file_path, s.duration, s.play_count, s.rating,
            d.full_path AS dir_path, d.name AS dir_name
        FROM table_songs s
        JOIN table_directories d ON s.directory_id = d.id
    )");

    if (!ok)
        spdlog::error("[DB] View creation failed: {}", q.lastError().text().toStdString());

    // --- 3. 创建触发器 (Trigger) ---
    q.exec("DROP TRIGGER IF EXISTS trg_validate_rating_insert");
    bool t1 = q.exec(R"(
        CREATE TRIGGER trg_validate_rating_insert BEFORE INSERT ON table_songs
        FOR EACH ROW
        BEGIN
            IF NEW.rating < 0 THEN SET NEW.rating = 0; END IF;
            IF NEW.rating > 5 THEN SET NEW.rating = 5; END IF;
        END
    )");

    q.exec("DROP TRIGGER IF EXISTS trg_validate_rating_update");
    bool t2 = q.exec(R"(
        CREATE TRIGGER trg_validate_rating_update BEFORE UPDATE ON table_songs
        FOR EACH ROW
        BEGIN
            IF NEW.rating < 0 THEN SET NEW.rating = 0; END IF;
            IF NEW.rating > 5 THEN SET NEW.rating = 5; END IF;
        END
    )");

    if (!t1 || !t2)
        spdlog::error("[DB] Trigger creation failed.");

    // --- 4. 创建存储过程 (Stored Procedure) ---
    q.exec("DROP PROCEDURE IF EXISTS sp_record_play");
    ok = q.exec(R"(
        CREATE PROCEDURE sp_record_play(IN target_path VARCHAR(768), IN current_time_tick BIGINT)
        BEGIN
            UPDATE table_songs 
            SET play_count = play_count + 1, last_played_at = current_time_tick
            WHERE file_path = target_path;
        END
    )");

    if (!ok)
        spdlog::error("[DB] Procedure creation failed: {}", q.lastError().text().toStdString());

    return true;
}

bool DatabaseService::isPopulated()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db.isOpen())
        return false;
    QSqlQuery q("SELECT 1 FROM table_songs LIMIT 1", m_db);
    return q.next();
}

// ==========================================
//  封面数据服务 (核心修复部分)
// ==========================================

void DatabaseService::saveCoverBlob(const std::string &key, const std::vector<uint8_t> &pngData)
{
    if (key.empty() || pngData.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // 使用 INSERT IGNORE，如果 Key 已存在则跳过
    QSqlQuery q(m_db);
    q.prepare("INSERT IGNORE INTO table_covers (cache_key, thumbnail_data) VALUES (:k, :d)");
    q.bindValue(":k", TO_QSTR(key));
    q.bindValue(":d", QByteArray(reinterpret_cast<const char *>(pngData.data()), pngData.size()));

    if (!q.exec())
    {
        spdlog::warn("[DB] Immediate save cover failed: {}", q.lastError().text().toStdString());
    }
}

std::vector<uint8_t> DatabaseService::getCoverBlob(const std::string &cacheKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    q.prepare("SELECT thumbnail_data FROM table_covers WHERE cache_key = :k");
    q.bindValue(":k", TO_QSTR(cacheKey));
    if (q.exec() && q.next())
    {
        QByteArray ba = q.value(0).toByteArray();
        return std::vector<uint8_t>(ba.begin(), ba.end());
    }
    return {};
}

// 优化后的 checkAndSaveCover：支持传入 existingKeys 缓存，减少 SELECT 查询
void DatabaseService::checkAndSaveCover(const std::string &key, std::unordered_set<std::string> *existingKeys)
{
    if (key.empty() || key == "Unknown")
        return;

    // 1. 如果有缓存集合，先查集合 (极速)
    if (existingKeys)
    {
        if (existingKeys->find(key) != existingKeys->end())
        {
            return; // 数据库已有，跳过
        }
    }
    else
    {
        // 2. 无缓存集合时的回退逻辑
        QSqlQuery checkQ(m_db);
        checkQ.prepare("SELECT 1 FROM table_covers WHERE cache_key = :k");
        checkQ.bindValue(":k", TO_QSTR(key));
        if (checkQ.exec() && checkQ.next())
            return;
    }

    // 3. 内存里还有吗？
    // [关键修复] 使用 getRamOnly 避免死锁 (saveFullTree 持有 DB 锁 -> CoverCache -> getCoverBlob 尝试获取 DB 锁)
    // 因为如果在上面的第1/2步确认数据库没有，那么 CoverCache::get 去查数据库也是没用的，只会导致死锁。
    auto img = CoverCache::instance().getRamOnly(key);

    if (!img || !img->isValid())
        return;

    try
    {
        cv::Mat mat(img->height(), img->width(), CV_8UC4, (void *)img->data());
        std::vector<uchar> buf;
        std::vector<int> params = {cv::IMWRITE_PNG_COMPRESSION, 3};
        cv::imencode(".png", mat, buf, params);

        QSqlQuery insQ(m_db);
        insQ.prepare("INSERT IGNORE INTO table_covers (cache_key, thumbnail_data) VALUES (:k, :d)");
        insQ.bindValue(":k", TO_QSTR(key));
        insQ.bindValue(":d", QByteArray(reinterpret_cast<const char *>(buf.data()), buf.size()));
        insQ.exec();

        // [优化] 如果插入成功，且使用了 external set，记得同步更新 set，避免后续重复处理
        if (existingKeys)
        {
            existingKeys->insert(key);
        }
    }
    catch (...)
    {
    }
}

void DatabaseService::cleanupOrphanedCovers()
{
    spdlog::info("[DB] Cleaning orphaned covers...");
    QSqlQuery q(m_db);
    bool ok = q.exec(R"(
        DELETE FROM table_covers 
        WHERE cache_key NOT IN (
            SELECT DISTINCT album FROM table_songs WHERE album IS NOT NULL
            UNION 
            SELECT DISTINCT cover_key FROM table_directories WHERE cover_key IS NOT NULL
        )
    )");
    if (!ok)
        spdlog::error("[DB] Orphan cleanup error: {}", q.lastError().text().toStdString());
}

// ==========================================
//  整树存取 (核心逻辑 - 优化版)
// ==========================================

// 辅助函数：将 SongData 列表转换为 Qt execBatch 所需的列数据
static void bindAndExecBatchSongs(QSqlQuery &q, const std::vector<SongData> &songs)
{
    if (songs.empty())
        return;

    QVariantList dids, titles, artists, albums, years, paths,
        durs, offs, lwts, srs, bds, fmts;

    // 预分配以提升性能
    size_t sz = songs.size();
    dids.reserve(sz);
    titles.reserve(sz);
    artists.reserve(sz);
    albums.reserve(sz);
    years.reserve(sz);
    paths.reserve(sz);
    durs.reserve(sz);
    offs.reserve(sz);
    lwts.reserve(sz);
    srs.reserve(sz);
    bds.reserve(sz);
    fmts.reserve(sz);

    for (const auto &s : songs)
    {
        dids << s.dirId;
        titles << s.title;
        artists << s.artist;
        albums << s.album;
        years << s.year;
        paths << s.filePath;
        durs << s.duration;
        offs << s.offset;
        lwts << s.lastWriteTime;
        srs << s.sampleRate;
        bds << s.bitDepth;
        fmts << s.format;
    }

    q.addBindValue(dids);
    q.addBindValue(titles);
    q.addBindValue(artists);
    q.addBindValue(albums);
    q.addBindValue(years);
    q.addBindValue(paths);
    q.addBindValue(durs);
    q.addBindValue(offs);
    q.addBindValue(lwts);
    q.addBindValue(srs);
    q.addBindValue(bds);
    q.addBindValue(fmts);

    if (!q.execBatch())
    {
        spdlog::error("[DB Batch] Insert failed: {}", q.lastError().text().toStdString());
    }
}

void DatabaseService::saveFullTree(const std::shared_ptr<PlaylistNode> &root)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!root)
    {
        spdlog::error("[DB] Root node is null, cannot save.");
        return;
    }

    spdlog::info("[DB] Starting optimized saveFullTree...");
    auto startTotal = std::chrono::high_resolution_clock::now();

    // 1. 准备阶段：清空表 & 预加载封面 Key
    m_db.transaction();
    {
        QSqlQuery q(m_db);
        q.exec("SET FOREIGN_KEY_CHECKS = 0");
        q.exec("TRUNCATE TABLE table_songs");
        q.exec("TRUNCATE TABLE table_directories");
        q.exec("SET FOREIGN_KEY_CHECKS = 1");
    }
    m_db.commit();

    // 预加载所有现存封面 Key
    std::unordered_set<std::string> existingCoverKeys;
    {
        QSqlQuery q("SELECT cache_key FROM table_covers", m_db);
        while (q.next())
        {
            existingCoverKeys.insert(TO_STD(q.value(0).toString()));
        }
    }
    spdlog::info("[DB] Preloaded {} cover keys.", existingCoverKeys.size());

    // 2. 内存扁平化 (遍历树)
    std::vector<DirData> allDirs;
    std::vector<SongData> allSongs;
    allDirs.reserve(1000);
    allSongs.reserve(10000);

    int idCounter = 1;

    std::function<void(const std::shared_ptr<PlaylistNode> &, int)> collect;
    collect = [&](const std::shared_ptr<PlaylistNode> &node, int parentId)
    {
        if (!node)
            return;

        checkAndSaveCover(node->getCoverKey(), &existingCoverKeys);

        if (node->isDir())
        {
            int currentId = idCounter++;
            allDirs.push_back({currentId,
                               parentId,
                               TO_QSTR(fs::path(node->getPath()).filename().string()),
                               TO_QSTR(node->getPath()),
                               TO_QSTR(node->getCoverKey())});

            for (const auto &child : node->getChildren())
            {
                collect(child, currentId);
            }
        }
        else
        {
            const auto &md = node->getMetaData();
            allSongs.push_back({parentId,
                                TO_QSTR(md.getTitle()),
                                TO_QSTR(md.getArtist()),
                                TO_QSTR(md.getAlbum()),
                                TO_QSTR(md.getYear()),
                                TO_QSTR(md.getFilePath()),
                                static_cast<qint64>(md.getDuration()),
                                static_cast<qint64>(md.getOffset()),
                                static_cast<qint64>(timeToDb(md.getLastWriteTime())),
                                md.getSampleRate(),
                                md.getBitDepth(),
                                TO_QSTR(md.getFormatType())});
        }
    };

    collect(root, 0);
    spdlog::info("[DB] Flattening done. Dirs: {}, Songs: {}", allDirs.size(), allSongs.size());

    // 3. 批量插入目录
    if (!allDirs.empty())
    {
        m_db.transaction();
        QSqlQuery qDir(m_db);
        qDir.prepare("INSERT INTO table_directories (id, parent_id, name, full_path, cover_key) VALUES (?, ?, ?, ?, ?)");

        QVariantList ids, pids, names, paths, cks;
        for (const auto &d : allDirs)
        {
            ids << d.id;
            pids << (d.parentId == 0 ? QVariant(QMetaType(QMetaType::Int)) : d.parentId);
            names << d.name;
            paths << d.fullPath;
            cks << d.coverKey;
        }
        qDir.addBindValue(ids);
        qDir.addBindValue(pids);
        qDir.addBindValue(names);
        qDir.addBindValue(paths);
        qDir.addBindValue(cks);

        if (!qDir.execBatch())
        {
            spdlog::error("[DB] Dir batch insert failed: {}", qDir.lastError().text().toStdString());
            m_db.rollback();
            return;
        }
        m_db.commit();
    }

    // 4. 插入歌曲 (优化后的逻辑)
    if (allSongs.empty())
    {
        spdlog::info("[DB] Save completed (No songs).");
        return;
    }

    size_t totalSongs = allSongs.size();

    // [Fix 1] 提高阈值：50000 以下直接单线程，避免死锁开销
    if (totalSongs < 50000)
    {
        m_db.transaction();
        QSqlQuery q(m_db);
        // [Fix 2] 使用 INSERT IGNORE 忽略 CUE/文件 的重复冲突
        q.prepare(R"(
            INSERT IGNORE INTO table_songs 
            (directory_id, title, artist, album, year, file_path, 
             duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
             play_count, rating, last_played_at)
            VALUES 
            (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0)
        )");
        bindAndExecBatchSongs(q, allSongs);
        m_db.commit();
    }
    else
    {
        // --- 多线程并行写入 (仅在大数据量时启用) ---
        // [Fix 3] 限制最大并发数为 4，防止数据库死锁
        size_t hwThreads = std::thread::hardware_concurrency();
        size_t threadCount = std::min((size_t)4, hwThreads == 0 ? 2 : hwThreads);

        size_t batchSize = (totalSongs + threadCount - 1) / threadCount;
        std::vector<std::future<void>> futures;

        spdlog::info("[DB] Spawning {} threads for song insertion...", threadCount);

        ConnectionParams params = m_connParams;

        for (size_t i = 0; i < threadCount; ++i)
        {
            size_t startIdx = i * batchSize;
            size_t endIdx = std::min(startIdx + batchSize, totalSongs);
            if (startIdx >= totalSongs)
                break;

            std::vector<SongData> chunk(allSongs.begin() + startIdx, allSongs.begin() + endIdx);

            futures.emplace_back(SimpleThreadPool::instance().enqueue([params, chunk, i]()
                                                                      {
                QString connName = QString("Worker_%1_%2").arg(QUuid::createUuid().toString()).arg(i);
                
                {
                    QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", connName);
                    db.setHostName(params.host);
                    db.setPort(params.port);
                    db.setUserName(params.user);
                    db.setPassword(params.password);
                    db.setDatabaseName(params.dbName);
                    db.setConnectOptions("MYSQL_OPT_RECONNECT=1");

                    if (db.open())
                    {
                        // [Fix 4] 简单的死锁重试机制
                        int retryCount = 0;
                        bool success = false;
                        while(retryCount < 3 && !success) {
                            if(retryCount > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100 * retryCount));
                            
                            db.transaction();
                            QSqlQuery q(db);
                            q.prepare(R"(
                                INSERT IGNORE INTO table_songs 
                                (directory_id, title, artist, album, year, file_path, 
                                duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
                                play_count, rating, last_played_at)
                                VALUES 
                                (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0)
                            )");

                            bindAndExecBatchSongs(q, chunk);

                            if (db.commit()) {
                                success = true;
                            } else {
                                QSqlError err = db.lastError();
                                // Error 1213 = Deadlock
                                if (err.nativeErrorCode() == "1213") {
                                    db.rollback();
                                    retryCount++;
                                    spdlog::warn("[Thread {}] Deadlock detected, retrying {}...", i, retryCount);
                                } else {
                                    spdlog::error("[Thread {}] Commit failed: {}", i, err.text().toStdString());
                                    db.rollback();
                                    break; // 非死锁错误，不重试
                                }
                            }
                        }
                    }
                    else
                    {
                        spdlog::error("[Thread] DB Connect failed: {}", db.lastError().text().toStdString());
                    }
                    db.close();
                } 
                QSqlDatabase::removeDatabase(connName); }));
        }

        for (auto &f : futures)
        {
            f.wait();
        }
    }

    auto endTotal = std::chrono::high_resolution_clock::now();
    spdlog::info("[DB] Save Full Tree completed in {} ms.",
                 std::chrono::duration_cast<std::chrono::milliseconds>(endTotal - startTotal).count());
}

std::shared_ptr<PlaylistNode> DatabaseService::loadFullTree()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    spdlog::info("[DB] Loading full playlist tree...");

    std::unordered_map<int, std::shared_ptr<PlaylistNode>> dirMap;
    std::shared_ptr<PlaylistNode> root = nullptr;

    // 1. 加载所有目录结构
    QSqlQuery dirQ("SELECT id, parent_id, full_path, cover_key FROM table_directories ORDER BY id ASC", m_db);
    while (dirQ.next())
    {
        int id = dirQ.value(0).toInt();
        int pid = dirQ.value(1).toInt(); // NULL -> 0
        std::string path = TO_STD(dirQ.value(2).toString());
        std::string ckey = TO_STD(dirQ.value(3).toString());

        auto node = std::make_shared<PlaylistNode>(path, true);
        node->setCoverKey(ckey);
        dirMap[id] = node;

        if (pid == 0)
        {
            // 简单假定第一个无父节点的为根
            if (!root)
                root = node;
        }
        else
        {
            if (dirMap.count(pid))
                dirMap[pid]->addChild(node);
        }
    }

    if (!root)
    {
        spdlog::warn("[DB] No directory structure found.");
        return nullptr;
    }

    // 2. 加载歌曲 + 物理存在性检测 + 变更检测
    std::vector<int> idsToDelete; // 待删除的消失文件ID
    int updatedCount = 0;

    // 预编译更新语句
    QSqlQuery updateQ(m_db);
    updateQ.prepare(R"(
        UPDATE table_songs SET 
            title=:t, artist=:a, album=:al, year=:y, duration=:d, 
            last_write_time=:lwt, sample_rate=:sr, bit_depth=:bd, format_type=:fmt 
        WHERE id=:id
    )");

    QSqlQuery songQ("SELECT * FROM table_songs", m_db);

    int idx_id = songQ.record().indexOf("id");
    int idx_dir = songQ.record().indexOf("directory_id");
    int idx_path = songQ.record().indexOf("file_path");
    int idx_lwt = songQ.record().indexOf("last_write_time");

    // 开启事务以处理可能的批量更新/删除
    m_db.transaction();

    while (songQ.next())
    {
        int songId = songQ.value(idx_id).toInt();
        int dirId = songQ.value(idx_dir).toInt();
        std::string filePath = TO_STD(songQ.value(idx_path).toString());
        int64_t dbTime = songQ.value(idx_lwt).toLongLong();

        // 目录完整性检查
        if (dirMap.find(dirId) == dirMap.end())
        {
            idsToDelete.push_back(songId);
            continue;
        }

        // --- 检测 1: 物理文件是否存在? ---
        std::error_code ec;
        if (!fs::exists(filePath, ec))
        {
            spdlog::warn("[DB] File missing on disk: {}", filePath);
            idsToDelete.push_back(songId);
            continue;
        }

        // --- 检测 2: 文件是否被修改? ---
        auto ftime = fs::last_write_time(filePath, ec);
        MetaData md;

        if (timeToDb(ftime) != dbTime)
        {
            // 时间戳不一致 -> 重新解析
            spdlog::info("[DB] File changed detected: {}", filePath);
            md = FileScanner::getMetaData(filePath);

            // 同步更新数据库
            updateQ.bindValue(":t", TO_QSTR(md.getTitle()));
            updateQ.bindValue(":a", TO_QSTR(md.getArtist()));
            updateQ.bindValue(":al", TO_QSTR(md.getAlbum()));
            updateQ.bindValue(":y", TO_QSTR(md.getYear()));
            updateQ.bindValue(":d", static_cast<qint64>(md.getDuration()));
            updateQ.bindValue(":lwt", static_cast<qint64>(timeToDb(ftime)));
            updateQ.bindValue(":sr", md.getSampleRate());
            updateQ.bindValue(":bd", md.getBitDepth());
            updateQ.bindValue(":fmt", TO_QSTR(md.getFormatType()));
            updateQ.bindValue(":id", songId);
            updateQ.exec();

            updatedCount++;
        }
        else
        {
            // 一致 -> 使用 DB 缓存
            md.setTitle(TO_STD(songQ.value("title").toString()));
            md.setArtist(TO_STD(songQ.value("artist").toString()));
            md.setAlbum(TO_STD(songQ.value("album").toString()));
            md.setYear(TO_STD(songQ.value("year").toString()));
            md.setFilePath(filePath);
            md.setDuration(songQ.value("duration").toLongLong());
            md.setOffset(songQ.value("offset_val").toLongLong());
            md.setLastWriteTime(ftime);
            md.setSampleRate(songQ.value("sample_rate").toUInt());
            md.setBitDepth(songQ.value("bit_depth").toUInt());
            md.setFormatType(TO_STD(songQ.value("format_type").toString()));
        }

        // 构建节点
        auto songNode = std::make_shared<PlaylistNode>(filePath, false);

        std::string coverKey = md.getAlbum().empty() ? md.getTitle() : md.getAlbum();
        songNode->setCoverKey(coverKey);
        songNode->setMetaData(md);

        dirMap[dirId]->addChild(songNode);
    }

    // 3. 执行批量删除 (移除物理丢失的文件)
    if (!idsToDelete.empty())
    {
        spdlog::info("[DB] Deleting {} missing files...", idsToDelete.size());
        QSqlQuery delQ(m_db);
        delQ.prepare("DELETE FROM table_songs WHERE id = :id");
        for (int id : idsToDelete)
        {
            delQ.bindValue(":id", id);
            delQ.exec();
        }
    }

    m_db.commit(); // 提交事务

    // 4. 清理孤儿封面 (如果发生了删除或更新)
    if (!idsToDelete.empty() || updatedCount > 0)
    {
        cleanupOrphanedCovers();
    }

    // 5. 重新计算树统计 (TotalSongs / TotalDuration)
    std::function<void(std::shared_ptr<PlaylistNode>)> aggregate =
        [&](std::shared_ptr<PlaylistNode> n)
    {
        if (n->isDir())
        {
            uint64_t ts = 0, td = 0;
            for (auto &c : n->getChildren())
            {
                if (c->isDir())
                    aggregate(c);
                ts += c->isDir() ? c->getTotalSongs() : 1;
                td += c->isDir() ? c->getTotalDuration() : (c->getMetaData().getDuration() / 1000000);
            }
            n->setTotalSongs(ts);
            n->setTotalDuration(td);
        }
    };
    aggregate(root);

    spdlog::info("[DB] Load finished. Missing deleted: {}, Updated: {}", idsToDelete.size(), updatedCount);
    return root;
}

// ==========================================
//  业务功能实现 (Stored Proc, View, Triggers)
// ==========================================

bool DatabaseService::recordPlay(const std::string &filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    int64_t nowTick = std::chrono::system_clock::now().time_since_epoch().count();

    q.prepare("CALL sp_record_play(:path, :time)");
    q.bindValue(":path", TO_QSTR(filePath));
    q.bindValue(":time", static_cast<qint64>(nowTick));

    if (!q.exec())
    {
        spdlog::error("[DB] Call sp_record_play failed: {}", q.lastError().text().toStdString());
        return false;
    }
    return true;
}

std::vector<MetaData> DatabaseService::searchSongs(const std::string &keyword)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MetaData> results;
    QSqlQuery q(m_db);

    // 使用视图 view_library_search
    QString pattern = "%" + TO_QSTR(keyword) + "%";
    q.prepare("SELECT * FROM view_library_search WHERE title LIKE :k OR artist LIKE :k OR album LIKE :k");
    q.bindValue(":k", pattern);

    if (q.exec())
    {
        while (q.next())
        {
            MetaData md;
            md.setTitle(TO_STD(q.value("title").toString()));
            md.setArtist(TO_STD(q.value("artist").toString()));
            md.setAlbum(TO_STD(q.value("album").toString()));
            md.setFilePath(TO_STD(q.value("file_path").toString()));
            md.setDuration(q.value("duration").toLongLong());
            results.push_back(md);
        }
    }
    return results;
}

bool DatabaseService::updateRating(const std::string &filePath, int rating)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    // 触发器 trg_validate_rating_update 会自动处理范围限制
    q.prepare("UPDATE table_songs SET rating = :r WHERE file_path = :p");
    q.bindValue(":r", rating);
    q.bindValue(":p", TO_QSTR(filePath));
    return q.exec();
}

int DatabaseService::getPlayCount(const std::string &filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    q.prepare("SELECT play_count FROM table_songs WHERE file_path = :p");
    q.bindValue(":p", TO_QSTR(filePath));
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

int DatabaseService::getRating(const std::string &filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    q.prepare("SELECT rating FROM table_songs WHERE file_path = :p");
    q.bindValue(":p", TO_QSTR(filePath));
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

// ==========================================
//  CRUD (Add / Remove)
// ==========================================

int DatabaseService::getDirectoryId(const std::string &fullPath)
{
    QSqlQuery q(m_db);
    q.prepare("SELECT id FROM table_directories WHERE full_path = :p");
    q.bindValue(":p", TO_QSTR(fullPath));
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return -1;
}

bool DatabaseService::addSong(const MetaData &meta)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int dirId = getDirectoryId(meta.getParentDir());
    if (dirId == -1)
    {
        spdlog::error("[DB] Add song failed. Parent dir not found: {}", meta.getFilePath());
        return false;
    }

    QSqlQuery q(m_db);
    q.prepare(R"(
        INSERT INTO table_songs 
        (directory_id, title, artist, album, year, file_path, 
         duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
         play_count, rating, last_played_at)
        VALUES 
        (:did, :title, :artist, :album, :year, :path, 
         :dur, :off, :lwt, :sr, :bd, :fmt, 0, 0, 0)
    )");

    q.bindValue(":did", dirId);
    q.bindValue(":title", TO_QSTR(meta.getTitle()));
    q.bindValue(":artist", TO_QSTR(meta.getArtist()));
    q.bindValue(":album", TO_QSTR(meta.getAlbum()));
    q.bindValue(":year", TO_QSTR(meta.getYear()));
    q.bindValue(":path", TO_QSTR(meta.getFilePath()));
    q.bindValue(":dur", static_cast<qint64>(meta.getDuration()));
    q.bindValue(":off", static_cast<qint64>(meta.getOffset()));
    q.bindValue(":lwt", static_cast<qint64>(timeToDb(meta.getLastWriteTime())));
    q.bindValue(":sr", meta.getSampleRate());
    q.bindValue(":bd", meta.getBitDepth());
    q.bindValue(":fmt", TO_QSTR(meta.getFormatType()));

    return q.exec();
}

bool DatabaseService::addDirectory(const std::string &path, const std::string &name,
                                   const std::string &parentPath, const std::string &coverKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QVariant pidVal = QVariant(QMetaType(QMetaType::Int)); // NULL

    if (!parentPath.empty())
    {
        int pid = getDirectoryId(parentPath);
        if (pid != -1)
            pidVal = pid;
        else
            return false;
    }

    // 兼容手动ID模式：虽然我们有 AUTO_INCREMENT，但为了防止ID冲突或空洞，
    // 我们在这里可以让DB自动处理（传入 NULL id），或者手动查询 max(id)+1。
    // 这里保持使用 AUTO_INCREMENT 的特性，不指定 id。
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO table_directories (parent_id, name, full_path, cover_key) VALUES (:pid, :name, :path, :ck)");
    q.bindValue(":pid", pidVal);
    q.bindValue(":name", TO_QSTR(name));
    q.bindValue(":path", TO_QSTR(path));
    q.bindValue(":ck", TO_QSTR(coverKey));

    return q.exec();
}

bool DatabaseService::removeSong(const std::string &filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM table_songs WHERE file_path = :p");
    q.bindValue(":p", TO_QSTR(filePath));
    return q.exec();
}

bool DatabaseService::removeDirectory(const std::string &dirPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM table_directories WHERE full_path = :p");
    q.bindValue(":p", TO_QSTR(dirPath));
    return q.exec();
}