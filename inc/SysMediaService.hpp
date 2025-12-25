#ifndef _SYSTEM_MEDIA_SERVICE_HPP_
#define _SYSTEM_MEDIA_SERVICE_HPP_

#include "MediaController.hpp"
#ifdef __linux__
#include "mpris_server.hpp"
#include <sdbus-c++/sdbus-c++.h>
#endif

#include "PCH.h"

#ifdef __WIN32__
namespace mpris
{
enum class PlaybackStatus
{
    Playing,
    Paused,
    Stopped
};
enum class LoopStatus
{
    None,
    Track,
    Playlist
};
} // namespace mpris
#endif

class SysMediaService : public IMediaControllerListener
{
    MediaController &controller;

#ifdef __linux__
    std::unique_ptr<mpris::Server> server;

    // --- MPRIS 回调 (外部控制内部) ---
    void onMprisQuit();
    void onMprisNext();
    void onMprisPrevious();
    void onMprisPause();
    void onMprisPlayPause();
    void onMprisStop();
    void onMprisPlay();
    void onMprisLoopStatusChanged(mpris::LoopStatus status);
    void onMprisShuffleChanged(bool shuffle);
    void onMprisVolumeChanged(double vol);
    void onMprisSeek(std::chrono::microseconds offset);
    void onMprisSetPosition(std::chrono::microseconds position);

    static std::string localPathToUri(const std::string &path);
#endif

public:
    SysMediaService(MediaController &controller);
    ~SysMediaService();

    // --- IMediaControllerListener 接口 (内部同步外部) ---
    void onPlaybackStateChanged(bool isPlaying) override;
    void onTrackChanged(PlaylistNode *newNode) override;
    void onMetadataChanged(PlaylistNode *node) override;
    void onPositionChanged(int64_t microsec) override;
    void onVolumeChanged(double volume) override;
    void onShuffleChanged(bool shuffle) override;
    void onRepeatModeChanged(RepeatMode mode) override;
    void onScanFinished() override
    {
    }

    // --- 通用接口 ---
    void setMetaData(const MetaData &metadata);
    void setPlayBackStatus(mpris::PlaybackStatus status);
    void setPosition(std::chrono::microseconds position);
    void triggerSeeked(std::chrono::microseconds position);
    void setLoopStatus(mpris::LoopStatus status);
    void setShuffle(bool shuffle);

    void setMetaData(const std::string &title, const std::vector<std::string> &artist,
                     const std::string &album,
                     const std::string &coverPath = "", int64_t duration = 0, const std::string &uri = "");
};

#endif