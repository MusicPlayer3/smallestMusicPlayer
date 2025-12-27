#include "DatabaseService.hpp"
#include "CoverCache.hpp"
#include "FileScanner.hpp"
#include "CoverImage.hpp"
// #include "SimpleThreadPool.hpp" // SQLite 写入建议单线程，不再需要线程池进行并发写入

// OpenCV 用于图片编解码
#include <opencv2/opencv.hpp>

// Qt & Utils
#include <QVariant>
#include <QVariantList>
#include <QUuid>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <spdlog/spdlog.h>
#include <unordered_set>

#define TO_QSTR(s) QString::fromStdString(s)
#define TO_STD(s) (s).toStdString()

namespace fs = std::filesystem;

// ==========================================
//  内部数据结构
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
    QString coverKey;
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

bool DatabaseService::connect(const std::string &dbPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dbPath = dbPath;

    if (QSqlDatabase::contains("qt_sql_default_connection"))
        m_db = QSqlDatabase::database("qt_sql_default_connection");
    else
        m_db = QSqlDatabase::addDatabase("QSQLITE");

    // 确保目录存在
    QString qPath = TO_QSTR(dbPath);
    QFileInfo fi(qPath);
    if (!fi.absoluteDir().exists())
    {
        QDir().mkpath(fi.absoluteDir().path());
    }

    m_db.setDatabaseName(qPath);

    if (!m_db.open())
    {
        spdlog::error("[DB] Connection failed: {}", m_db.lastError().text().toStdString());
        return false;
    }

    // SQLite 性能与功能优化指令
    QSqlQuery q(m_db);
    q.exec("PRAGMA foreign_keys = ON;");    // 启用外键约束
    q.exec("PRAGMA journal_mode = WAL;");   // Write-Ahead Logging，大幅提升并发性能
    q.exec("PRAGMA synchronous = NORMAL;"); // 适当降低同步级别以提升写入速度

    return initSchema();
}

bool DatabaseService::initSchema()
{
    QSqlQuery q(m_db);
    bool ok = true;

    // --- 1. Tables ---
    // SQLite: AUTO_INCREMENT -> AUTOINCREMENT (必须配合 INTEGER PRIMARY KEY)
    // 移除 ENGINE, CHARSET
    ok &= q.exec(R"(
        CREATE TABLE IF NOT EXISTS table_directories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            parent_id INTEGER DEFAULT NULL,
            name TEXT NOT NULL,
            full_path TEXT NOT NULL, 
            cover_key TEXT DEFAULT NULL,
            CONSTRAINT fk_dir_parent FOREIGN KEY (parent_id) REFERENCES table_directories(id) ON DELETE CASCADE
        );
    )");

    // SQLite 创建唯一索引需要单独语句
    q.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_dir_path ON table_directories (full_path)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_dir_cover ON table_directories (cover_key)");

    ok &= q.exec(R"(
        CREATE TABLE IF NOT EXISTS table_songs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            directory_id INTEGER NOT NULL,
            title TEXT,
            artist TEXT,
            album TEXT,
            year TEXT,
            file_path TEXT NOT NULL,
            cover_key TEXT DEFAULT NULL,
            duration INTEGER DEFAULT 0,
            offset_val INTEGER DEFAULT 0,
            last_write_time INTEGER DEFAULT 0,
            sample_rate INTEGER DEFAULT 0,
            bit_depth INTEGER DEFAULT 0,
            format_type TEXT,
            play_count INTEGER DEFAULT 0,
            rating INTEGER DEFAULT 0,
            last_played_at INTEGER DEFAULT 0,
            CONSTRAINT fk_song_dir FOREIGN KEY (directory_id) REFERENCES table_directories(id) ON DELETE CASCADE
        );
    )");

    q.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_file_path_offset ON table_songs (file_path, offset_val)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_album ON table_songs (album)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_artist ON table_songs (artist)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_title ON table_songs (title)");

    // Blob Table
    ok &= q.exec(R"(
        CREATE TABLE IF NOT EXISTS table_covers (
            cache_key TEXT NOT NULL PRIMARY KEY,
            thumbnail_data BLOB NOT NULL,
            updated_at INTEGER DEFAULT (strftime('%s', 'now')) 
        );
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
    // SQLite 的 Trigger 写法略有不同，不支持 IF...SET 这种过程式写法，需要用 CASE 或者 MIN/MAX
    // 或者完全依赖应用层逻辑。这里用 UPDATE 时的 CASE 语句演示。

    q.exec("DROP TRIGGER IF EXISTS trg_validate_rating_insert");
    q.exec(R"(
        CREATE TRIGGER trg_validate_rating_insert AFTER INSERT ON table_songs
        BEGIN
            UPDATE table_songs SET rating = 0 WHERE id = NEW.id AND rating < 0;
            UPDATE table_songs SET rating = 5 WHERE id = NEW.id AND rating > 5;
        END;
    )");

    q.exec("DROP TRIGGER IF EXISTS trg_validate_rating_update");
    q.exec(R"(
        CREATE TRIGGER trg_validate_rating_update AFTER UPDATE ON table_songs
        BEGIN
            UPDATE table_songs SET rating = 0 WHERE id = NEW.id AND rating < 0;
            UPDATE table_songs SET rating = 5 WHERE id = NEW.id AND rating > 5;
        END;
    )");

    // --- 4. Procedures ---
    // SQLite 不支持 Stored Procedures。逻辑已移至 recordPlay 函数内的 C++ 代码。

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
    // SQLite: INSERT OR IGNORE
    q.prepare("INSERT OR IGNORE INTO table_covers (cache_key, thumbnail_data) VALUES (:k, :d)");
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
        insQ.prepare("INSERT OR IGNORE INTO table_covers (cache_key, thumbnail_data) VALUES (:k, :d)");
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
            SELECT DISTINCT cover_key FROM table_songs WHERE cover_key IS NOT NULL
            UNION 
            SELECT DISTINCT cover_key FROM table_directories WHERE cover_key IS NOT NULL
        )
    )");
}

// ==========================================
//  整树存取 (核心逻辑)
// ==========================================

static void bindAndExecBatchSongs(QSqlQuery &q, const std::vector<SongData> &songs)
{
    if (songs.empty())
        return;

    QVariantList dids, titles, artists, albums, years, paths, cks, durs, offs, lwts, srs, bds, fmts;
    size_t sz = songs.size();

    dids.reserve(sz);
    titles.reserve(sz);
    artists.reserve(sz);
    albums.reserve(sz);
    years.reserve(sz);
    paths.reserve(sz);
    cks.reserve(sz);
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
        cks << s.coverKey;
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
    q.addBindValue(cks);
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

    spdlog::info("[DB] Starting saveFullTree (SQLite)...");
    auto startTotal = std::chrono::high_resolution_clock::now();

    // -------------------------------------------------------------------------
    // 步骤 1: 备份统计数据 (PlayCount, Rating)
    // -------------------------------------------------------------------------
    m_db.transaction();
    QSqlQuery q(m_db);

    // SQLite 的内存表或临时表
    q.exec("CREATE TEMP TABLE IF NOT EXISTS tmp_stats_backup ("
           "  file_path TEXT NOT NULL, "
           "  play_count INTEGER DEFAULT 0, "
           "  rating INTEGER DEFAULT 0, "
           "  last_played_at INTEGER DEFAULT 0 "
           ")");
    q.exec("CREATE INDEX IF NOT EXISTS idx_tmp_path ON tmp_stats_backup (file_path)");
    q.exec("DELETE FROM tmp_stats_backup"); // TRUNCATE -> DELETE FROM

    if (!q.exec("INSERT INTO tmp_stats_backup (file_path, play_count, rating, last_played_at) "
                "SELECT file_path, play_count, rating, last_played_at FROM table_songs "
                "WHERE play_count > 0 OR rating > 0"))
    {
        spdlog::error("[DB] Failed to backup stats: {}", q.lastError().text().toStdString());
        m_db.rollback();
        return;
    }

    // -------------------------------------------------------------------------
    // 步骤 2: 清空主表
    // -------------------------------------------------------------------------
    // SQLite 不需要关闭外键检查来清空表，只要按顺序删除或使用 DELETE FROM
    // 如果设置了 ON DELETE CASCADE，删目录也会删歌
    q.exec("DELETE FROM table_directories");
    // 上面会自动删除 table_songs (因外键 CASCADE)
    // 为了保险起见也可以显式清空:
    q.exec("DELETE FROM table_songs");

    // SQLite: 重置自增 ID (可选)
    q.exec("DELETE FROM sqlite_sequence WHERE name='table_directories'");
    q.exec("DELETE FROM sqlite_sequence WHERE name='table_songs'");

    m_db.commit();

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
                                TO_QSTR(node->getCoverKey()),
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
    // 步骤 5: 插入歌曲 (改为单线程)
    // -------------------------------------------------------------------------
    // SQLite 是文件锁数据库，多线程同时写入容易导致 SQLITE_BUSY。
    // 对于本地文件 I/O，单线程大事务通常比多线程竞争锁更快。
    size_t totalSongs = allSongs.size();
    if (totalSongs > 0)
    {
        m_db.transaction();
        QSqlQuery qS(m_db);
        // INSERT OR IGNORE
        qS.prepare(R"(
            INSERT OR IGNORE INTO table_songs 
            (directory_id, title, artist, album, year, file_path, cover_key,
                duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
                play_count, rating, last_played_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0)
        )");

        // 分批 Batch 执行，防止一次性绑定内存过大 (虽然 SQLite 限制较宽，但保守起见分块)
        const size_t BATCH_SIZE = 5000;
        for (size_t i = 0; i < totalSongs; i += BATCH_SIZE)
        {
            size_t end = std::min(i + BATCH_SIZE, totalSongs);
            std::vector<SongData> chunk(allSongs.begin() + i, allSongs.begin() + end);
            bindAndExecBatchSongs(qS, chunk);
        }
        m_db.commit();
    }

    // -------------------------------------------------------------------------
    // 步骤 6: 恢复统计数据
    // -------------------------------------------------------------------------
    spdlog::info("[DB] Restoring stats from backup...");
    m_db.transaction();

    // SQLite UPDATE ... FROM 语法 (SQLite 3.33+)
    // 兼容旧版 SQLite 的写法通常比较麻烦，但 Qt 附带的 SQLite 通常较新。
    // 如果支持 UPDATE FROM:
    bool restoreOk = q.exec(R"(
        UPDATE table_songs 
        SET play_count = tmp_stats_backup.play_count,
            rating = tmp_stats_backup.rating,
            last_played_at = tmp_stats_backup.last_played_at
        FROM tmp_stats_backup
        WHERE table_songs.file_path = tmp_stats_backup.file_path
    )");

    if (!restoreOk)
    {
        // 降级方案：如果不支持 UPDATE FROM，尝试 REPLACE 或忽略
        // 或者在 C++ 中读取 tmp 表然后逐条更新（较慢）
        spdlog::warn("[DB] UPDATE FROM failed (old SQLite?), stats might be lost: {}", q.lastError().text().toStdString());
    }

    q.exec("DROP TABLE IF EXISTS tmp_stats_backup");
    m_db.commit();

    auto endTotal = std::chrono::high_resolution_clock::now();
    spdlog::info("[DB] Save Full Tree (SQLite) completed in {} ms.",
                 std::chrono::duration_cast<std::chrono::milliseconds>(endTotal - startTotal).count());
}

std::shared_ptr<PlaylistNode> DatabaseService::loadFullTree()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    spdlog::info("[DB] Loading full playlist tree...");

    std::unordered_map<int, std::shared_ptr<PlaylistNode>> dirMap;
    std::shared_ptr<PlaylistNode> root = nullptr;

    // 1. Load Directories
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

    // 2. Load Songs
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
    int idx_cover_key = songQ.record().indexOf("cover_key");

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

        md.setPlayCount(songQ.value("play_count").toInt());
        md.setRating(songQ.value("rating").toInt());

        auto songNode = std::make_shared<PlaylistNode>(filePath, false);

        if (idx_cover_key != -1)
        {
            std::string dbKey = TO_STD(songQ.value(idx_cover_key).toString());
            songNode->setCoverKey(dbKey.empty() ? (md.getAlbum().empty() ? md.getTitle() : md.getAlbum()) : dbKey);
        }
        else
        {
            songNode->setCoverKey(md.getAlbum().empty() ? md.getTitle() : md.getAlbum());
        }

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
            n->sortChildren();
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

    // SQLite 不支持 CALL，直接执行 Update
    q.prepare("UPDATE table_songs SET play_count = play_count + 1, last_played_at = :time WHERE file_path = :path");
    q.bindValue(":time", static_cast<qint64>(nowTick));
    q.bindValue(":path", TO_QSTR(filePath));
    return q.exec();
}

std::vector<MetaData> DatabaseService::searchSongs(const std::string &keyword)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MetaData> results;
    QSqlQuery q(m_db);

    // SQLite 没有 MATCH ... AGAINST (除非用 FTS5)
    // 使用标准 LIKE，配合 View
    QString pattern = "%" + TO_QSTR(keyword) + "%";

    q.prepare(R"(
        SELECT * FROM view_library_search 
        WHERE 
            title LIKE :pat 
            OR artist LIKE :pat 
            OR album LIKE :pat
        LIMIT 200
    )");

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
    // 范围检查由 Trigger 处理，或者这里也可以做
    if (rating < 0)
        rating = 0;
    if (rating > 5)
        rating = 5;

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

bool DatabaseService::addSong(const MetaData &meta, const std::string &coverKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    int dirId = getDirectoryId(meta.getParentDir());
    if (dirId == -1)
        return false;

    QSqlQuery q(m_db);
    // SQLite Upsert Syntax:
    // INSERT INTO ... VALUES (...) ON CONFLICT(col) DO UPDATE SET ...
    // 前提：table_songs 在 (file_path, offset_val) 上必须有 UNIQUE 索引（initSchema中已创建）
    q.prepare(R"(
        INSERT INTO table_songs 
        (directory_id, title, artist, album, year, file_path, cover_key,
         duration, offset_val, last_write_time, sample_rate, bit_depth, format_type, 
         play_count, rating, last_played_at)
        VALUES 
        (:did, :title, :artist, :album, :year, :path, :ck,
         :dur, :off, :lwt, :sr, :bd, :fmt, 0, 0, 0)
        ON CONFLICT(file_path, offset_val) DO UPDATE SET
            directory_id = excluded.directory_id,
            title = excluded.title,
            cover_key = excluded.cover_key,
            last_write_time = excluded.last_write_time,
            duration = excluded.duration, 
            sample_rate = excluded.sample_rate,
            bit_depth = excluded.bit_depth,
            format_type = excluded.format_type
    )");

    q.bindValue(":did", dirId);
    q.bindValue(":title", TO_QSTR(meta.getTitle()));
    q.bindValue(":artist", TO_QSTR(meta.getArtist()));
    q.bindValue(":album", TO_QSTR(meta.getAlbum()));
    q.bindValue(":year", TO_QSTR(meta.getYear()));
    q.bindValue(":path", TO_QSTR(meta.getFilePath()));
    q.bindValue(":ck", TO_QSTR(coverKey));
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