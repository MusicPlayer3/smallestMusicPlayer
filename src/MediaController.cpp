#include "MediaController.hpp"
#include "FileScanner.hpp"
#include "SysMediaService.hpp"
#include <memory>

#ifdef DEBUG

void MediaController::initPlayList()
{
}

MediaController::MediaController()
{
    player = std::make_shared<AudioPlayer>();
    mediaService = std::make_shared<SysMediaService>(*this);
    scanner = std::make_unique<FileScanner>();
}

void MediaController::play()
{
    player->play();
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Playing);
}

void MediaController::pause()
{
    player->pause();
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Paused);
}

void MediaController::stop()
{
    // TODO: stop()
    mediaService->setPlayBackStatus(mpris::PlaybackStatus::Stopped);
}

void MediaController::next()
{
    // TODO: next()
}

void MediaController::prev()
{
    // TODO: prev()
}

#endif