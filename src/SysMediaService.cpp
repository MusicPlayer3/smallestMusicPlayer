#include "SysMediaService.hpp"
#include "MediaController.hpp"
#include <functional> // for std::hash

#ifdef __linux__
SysMediaService::SysMediaService(MediaController &controller_) : controller(controller_)
{
    server = mpris::Server::make("org.mpris.MediaPlayer2.SysMediaService");

    if (!server)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "[SysMediaService] Error: Can't create MPRIS server. MPRIS disabled.\n");
        return;
    }

    // 基础信息设置
    server->set_identity("MusicPlayer");
    server->set_desktop_entry("music-player"); // 需对应 .desktop 文件名
    server->set_supported_uri_schemes({"file"});
    server->set_supported_mime_types({"application/octet-stream", "audio/mpeg", "audio/flac", "audio/x-wav", "text/plain"});

    // 回调函数绑定
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

    server->on_loop_status_changed([this](mpris::LoopStatus status)
                                   { this->onLoopStatusChanged(status); });

    server->on_shuffle_changed([this](bool shuffle)
                               { this->onShuffleChanged(shuffle); });

    server->on_volume_changed([this](double volume)
                              { this->onVolumeChanged(volume); });

    server->on_seek([this](int64_t offset)
                    { this->onSeek(std::chrono::microseconds(offset)); });

    server->on_set_position([this](int64_t pos)
                            { this->onSetPosition(std::chrono::microseconds(pos)); });

    server->start_loop_async();
}

void SysMediaService::setMetaData(const std::string &title, const std::vector<std::string> &artist,
                                  const std::string &album,
                                  const std::string &coverPath, int64_t duration, const std::string &uri)
{
    if (!server)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "[SysMediaService] Error: MPRIS server not initialized.\n");
        return;
    }

    // 修复：使用 map<mpris::Field, sdbus::Variant> 构建元数据
    std::map<mpris::Field, sdbus::Variant> meta;

    // 1. Track ID (关键修复：必须唯一，否则客户端不会重置进度)
    // 使用 URI 的哈希值生成唯一的 TrackID
    std::string uniqueIdStr = uri.empty() ? title : uri;
    std::size_t hash = std::hash<std::string>{}(uniqueIdStr);
    std::string trackIdPath = "/org/mpris/MediaPlayer2/Track/ID_" + std::to_string(hash);

    meta[mpris::Field::TrackId] = sdbus::Variant(sdbus::ObjectPath(trackIdPath));

    // 2. Title
    meta[mpris::Field::Title] = sdbus::Variant(title);

    // 3. Artist (MPRIS 标准要求是字符串列表 "as")
    meta[mpris::Field::Artist] = sdbus::Variant(artist);

    // 4. Album
    meta[mpris::Field::Album] = sdbus::Variant(album);

    if (!uri.empty())
    { // 5. URL
        meta[mpris::Field::Url] = sdbus::Variant(localPathToUri(uri));
    }

    // 6. Art URL (封面)
    if (!coverPath.empty())
    {
        meta[mpris::Field::ArtUrl] = sdbus::Variant(localPathToUri(coverPath));
    }

    // 7. Length (单位微秒，int64_t)
    if (duration > 0)
    {
        meta[mpris::Field::Length] = sdbus::Variant(duration);
    }

    // 调用 set_metadata
    server->set_metadata(meta);
}

void SysMediaService::setMetaData(const MetaData &metadata)
{
    setMetaData(metadata.getTitle(), std::vector<std::string>({metadata.getArtist()}), metadata.getAlbum(), metadata.getCoverPath(), metadata.getDuration(), metadata.getFilePath());
}

std::string SysMediaService::localPathToUri(const std::string &path)
{
    if (path.empty())
        return "";
    if (path.find("file://") == 0 || path.find("http") == 0)
        return path;

    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : path)
    {
        // 保留字符：字母、数字、安全符号
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == '~')
        {
            escaped << c;
        }
        else
        {
            // URL 编码
            escaped << '%' << std::setw(2) << int(c);
        }
    }

    std::string uri = "file://";
    if (!path.empty() && path[0] != '/')
    {
        uri += "/";
    }
    uri += escaped.str();
    return uri;
}

void SysMediaService::setPlayBackStatus(mpris::PlaybackStatus status)
{
    if (!server)
    {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "[SysMediaService] Error: MPRIS server not initialized.\n");
        return;
    }
    server->set_playback_status(status);
}

void SysMediaService::triggerSeeked(std::chrono::microseconds position)
{
    if (!server)
        return;

    int64_t pos = position.count();
    // 1. 更新内部状态，防止客户端随后查询到旧时间
    server->set_position(pos);
    // 2. 发送信号通知客户端重置
    server->send_seeked_signal(pos);
}

void SysMediaService::onQuit()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] Quit signal received.\n");
    // TODO:quit
}

void SysMediaService::onNext()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] Next signal received.\n");
    //  next
    controller.next();
}

void SysMediaService::onPrevious()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] Previous signal received.\n");
    //  previous
    controller.prev();
}

void SysMediaService::onPause()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] Pause signal received.\n");
    // pause
    controller.pause();
}

void SysMediaService::onPlayPause()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] PlayPause signal received.\n");
    //  playPause
    controller.playpluse();
}

void SysMediaService::onStop()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] Stop signal received.\n");
    //  stop
    controller.stop();
}

void SysMediaService::onPlay()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] Play signal received.\n");
    //  play
    controller.play();
    setPlayBackStatus(mpris::PlaybackStatus::Playing);
}

void SysMediaService::onLoopStatusChanged(mpris::LoopStatus status)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] LoopStatusChanged signal received: %d\n", status);
    if (status == mpris::LoopStatus::None)
    {
        controller.setRepeatMode(RepeatMode::None);
    }
    else if (status == mpris::LoopStatus::Playlist)
    {
        controller.setRepeatMode(RepeatMode::Playlist);
    }
    else if (status == mpris::LoopStatus::Track)
    {
        controller.setRepeatMode(RepeatMode::Single);
    }
}

void SysMediaService::onShuffleChanged(bool shuffle)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] ShuffleChanged signal received: %d\n", shuffle);
    controller.setShuffle(shuffle);
}

void SysMediaService::onVolumeChanged(double volume)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] VolumeChanged signal received: %f\n", volume);
    //  volumeChanged
    controller.setVolume(volume);
}

void SysMediaService::onSeek(std::chrono::microseconds offset)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] Seek signal received: %lld\n", offset.count());
    //  seek
    int64_t currentPos = controller.getCurrentPosMicroseconds();
    int64_t duration = controller.getDurationMicroseconds();

    int64_t targetPos = currentPos + offset.count();
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
    controller.seek(targetPos);
    server->set_position(targetPos);
}

void SysMediaService::onSetPosition(std::chrono::microseconds pos)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[SysMediaService] SetPosition signal received: %lld\n", pos.count());
    // setPosition
    int64_t duration = controller.getDurationMicroseconds();
    int64_t position = pos.count();
    if (position < 0)
        position = 0;
    if (position > duration)
        position = duration;

    // 直接跳转
    controller.seek(position);
    server->set_position(position);
}

void SysMediaService::setShuffle(bool shuffle)
{
    if (server)
    {
        server->set_shuffle(shuffle);
    }
}
#endif

#ifdef __WIN32__
void SysMediaService::triggerSeeked(std::chrono::microseconds position)
{
}

void SysMediaService::setPlayBackStatus(mpris::PlaybackStatus status)
{
}

void SysMediaService::setShuffle(bool shuffle)
{
}
void SysMediaService::setMetaData(const MetaData &metadata)
{
    // setMetaData(metadata.getTitle(), std::vector<std::string>({metadata.getArtist()}), metadata.getAlbum(), metadata.getCoverPath(), metadata.getDuration(), metadata.getFilePath());
}
#endif