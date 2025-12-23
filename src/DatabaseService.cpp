#include "DatabaseService.hpp"
#include "CoverCache.hpp"
#include "FileScanner.hpp"
#include "CoverImage.hpp"
#include "SimpleThreadPool.hpp"

// OpenCV 用于图片编解码
#include <opencv2/opencv.hpp>

// Qt & Utils
#include <QVariant>
#include <QVariantList>
#include <QUuid>
#include <QDebug>
#include <spdlog/spdlog.h>
#include <future>
#include <unordered_set>
#include <thread>

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

    m_connParams.host = TO_QSTR(host);
    m_connParams.port = port;
    m_connParams.user = TO_QSTR(user);
    m_connParams.password = TO_QSTR(password);
    m_connParams.dbName = TO_QSTR(dbName);

    if (!QSqlDatabase::isDriverAvailable("QMYSQL"))
    {
        spdlog::critical("[DB] QMYSQL 驱动不可用！");
        return false;
    }

    if (QSqlDatabase::contains("qt_sql_default_connection"))
        m_db = QSqlDatabase::database("qt_sql_default_connection");
    else
        m_db = QSqlDatabase::addDatabase("QMYSQL");

    m_db.setHostName(m_connParams.host);
    m_db.setPort(m_connParams.port);
    m_db.setUserName(m_connParams.user);
    m_db.setPassword(m_connParams.password);
    m_db.setConnectOptions("MYSQL_OPT_RECONNECT=1");

    // 尝试连接并自动创建库
    m_db.setDatabaseName(m_connParams.dbName);
    if (!m_db.open())
    {
        QSqlError err = m_db.lastError();
        if (err.nativeErrorCode() == "1049" || err.text().contains("Unknown database"))
        {
            spdlog::info("[DB] Database '{}' not found. Creating...", dbName);
            m_db.setDatabaseName("");
            if (!m_db.open())
                return false;

            QSqlQuery q(m_db);
            QString createSql = QString("CREATE DATABASE IF NOT EXISTS %1 CHARACTER SET utf8mb4 COLLATE utf8mb4_bin").arg(m_connParams.dbName);
            if (!q.exec(createSql))
                return false;

            m_db.close();
            m_db.setDatabaseName(m_connParams.dbName);
            if (!m_db.open())
                return false;
        }
        else
        {
            spdlog::error("[DB] Connection failed: {}", err.text().toStdString());
            return false;
        }
    }

    QSqlQuery q(m_db);
    q.exec("SET NAMES 'utf8mb4' COLLATE 'utf8mb4_bin'");

    return initSchema();
}

bool DatabaseService::initSchema()
{
    QSqlQuery q(m_db);
    bool ok = true;

    // --- 1. Tables ---
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

    // [优化] 添加全文索引，加速搜索 (注意: MySQL InnoDB 5.6+ 支持)
    // 使用 try-catch 风格或忽略错误，防止重复创建报错
    q.exec("CREATE FULLTEXT INDEX idx_ft_meta ON table_songs(title, artist, album)");

    ok &= q.exec(R"(
        CREATE TABLE IF NOT EXISTS table_covers (
            cache_key VARCHAR(255) NOT NULL PRIMARY KEY,
            thumbnail_data MEDIUMBLOB NOT NULL,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
    )");

    if (!ok)
        spdlog::error("[DB] Table creation failed: {}", q.lastError().text().toStdString());

    // --- 2. View ---
    q.exec("DROP VIEW IF EXISTS view_library_search");
    ok = q.exec(R"(
        CREATE VIEW view_library_search AS
        SELECT 
            s.id, s.title, s.artist, s.album, s.file_path, s.duration, s.play_count, s.rating,
            d.full_path AS dir_path
        FROM table_songs s
        JOIN table_directories d ON s.directory_id = d.id
    )");

    // --- 3. Triggers ---
    q.exec("DROP TRIGGER IF EXISTS trg_validate_rating_insert");
    q.exec(R"(
        CREATE TRIGGER trg_validate_rating_insert BEFORE INSERT ON table_songs
        FOR EACH ROW BEGIN
            IF NEW.rating < 0 THEN SET NEW.rating = 0; END IF;
            IF NEW.rating > 5 THEN SET NEW.rating = 5; END IF;
        END
    )");

    q.exec("DROP TRIGGER IF EXISTS trg_validate_rating_update");
    q.exec(R"(
        CREATE TRIGGER trg_validate_rating_update BEFORE UPDATE ON table_songs
        FOR EACH ROW BEGIN
            IF NEW.rating < 0 THEN SET NEW.rating = 0; END IF;
            IF NEW.rating > 5 THEN SET NEW.rating = 5; END IF;
        END
    )");

    // --- 4. Procedures ---
    q.exec("DROP PROCEDURE IF EXISTS sp_record_play");
    ok = q.exec(R"(
        CREATE PROCEDURE sp_record_play(IN target_path VARCHAR(768), IN current_time_tick BIGINT)
        BEGIN
            UPDATE table_songs 
            SET play_count = play_count + 1, last_played_at = current_time_tick
            WHERE file_path = target_path;
        END
    )");

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
//  封面数据服务
// ==========================================

void DatabaseService::saveCoverBlob(const std::string &key, const std::vector<uint8_t> &pngData)
{
    if (key.empty() || pngData.empty())
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    q.prepare("INSERT IGNORE INTO table_covers (cache_key, thumbnail_data) VALUES (:k, :d)");
    q.bindValue(":k", TO_QSTR(key));
    q.bindValue(":d", QByteArray(reinterpret_cast<const char *>(pngData.data()), pngData.size()));
    q.exec();
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

void DatabaseService::checkAndSaveCover(const std::string &key, std::unordered_set<std::string> *existingKeys)
{
    if (key.empty() || key == "Unknown")
        return;

    if (existingKeys && existingKeys->find(key) != existingKeys->end())
        return;

    if (!existingKeys)
    {
        QSqlQuery checkQ(m_db);
        checkQ.prepare("SELECT 1 FROM table_covers WHERE cache_key = :k");
        checkQ.bindValue(":k", TO_QSTR(key));
        if (checkQ.exec() && checkQ.next())
            return;
    }

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

        if (existingKeys)
            existingKeys->insert(key);
    }
    catch (...)
    {
    }
}

void DatabaseService::cleanupOrphanedCovers()
{
    QSqlQuery q(m_db);
    q.exec(R"(
        DELETE FROM table_covers 
        WHERE cache_key NOT IN (
            SELECT DISTINCT album FROM table_songs WHERE album IS NOT NULL
            UNION 
            SELECT DISTINCT cover_key FROM table_directories WHERE cover_key IS NOT NULL
        )
    )");
}

// ==========================================
//  整树存取 (核心逻辑 - 优化版)
// ==========================================

static void bindAndExecBatchSongs(QSqlQuery &q, const std::vector<SongData> &songs)
{
    if (songs.empty())
        return;

    QVariantList dids, titles, artists, albums, years, paths, durs, offs, lwts, srs, bds, fmts;
    size_t sz = songs.size();

    // 预分配内存
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
        return;

    spdlog::info("[DB] Starting saveFullTree with stats preservation...");
    auto startTotal = std::chrono::high_resolution_clock::now();

    // -------------------------------------------------------------------------
    // 步骤 1: 备份统计数据 (PlayCount, Rating)
    // -------------------------------------------------------------------------
    // 即使目录结构变了，只要文件路径没变，我们就应该保留它的评分和播放次数。
    // 使用临时表进行备份。
    // -------------------------------------------------------------------------
    m_db.transaction();
    QSqlQuery q(m_db);

    // 1.1 创建临时表 (MEMORY引擎更快)
    q.exec("CREATE TEMPORARY TABLE IF NOT EXISTS tmp_stats_backup ("
           "  file_path VARCHAR(768) NOT NULL, "
           "  play_count INT DEFAULT 0, "
           "  rating TINYINT DEFAULT 0, "
           "  last_played_at BIGINT DEFAULT 0, "
           "  INDEX idx_tmp_path (file_path)"
           ") ENGINE=MEMORY DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin");

    // 1.2 清空临时表 (以防连接复用有残留)
    q.exec("TRUNCATE TABLE tmp_stats_backup");

    // 1.3 备份有意义的数据 (播放过或评过分的)
    if (!q.exec("INSERT INTO tmp_stats_backup (file_path, play_count, rating, last_played_at) "
                "SELECT file_path, play_count, rating, last_played_at FROM table_songs "
                "WHERE play_count > 0 OR rating > 0"))
    {
        spdlog::error("[DB] Failed to backup stats: {}", q.lastError().text().toStdString());
        // 如果备份失败，回滚并终止，防止数据丢失
        m_db.rollback();
        return;
    }

    // -------------------------------------------------------------------------
    // 步骤 2: 清空主表并预备新数据
    // -------------------------------------------------------------------------
    q.exec("SET FOREIGN_KEY_CHECKS = 0");
    q.exec("TRUNCATE TABLE table_songs");
    q.exec("TRUNCATE TABLE table_directories");
    q.exec("SET FOREIGN_KEY_CHECKS = 1");
    m_db.commit(); // 提交清空操作，后续插入开启新事务

    // 预加载封面Key
    std::unordered_set<std::string> existingCoverKeys;
    QSqlQuery coverQ("SELECT cache_key FROM table_covers", m_db);
    while (coverQ.next())
        existingCoverKeys.insert(TO_STD(coverQ.value(0).toString()));

    // -------------------------------------------------------------------------
    // 步骤 3: 内存扁平化
    // -------------------------------------------------------------------------
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
            allDirs.push_back({currentId, parentId,
                               TO_QSTR(fs::path(node->getPath()).filename().string()),
                               TO_QSTR(node->getPath()),
                               TO_QSTR(node->getCoverKey())});
            for (const auto &child : node->getChildren())
                collect(child, currentId);
        }
        else
        {
            const auto &md = node->getMetaData();
            allSongs.push_back({parentId,
                                TO_QSTR(md.getTitle()), TO_QSTR(md.getArtist()),
                                TO_QSTR(md.getAlbum()), TO_QSTR(md.getYear()),
                                TO_QSTR(md.getFilePath()),
                                static_cast<qint64>(md.getDuration()),
                                static_cast<qint64>(md.getOffset()),
                                static_cast<qint64>(timeToDb(md.getLastWriteTime())),
                                md.getSampleRate(), md.getBitDepth(),
                                TO_QSTR(md.getFormatType())});
        }
    };
    collect(root, 0);

    // -------------------------------------------------------------------------
    // 步骤 4: 插入目录
    // -------------------------------------------------------------------------
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

    // -------------------------------------------------------------------------
    // 步骤 5: 插入歌曲 (单线程或多线程)
    // -------------------------------------------------------------------------
    size_t totalSongs = allSongs.size();
    if (totalSongs > 0)
    {
        if (totalSongs < 50000)
        {
            m_db.transaction();
            QSqlQuery qS(m_db);
            // 初始插入统计数据为0
            qS.prepare(R"(
                INSERT IGNORE INTO table_songs 
                (directory_id, title, artist, album, year, file_path, 
                 duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
                 play_count, rating, last_played_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0)
            )");
            bindAndExecBatchSongs(qS, allSongs);
            m_db.commit();
        }
        else
        {
            // 多线程插入
            size_t hardwareConcurrency = std::thread::hardware_concurrency();
            size_t threadCount = std::min((size_t)4, hardwareConcurrency != 0 ? hardwareConcurrency : (size_t)2);
            size_t batchSize = (totalSongs + threadCount - 1) / threadCount;
            std::vector<std::future<void>> futures;
            ConnectionParams params = m_connParams;

            for (size_t i = 0; i < threadCount; ++i)
            {
                size_t start = i * batchSize;
                size_t end = std::min(start + batchSize, totalSongs);
                if (start >= totalSongs)
                    break;

                std::vector<SongData> chunk(allSongs.begin() + start, allSongs.begin() + end);
                futures.emplace_back(SimpleThreadPool::instance().enqueue([params, chunk, i]()
                                                                          {
                    QString cName = QString("Worker_%1").arg(i);
                    {
                        QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", cName);
                        db.setHostName(params.host); db.setPort(params.port);
                        db.setUserName(params.user); db.setPassword(params.password);
                        db.setDatabaseName(params.dbName);
                        db.setConnectOptions("MYSQL_OPT_RECONNECT=1");
                        if (db.open()) {
                            db.transaction();
                            QSqlQuery q(db);
                            q.prepare(R"(
                                INSERT IGNORE INTO table_songs 
                                (directory_id, title, artist, album, year, file_path, 
                                duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
                                play_count, rating, last_played_at)
                                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0)
                            )");
                            bindAndExecBatchSongs(q, chunk);
                            db.commit();
                        }
                        db.close();
                    }
                    QSqlDatabase::removeDatabase(cName); }));
            }
            for (auto &f : futures)
                f.wait();
        }
    }

    // -------------------------------------------------------------------------
    // 步骤 6: 恢复统计数据 (Restore Stats)
    // -------------------------------------------------------------------------
    // 此时所有歌曲已插入，play_count 均为 0。
    // 我们将临时表中的数据 Update 回去。
    // -------------------------------------------------------------------------
    spdlog::info("[DB] Restoring stats from backup...");
    m_db.transaction();
    bool restoreOk = q.exec(R"(
        UPDATE table_songs s
        JOIN tmp_stats_backup t ON s.file_path = t.file_path
        SET s.play_count = t.play_count,
            s.rating = t.rating,
            s.last_played_at = t.last_played_at
    )");

    if (!restoreOk)
        spdlog::error("[DB] Stats restore failed: {}", q.lastError().text().toStdString());

    // 清理临时表
    q.exec("DROP TEMPORARY TABLE IF EXISTS tmp_stats_backup");
    m_db.commit();

    auto endTotal = std::chrono::high_resolution_clock::now();
    spdlog::info("[DB] Save Full Tree (w/ Stats) completed in {} ms.",
                 std::chrono::duration_cast<std::chrono::milliseconds>(endTotal - startTotal).count());
}

std::shared_ptr<PlaylistNode> DatabaseService::loadFullTree()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    spdlog::info("[DB] Loading full playlist tree...");

    std::unordered_map<int, std::shared_ptr<PlaylistNode>> dirMap;
    std::shared_ptr<PlaylistNode> root = nullptr;

    QSqlQuery dirQ("SELECT id, parent_id, full_path, cover_key FROM table_directories ORDER BY id ASC", m_db);
    while (dirQ.next())
    {
        int id = dirQ.value(0).toInt();
        int pid = dirQ.value(1).toInt();
        auto node = std::make_shared<PlaylistNode>(TO_STD(dirQ.value(2).toString()), true);
        node->setCoverKey(TO_STD(dirQ.value(3).toString()));
        dirMap[id] = node;

        if (pid == 0 && !root)
            root = node;
        else if (dirMap.count(pid))
            dirMap[pid]->addChild(node);
    }

    if (!root)
        return nullptr;

    // Load Songs
    std::vector<int> idsToDelete;
    int updatedCount = 0;

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

    m_db.transaction();
    while (songQ.next())
    {
        int songId = songQ.value(idx_id).toInt();
        int dirId = songQ.value(idx_dir).toInt();
        std::string filePath = TO_STD(songQ.value(idx_path).toString());

        if (dirMap.find(dirId) == dirMap.end())
        {
            idsToDelete.push_back(songId);
            continue;
        }

        std::error_code ec;
        if (!fs::exists(filePath, ec))
        {
            idsToDelete.push_back(songId);
            continue;
        }

        MetaData md;
        auto ftime = fs::last_write_time(filePath, ec);
        int64_t dbTime = songQ.value(idx_lwt).toLongLong();

        if (timeToDb(ftime) != dbTime)
        {
            md = FileScanner::getMetaData(filePath);
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

        // 加载统计数据
        md.setPlayCount(songQ.value("play_count").toInt());
        md.setRating(songQ.value("rating").toInt());

        auto songNode = std::make_shared<PlaylistNode>(filePath, false);
        songNode->setCoverKey(md.getAlbum().empty() ? md.getTitle() : md.getAlbum());
        songNode->setMetaData(md);
        dirMap[dirId]->addChild(songNode);
    }

    if (!idsToDelete.empty())
    {
        QSqlQuery delQ(m_db);
        delQ.prepare("DELETE FROM table_songs WHERE id = :id");
        for (int id : idsToDelete)
        {
            delQ.bindValue(":id", id);
            delQ.exec();
        }
    }
    m_db.commit();

    if (!idsToDelete.empty() || updatedCount > 0)
        cleanupOrphanedCovers();

    // 统计聚合
    std::function<void(std::shared_ptr<PlaylistNode>)> aggregate = [&](std::shared_ptr<PlaylistNode> n)
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

    return root;
}

// ==========================================
//  业务功能
// ==========================================

bool DatabaseService::recordPlay(const std::string &filePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QSqlQuery q(m_db);
    int64_t nowTick = std::chrono::system_clock::now().time_since_epoch().count();
    q.prepare("CALL sp_record_play(:path, :time)");
    q.bindValue(":path", TO_QSTR(filePath));
    q.bindValue(":time", static_cast<qint64>(nowTick));
    return q.exec();
}

std::vector<MetaData> DatabaseService::searchSongs(const std::string &keyword)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MetaData> results;
    QSqlQuery q(m_db);

    // [优化] 尝试使用全文索引，若不满足条件(如中文分词问题)可回退到LIKE
    // 这里使用 OR 逻辑：匹配全文索引 或者 LIKE 匹配路径/标题
    QString pattern = "%" + TO_QSTR(keyword) + "%";

    // MATCH ... AGAINST IN BOOLEAN MODE 允许使用 +word -word 等操作符
    // 联合查询 View 以获取目录信息
    q.prepare(R"(
        SELECT * FROM view_library_search 
        WHERE 
            MATCH(title, artist, album) AGAINST(:kw IN BOOLEAN MODE)
            OR title LIKE :pat 
            OR artist LIKE :pat 
            OR album LIKE :pat
        LIMIT 200
    )");

    q.bindValue(":kw", TO_QSTR(keyword));
    q.bindValue(":pat", pattern);

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
//  CRUD
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
        return false;

    QSqlQuery q(m_db);
    // [Fix] AddSong 也应该遵守不覆盖统计数据的原则，使用 ON DUPLICATE UPDATE
    q.prepare(R"(
        INSERT INTO table_songs 
        (directory_id, title, artist, album, year, file_path, 
         duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
         play_count, rating, last_played_at)
        VALUES 
        (:did, :title, :artist, :album, :year, :path, 
         :dur, :off, :lwt, :sr, :bd, :fmt, 0, 0, 0)
        ON DUPLICATE KEY UPDATE
            directory_id = VALUES(directory_id),
            title = VALUES(title),
            last_write_time = VALUES(last_write_time)
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
    QVariant pidVal = QVariant(QMetaType(QMetaType::Int));

    if (!parentPath.empty())
    {
        int pid = getDirectoryId(parentPath);
        if (pid != -1)
            pidVal = pid;
        else
            return false;
    }

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