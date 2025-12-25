#include "SysMediaService.hpp"
#include "MediaController.hpp"
#include <sstream>
#include <iomanip>
#include <map>

// ==========================================
//  Linux 专用辅助函数
// ==========================================
#ifdef __linux__
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
#endif

// ==========================================
//  构造与析构
// ==========================================

SysMediaService::SysMediaService(MediaController &controller_) : controller(controller_)
{
#ifdef __linux__
    server = mpris::Server::make("org.mpris.MediaPlayer2.MusicPlayer");

    if (!server)
    {
        spdlog::error("[SysMediaService] Error: Can't create MPRIS server. MPRIS disabled.");
    }
    else
    {
        // 基础信息设置
        server->set_identity("MusicPlayer");
        server->set_desktop_entry("music-player"); // 需对应 .desktop 文件名
        server->set_supported_uri_schemes({"file"});
        server->set_supported_mime_types({"application/octet-stream", "audio/mpeg", "audio/flac", "audio/x-wav", "text/plain"});

        // 回调函数绑定
        server->on_quit([this]()
                        { this->onMprisQuit(); });
        server->on_next([this]()
                        { this->onMprisNext(); });
        server->on_previous([this]()
                            { this->onMprisPrevious(); });
        server->on_pause([this]()
                         { this->onMprisPause(); });
        server->on_play_pause([this]()
                              { this->onMprisPlayPause(); });
        server->on_stop([this]()
                        { this->onMprisStop(); });
        server->on_play([this]()
                        { this->onMprisPlay(); });
        server->on_loop_status_changed([this](mpris::LoopStatus status)
                                       { this->onMprisLoopStatusChanged(status); });
        server->on_shuffle_changed([this](bool shuffle)
                                   { this->onMprisShuffleChanged(shuffle); });
        server->on_volume_changed([this](double volume)
                                  { this->onMprisVolumeChanged(volume); });
        server->on_seek([this](int64_t offset)
                        { this->onMprisSeek(std::chrono::microseconds(offset)); });
        server->on_set_position([this](int64_t pos)
                                { this->onMprisSetPosition(std::chrono::microseconds(pos)); });

        server->start_loop_async();

        // 同步初始状态
        onVolumeChanged(controller.getVolume());
        onShuffleChanged(controller.getShuffle());
        onRepeatModeChanged(controller.getRepeatMode());
        auto *curr = controller.getCurrentPlayingNode();
        if (curr)
        {
            onTrackChanged(curr);
            onPlaybackStateChanged(controller.getIsPlaying());
        }
        else
        {
            setPlayBackStatus(mpris::PlaybackStatus::Stopped);
        }
    }
#endif

    // 无论 Linux 还是 Windows，都需要注册监听器
    controller.addListener(this);
}

SysMediaService::~SysMediaService()
{
    controller.removeListener(this);
}

// ==========================================
//  通用 Setter 实现
// ==========================================

void SysMediaService::setMetaData(const std::string &title, const std::vector<std::string> &artist,
                                  const std::string &album,
                                  const std::string &coverPath, int64_t duration, const std::string &uri)
{
#ifdef __linux__
    if (!server)
        return;

    std::map<mpris::Field, sdbus::Variant> meta;

    // 1. Track ID
    std::string uniqueIdStr = uri.empty() ? title : uri;
    std::size_t hash = std::hash<std::string>{}(uniqueIdStr);
    std::string trackIdPath = "/org/mpris/MediaPlayer2/Track/ID_" + std::to_string(hash);

    meta[mpris::Field::TrackId] = sdbus::Variant(sdbus::ObjectPath(trackIdPath));
    meta[mpris::Field::Title] = sdbus::Variant(title);
    meta[mpris::Field::Artist] = sdbus::Variant(artist);
    meta[mpris::Field::Album] = sdbus::Variant(album);

    if (!uri.empty())
    {
        meta[mpris::Field::Url] = sdbus::Variant(localPathToUri(uri));
    }

    if (!coverPath.empty())
    {
        meta[mpris::Field::ArtUrl] = sdbus::Variant(localPathToUri(coverPath));
    }

    if (duration > 0)
    {
        meta[mpris::Field::Length] = sdbus::Variant(duration);
    }

    server->set_metadata(meta);
#endif
}

void SysMediaService::setMetaData(const MetaData &metadata)
{
    setMetaData(metadata.getTitle(),
                std::vector<std::string>({metadata.getArtist()}),
                metadata.getAlbum(),
                metadata.getCoverPath(),
                metadata.getDuration(),
                metadata.getFilePath());
}

void SysMediaService::setPlayBackStatus(mpris::PlaybackStatus status)
{
#ifdef __linux__
    if (!server)
        return;
    server->set_playback_status(status);
#endif
}

void SysMediaService::setPosition(std::chrono::microseconds position)
{
#ifdef __linux__
    if (!server)
        return;
    server->set_position(position.count());
#endif
}

void SysMediaService::triggerSeeked(std::chrono::microseconds position)
{
#ifdef __linux__
    if (!server)
        return;
    int64_t pos = position.count();
    server->set_position(pos);
    server->send_seeked_signal(pos);
#endif
}

void SysMediaService::setLoopStatus(mpris::LoopStatus status)
{
#ifdef __linux__
    if (!server)
        return;
    server->set_loop_status(status);
#endif
}

void SysMediaService::setShuffle(bool shuffle)
{
#ifdef __linux__
    if (!server)
        return;
    server->set_shuffle(shuffle);
#endif
}

// ==========================================
//  Listener 接口实现 (内部 -> 外部)
// ==========================================

void SysMediaService::onPlaybackStateChanged(bool isPlaying)
{
    setPlayBackStatus(isPlaying ? mpris::PlaybackStatus::Playing : mpris::PlaybackStatus::Paused);
}

void SysMediaService::onTrackChanged(PlaylistNode *newNode)
{
    if (!newNode)
    {
        setPlayBackStatus(mpris::PlaybackStatus::Stopped);
        return;
    }
    setMetaData(newNode->getMetaData());
}

void SysMediaService::onMetadataChanged(PlaylistNode *node)
{
    if (node && node == controller.getCurrentPlayingNode())
    {
        setMetaData(node->getMetaData());
    }
}

void SysMediaService::onPositionChanged(int64_t microsec)
{
    setPosition(std::chrono::microseconds(microsec));
}

void SysMediaService::onVolumeChanged(double volume)
{
#ifdef __linux__
    if (server)
        server->set_volume(volume);
#endif
}

void SysMediaService::onShuffleChanged(bool shuffle)
{
    setShuffle(shuffle);
}

void SysMediaService::onRepeatModeChanged(RepeatMode mode)
{
    mpris::LoopStatus status = mpris::LoopStatus::None;
    switch (mode)
    {
    case RepeatMode::None: status = mpris::LoopStatus::None; break;
    case RepeatMode::Playlist: status = mpris::LoopStatus::Playlist; break;
    case RepeatMode::Single: status = mpris::LoopStatus::Track; break;
    }
    setLoopStatus(status);
}

// ==========================================
//  MPRIS 回调实现 (外部 -> 内部)
// ==========================================

#ifdef __linux__
void SysMediaService::onMprisQuit()
{
    spdlog::info("[SysMediaService] Quit signal received.");
    // 可以在这里处理退出逻辑
}

void SysMediaService::onMprisNext()
{
    spdlog::info("[SysMediaService] Next signal received.");
    controller.next();
}

void SysMediaService::onMprisPrevious()
{
    spdlog::info("[SysMediaService] Previous signal received.");
    controller.prev();
}

void SysMediaService::onMprisPause()
{
    spdlog::info("[SysMediaService] Pause signal received.");
    controller.pause();
}

void SysMediaService::onMprisPlayPause()
{
    spdlog::info("[SysMediaService] PlayPause signal received.");
    controller.playpluse();
}

void SysMediaService::onMprisStop()
{
    spdlog::info("[SysMediaService] Stop signal received.");
    controller.stop();
}

void SysMediaService::onMprisPlay()
{
    spdlog::info("[SysMediaService] Play signal received.");
    controller.play();
    setPlayBackStatus(mpris::PlaybackStatus::Playing);
}

void SysMediaService::onMprisLoopStatusChanged(mpris::LoopStatus status)
{
    const char *statusStr =
        (status == mpris::LoopStatus::None)     ? "None" :
        (status == mpris::LoopStatus::Playlist) ? "Playlist" :
        (status == mpris::LoopStatus::Track)    ? "Track" :
                                                  "Unknown";
    spdlog::info("[SysMediaService] LoopStatusChanged signal received. status:{}", statusStr);

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

void SysMediaService::onMprisShuffleChanged(bool shuffle)
{
    spdlog::info("[SysMediaService] ShuffleChanged signal received. status:{}", shuffle);
    controller.setShuffle(shuffle);
}

void SysMediaService::onMprisVolumeChanged(double volume)
{
    spdlog::info("[SysMediaService] VolumeChanged signal received. volume:{}", volume);
    controller.setVolume(volume);
}

void SysMediaService::onMprisSeek(std::chrono::microseconds offset)
{
    spdlog::info("[SysMediaService] Seek signal received. offset:{}", offset.count());
    int64_t currentPos = controller.getCurrentPosMicroseconds();
    int64_t duration = controller.getDurationMicroseconds();

    int64_t targetPos = currentPos + offset.count();

    if (targetPos < 0)
        targetPos = 0;
    else if (targetPos > duration)
        targetPos = duration;

    controller.seek(targetPos);

    // 立即更新服务端状态，增加响应感
    if (server)
        server->set_position(targetPos);
}

void SysMediaService::onMprisSetPosition(std::chrono::microseconds pos)
{
    spdlog::info("[SysMediaService] SetPosition signal received. pos:{}", pos.count());
    int64_t duration = controller.getDurationMicroseconds();
    int64_t position = pos.count();

    if (position < 0)
        position = 0;
    if (position > duration)
        position = duration;

    controller.seek(position);

    if (server)
        server->set_position(position);
}
#endif