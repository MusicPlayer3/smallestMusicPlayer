#include "MetaDataSharer.hpp"
#include "SDL_log.h"

#ifdef __linux__

#endif

MetaDataSharer::MetaDataSharer(std::shared_ptr<AudioPlayer> player)
{
#ifdef __linux__
    // 创建 Server 实例
    server = mpris::Server::make("smallestMusicPlayer");

    if (!server)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "[MetaDataSharer] Error: Can't create MPRIS server. MPRIS disabled.\n");
        throw std::runtime_error("Can't create MPRIS server.");
    }

    this->player = std::move(player);

    // 基础信息设置
    server->set_identity("smallestMusicPlayer");
    // server->set_desktop_entry("smallestMusicPlayer"); // 需对应 .desktop 文件名
    server->set_supported_uri_schemes({"file"});
    server->set_supported_mime_types({"application/octet-stream", "audio/mpeg", "audio/flac", "audio/x-wav", "text/plain"});

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

    server->on_volume_changed([this](double volume)
                              { this->onVolumeChange(volume); });

    // Seek 传入 int64_t (Offset)
    server->on_seek([this](int64_t offset)
                    { this->onSeek(offset); });

    // SetPosition 传入 int64_t (Absolute Position)
    // 注意：mpris_server.hpp 中 set_position_method 会先检查 TrackId，然后调用此回调
    server->on_set_position([this](int64_t pos)
                            { std::cout<<"in lambda function, pos = "<<pos<<std::endl;this->onSetPosition(pos); });

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
    {
        std::cerr << "error! no mpris server!";
        return;
    }

    // 修复：使用 map<mpris::Field, sdbus::Variant> 构建元数据
    std::map<mpris::Field, sdbus::Variant> meta;

    // 1. Track ID (必须是 ObjectPath 类型)
    meta[mpris::Field::TrackId] = sdbus::Variant(sdbus::ObjectPath("/org/mpris/MediaPlayer2/Track/Current"));

    // 2. Title
    std::cout << "title:" << title << std::endl;
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
    setMetaData(metadata.getTitle(), std::vector<std::string>({metadata.getArtist()}), metadata.getAlbum(), "", metadata.getCoverPath(), metadata.getDuration());
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
    exit(0);
}

void MetaDataSharer::onNext()
{
    std::cout << "[MetaDataSharer] Action: Next" << std::endl;
}

void MetaDataSharer::onPrevious()
{
    std::cout << "[MetaDataSharer] Action: Previous" << std::endl;
}

void MetaDataSharer::onPause()
{
    std::cout << "[MetaDataSharer] Action: Pause" << std::endl;

    player->pause();

    setPlayBackStatus(mpris::PlaybackStatus::Paused);
}

void MetaDataSharer::onPlay()
{
    std::cout << "[MetaDataSharer] Action: Play" << std::endl;

    player->play();
    setPlayBackStatus(mpris::PlaybackStatus::Playing);
}

void MetaDataSharer::onPlayPause()
{
    std::cout << "[MetaDataSharer] Action: PlayPause" << std::endl;

    if (player->isPlaying())
    {
        player->pause();
    }
    else
    {
        player->play();
    }
}

void MetaDataSharer::onStop()
{
    std::cout << "[MetaDataSharer] Action: Stop" << std::endl;
    setPlayBackStatus(mpris::PlaybackStatus::Stopped);
}

void MetaDataSharer::onSeek(int64_t offset)
{
    std::cout << "[MetaDataSharer] Action: Seek, Offset = " << offset << " us" << std::endl;
    int64_t currentPos = player->getCurrentPositionMicroseconds();
    int64_t duration = player->getDurationMicroseconds();

    int64_t targetPos = currentPos + offset;
    // 3. 边界处理 (Clamp)
    if (targetPos < 0)
    {
        targetPos = 0;
    }
    else if (targetPos > duration)
    {
        targetPos = duration;
        // 有些播放器策略是跳到下一首，但标准行为通常是停在末尾或跳到末尾
    }
    player->seek(targetPos);
    server->set_position(targetPos);
}

void MetaDataSharer::onSetPosition(int64_t position)
{
    std::cout << "[MetaDataSharer] Action: SetPosition, Pos = " << position << " us" << std::endl;

    int64_t duration = player->getDurationMicroseconds();

    if (position < 0)
        position = 0;
    if (position > duration)
        position = duration;

    // 直接跳转
    player->seek(position);
    server->set_position(position);
}

void MetaDataSharer::onVolumeChange(double vol)
{
    player->setVolume(vol);
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