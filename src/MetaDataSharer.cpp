#include "MetaDataSharer.hpp"

#ifdef __linux__

MetaDataSharer::MetaDataSharer(std::shared_ptr<AudioPlayer> player)
{
    this->player = std::move(player);
    server = mpris::Server::make("smallestMusicPlayer");
    if (!server)
    {
        std::cerr << "can't connect to dbus\n";
        // TODO:dbus无法打开时的错误处理
        exit(1);
    }
    server->set_identity("smallestMusicPlayer written by kaizen857");
    server->set_supported_uri_schemes({"file"});
    server->set_supported_mime_types({"application/octet-stream", "text/plain"});
    
}

#endif