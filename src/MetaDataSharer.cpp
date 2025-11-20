#include "MetaDataSharer.hpp"

#ifdef __linux__
#include "mpris_server.hpp"
// 引入 sdbus 头文件以使用 sdbus::ObjectPath 和 sdbus::Variant
#include <sdbus-c++/sdbus-c++.h>
#include <vector>
#include <map>
#endif

MetaDataSharer::MetaDataSharer(std::shared_ptr<AudioPlayer> player)
{
    this->player = std::move(player);

#ifdef __linux__
    // 创建 Server 实例
    server = mpris::Server::make("smallestMusicPlayer");

    if (!server)
    {
        std::cerr << "[MetaDataSharer] Error: Can't connect to D-Bus. MPRIS disabled.\n";
        return;
    }

    // 基础信息设置
    server->set_identity("smallestMusicPlayer");
    // server->set_desktop_entry("smallestMusicPlayer"); // 需对应 .desktop 文件名
    server->set_supported_uri_schemes({"file"});
    server->set_supported_mime_types({"application/octet-stream", "audio/mpeg", "audio/flac", "audio/x-wav"});

    // --- 绑定回调函数  ---
    server->on_quit([this]()
                    { this->onQuit(); });
    server->on_next([this]()
                    { this->onNext(); });
    server->on_previous([this]()
                        { this->onPrevious(); });
    server->on_pause([this]()
                     { this->onPause(); });
    server->on_play_pause([this]()
                          { this->onPlayPause(); });
    server->on_stop([this]()
                    { this->onStop(); });
    server->on_play([this]()
                    { this->onPlay(); });

    // [新增] 必须绑定以下三个回调，CanControl 才会变为 true
    server->on_loop_status_changed([](mpris::LoopStatus status)
                                   { std::cout << "[MPRIS] Loop status changed (Not implemented)" << std::endl; });

    server->on_shuffle_changed([](bool shuffle)
                               { std::cout << "[MPRIS] Shuffle changed (Not implemented)" << std::endl; });

    server->on_volume_changed([](double volume)
                              { std::cout << "[MPRIS] Volume changed (Not implemented)" << std::endl; });

    // Seek 传入 int64_t (Offset)
    server->on_seek([this](int64_t offset)
                    { this->onSeek(offset); });

    // SetPosition 传入 int64_t (Absolute Position)
    // 注意：mpris_server.hpp 中 set_position_method 会先检查 TrackId，然后调用此回调
    server->on_set_position([this](int64_t pos)
                            { this->onSetPosition(pos); });

    // 启动 DBus 事件循环 (必须调用，否则收不到信号)
    server->start_loop_async();

    std::cout << "[MetaDataSharer] MPRIS Server initialized and loop started." << std::endl;
#endif

#ifdef _WIN32
    std::cout << "[MetaDataSharer] Windows implementation placeholder." << std::endl;
#endif
}

MetaDataSharer::~MetaDataSharer()
{
    // 智能指针会自动释放 server
}

void MetaDataSharer::setMetaData(const std::string &title, const std::vector<std::string> &artist,
                                 const std::string &album, const std::string &uri,
                                 const std::string &coverPath, int64_t duration)
{
#ifdef __linux__
    if (!server)
        return;

    // 修复：使用 map<mpris::Field, sdbus::Variant> 构建元数据
    std::map<mpris::Field, sdbus::Variant> meta;

    // 1. Track ID (必须是 ObjectPath 类型)
    meta[mpris::Field::TrackId] = sdbus::Variant(sdbus::ObjectPath("/org/mpris/MediaPlayer2/Track/Current"));

    // 2. Title
    meta[mpris::Field::Title] = sdbus::Variant(title);

    // 3. Artist (MPRIS 标准要求是字符串列表 "as")
    meta[mpris::Field::Artist] = sdbus::Variant(artist);

    // 4. Album
    meta[mpris::Field::Album] = sdbus::Variant(album);

    // 5. URL
    meta[mpris::Field::Url] = sdbus::Variant(localPathToUri(uri));

    // 6. Art URL (封面)
    if (!coverPath.empty())
    {
        meta[mpris::Field::ArtUrl] = sdbus::Variant(localPathToUri(coverPath));
    }

    // 7. Length (单位微秒，int64_t)
    if (duration > 0)
    {
        meta[mpris::Field::Length] = sdbus::Variant((int64_t)duration);
    }

    // 调用 set_metadata
    server->set_metadata(meta);
#endif
}

void MetaDataSharer::setMetaData(const MetaData &metadata)
{
    // 假设 MetaData 结构体包含以下字段
    // 如果字段名不同，请自行修改
    // setMetaData(metadata.getTitle(),
    //             metadata.getArtist(),
    //             metadata.getAlbum(),
    //             metadata.getFilePath(),       // 或 metadata.filePath
    //             metadata.getc, // 假设有封面路径
    //             metadata.duration); // 假设有时长
}

#ifdef __linux__
void MetaDataSharer::setPlayBackStatus(mpris::PlaybackStatus status)
{
    if (server)
    {
        server->set_playback_status(status);
    }
}

void MetaDataSharer::setPosition(std::chrono::microseconds position)
{
    if (server)
    {
        // 这里只是更新属性，不会触发 seek 信号
        server->set_position(position.count());
    }
}

// --- 辅助函数 ---
std::string MetaDataSharer::localPathToUri(const std::string &path)
{
    if (path.empty())
        return "";
    if (path.find("file://") == 0)
        return path;
    if (path.find("http") == 0)
        return path;

    std::string uri = "file://";
    // 简单处理，实际可能需要 URL 编码
    if (path[0] != '/')
        uri += "/";
    uri += path;
    return uri;
}

// --- 回调实现 ---

void MetaDataSharer::onQuit()
{
    std::cout << "[MetaDataSharer] Action: Quit" << std::endl;
    if (player)
    {
        // player->quit(); // 假设 AudioPlayer 有此接口
    }
    exit(0);
}

void MetaDataSharer::onNext()
{
    std::cout << "[MetaDataSharer] Action: Next" << std::endl;
    if (player)
    {
        // player->next();
    }
}

void MetaDataSharer::onPrevious()
{
    std::cout << "[MetaDataSharer] Action: Previous" << std::endl;
    if (player)
    {
        // player->previous();
    }
}

void MetaDataSharer::onPause()
{
    std::cout << "[MetaDataSharer] Action: Pause" << std::endl;
    if (player)
    {
        player->pause();
    }
    setPlayBackStatus(mpris::PlaybackStatus::Paused);
}

void MetaDataSharer::onPlay()
{
    std::cout << "[MetaDataSharer] Action: Play" << std::endl;
    if (player)
    {
        player->play();
    }
    setPlayBackStatus(mpris::PlaybackStatus::Playing);
}

void MetaDataSharer::onPlayPause()
{
    std::cout << "[MetaDataSharer] Action: PlayPause" << std::endl;
    if (player)
    {
        if (player->isPlaying())
        {
            player->pause();
        }
        else
        {
            player->play();
        }
    }
}

void MetaDataSharer::onStop()
{
    std::cout << "[MetaDataSharer] Action: Stop" << std::endl;
    if (player)
    {
        // player->stop();
    }
    setPlayBackStatus(mpris::PlaybackStatus::Stopped);
}

void MetaDataSharer::onSeek(int64_t offset)
{
    std::cout << "[MetaDataSharer] Action: Seek, Offset = " << offset << " us" << std::endl;
    if (player)
    {
        // 注意：MPRIS 的 Seek 是相对当前位置的偏移量
        // player->seekRelative(std::chrono::microseconds(offset));
    }
}

void MetaDataSharer::onSetPosition(int64_t position)
{
    std::cout << "[MetaDataSharer] Action: SetPosition, Pos = " << position << " us" << std::endl;
    if (player)
    {
        // MPRIS 的 SetPosition 是绝对位置
        // player->seekAbsolute(std::chrono::microseconds(position));
    }
}

#else // Not Linux

void MetaDataSharer::setPlayBackStatus(int status)
{
}
void MetaDataSharer::setPosition(std::chrono::microseconds position)
{
}

#endif

#ifdef _WIN32
void MetaDataSharer::onSeek(int64_t off)
{
}
#endif