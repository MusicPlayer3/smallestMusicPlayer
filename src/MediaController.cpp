#include "MediaController.hpp"

#ifdef DEBUG

void MediaController::initPlayList()
{
    // TODO: 初始化用来debug的播放列表
    scanner->startScan();
    while (!scanner->isScanCompleted())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    playlist = scanner->getItems();
    for (const auto &i : playlist)
    {
        std::cout << "Title: " << i.getTitle() << "\n"
                  << "Artist: " << i.getArtist() << "\n"
                  << "Album: " << i.getAlbum() << "\n"
                  << "Year: " << i.getYear() << "\n"
                  << "File Path: " << i.getFilePath() << "\n"
                  << "Parent Directory: " << i.getParentDir() << "\n";
    }
    
}

#endif