#include "AudioPlayer.hpp"
#include "musiclistmodel.h"
#include "uicontroller.h"
#include "CoverImageProvider.hpp"
// 引入后端逻辑控制器
#include "MediaController.hpp"
#include "CoverCache.hpp"
#include "PCH.h"

// --- Linux Terminal Control Section ---
#if defined(Q_OS_LINUX)
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/select.h>

// 全局变量用于存储原始终端设置
// 必须是全局的，以便在 SIGTERM 信号处理函数中访问
static struct termios g_orig_termios;
static std::atomic<bool> g_terminal_modified(false);

#include <taglib/tdebuglistener.h> // 必须引入
#include <csignal>

// 1. 定义监听器类
class TrapDebugListener : public TagLib::DebugListener
{
public:
    void printMessage(const TagLib::String &msg) override
    {
        // // 打印到标准错误输出
        // std::cerr << "[TagLib Trap] " << msg << std::endl;

        // // 只要出现 Invalid UTF-8 就中断
        // if (msg.find("Invalid UTF-8") != -1)
        // {
        //     std::cerr << "!!! 捕捉到目标错误，触发断点 !!!" << std::endl;
        //     std::raise(SIGTRAP); // Linux 专用中断信号
        // }
#ifdef DEBUG
        spdlog::debug("[TagLib Trap] {}", msg.toCString());
#else

#endif
    }
};

// 恢复终端设置
void resetTerminalMode()
{
    if (g_terminal_modified.load())
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
}

// SIGTERM (kill) 处理函数
// 注意：信号处理函数中只能调用异步信号安全的函数 (Async-Signal-Safe)
void handleSigTerm(int signum)
{
    // 1. 紧急恢复终端设置，防止终端回显失效
    resetTerminalMode();

    // 2. 恢复默认处理并退出
    // 这里使用 _exit 而不是 exit，因为在信号处理函数中调用 exit 是不安全的
    _exit(signum);
}

// 设置终端为无缓冲模式（Raw Mode）
void setConioTerminalMode()
{
    struct termios new_termios;

    // 获取并保存当前终端属性
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    memcpy(&new_termios, &g_orig_termios, sizeof(new_termios));
    g_terminal_modified.store(true);

    // 禁用:
    // ICANON: 规范模式 (禁用行缓冲，按键即达，无需回车)
    // ECHO:   回显 (不显示输入的字符)
    // ISIG:   信号 (禁用 Ctrl+C/Z 发送 SIGINT/SIGTSTP 信号，让 Ctrl+C 变为字符 0x03)
    new_termios.c_lflag &= ~(ICANON | ECHO | ISIG);

    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;

    // 设置新属性
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
}

// 检查是否有键盘输入 (非阻塞)
int kbhit()
{
    struct timeval tv = {0L, 0L};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
}

// 读取一个字符
int getch()
{
    int r;
    unsigned char c;
    if ((r = read(STDIN_FILENO, &c, sizeof(c))) < 0)
    {
        return r;
    }
    else
    {
        return c;
    }
}

// void runColorExtractorTest();
void run_cover_test();

// 终端控制模式的主逻辑
void runTerminalMode(QCoreApplication &app, const QString &rootDir)
{
    // 注册 SIGTERM 处理函数
    signal(SIGTERM, handleSigTerm);

    // 实例化媒体控制器
    auto &mediaController = MediaController::getInstance();

    // 设置根目录并开始扫描
    if (!rootDir.isEmpty())
    {
        std::cout << "Setting root directory: " << rootDir.toStdString() << '\n';
        mediaController.setRootPath(rootDir.toStdString());
        auto start = std::chrono::high_resolution_clock::now();
        mediaController.startScan();

        // 简单的等待扫描开始
        std::cout << "Scanning...\n";
        while (!mediaController.isScanCplt())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Scan completed in " << duration.count() << " ms\n";
        std::cout << "Scan completed. Trying to auto-play...\n";
        mediaController.play();
        auto nowPlaying = mediaController.getCurrentPlayingNode();
#ifdef DEBUG
        int barwidth = 0;
        std::cout << "Waveform generation test 1 \n file path:/mnt/software/CloudMusic(for MP4)/R・I・O・T/RAISE A SUILEN - R·I·O·T.flac\n";
        start = std::chrono::high_resolution_clock::now();
        auto res = AudioPlayer::buildAudioWaveform("/mnt/software/CloudMusic(for MP4)/R・I・O・T/RAISE A SUILEN - R·I·O·T.flac", 70, 320, barwidth, 60, 0, 0);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Waveform generated in " << duration.count() << " ms\n";
        std::cout << "Waveform Data:\n";
        for (auto &i : res)
        {
            std::cout << i << " ";
        }
        std::cout << "\n";
        std::cout << "Waveform generation test 2 \n file path:/mnt/software/CloudMusic(for MP4)/Roselia/02. Ringing Bloom.m4a\n";
        start = std::chrono::high_resolution_clock::now();
        res = AudioPlayer::buildAudioWaveform("/mnt/software/CloudMusic(for MP4)/Roselia/02. Ringing Bloom.m4a", 70, 320, barwidth, 60, 0, 0);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Waveform generated in " << duration.count() << " ms\n";
        std::cout << "Waveform Data:\n";
        for (auto &i : res)
        {
            std::cout << i << " ";
        }
        std::cout << "\n";
#endif
        // run_cover_test();
        // runColorExtractorTest();
    }
    else
    {
        std::cerr << "Warning: No rootDir provided. Use --rootDir=\"/path/to/music\"\n";
    }

    std::cout << "==========================================\n";
    std::cout << "   Smallest Music Player - Terminal Mode  \n";
    std::cout << "==========================================\n";
    std::cout << " [p] Play/Pause\n";
    std::cout << " [s] Stop\n";
    std::cout << " [,] Previous Song\n";
    std::cout << " [.] Next Song\n";
    std::cout << " [q] or [Ctrl+C] Quit\n";
    std::cout << " [r] Random\n";
    std::cout << "==========================================\n";

    // 开启输入监听线程
    std::thread inputThread([&]()
                            {
        // 设置终端为 Raw Mode (同时禁用 SIGINT 信号)
        setConioTerminalMode();

        bool running = true;
        while (running) {
            // 检查输入
            if (kbhit()) {
                int c = getch();
                switch (c) {
                case 'p':
                    std::cout << "> Command: Play/Pause\n";
                    mediaController.playpluse(); 
                    break;
                case 's':
                    std::cout << "> Command: Stop\n";
                    mediaController.stop();
                    break;
                case ',':
                    std::cout << "> Command: Previous\n";
                    mediaController.prev();
                    break;
                case '.':
                    std::cout << "> Command: Next\n" ;
                    mediaController.next();
                    break;
                
                case 3: // ASCII 3 是 Ctrl+C (因为禁用了 ISIG)
                    std::cout << "> Ctrl+C Detected.\n";
                    [[fallthrough]]; 
                case 'q':
                    std::cout << "> Quitting...\n";
                    running = false;
                    app.quit(); // 触发 Qt 事件循环退出
                    break;
                case 'r':
                    std::cout << "> Command: random\n";
                    mediaController.setShuffle(!mediaController.getShuffle());
                default:
                    // 忽略其他按键
                    break;
                }
            }
            // 避免空转占用 CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // 线程结束前恢复终端
        resetTerminalMode(); });

    // 运行 Qt 事件循环 (处理定时器、音频回调等)
    int ret = app.exec();

    // 等待输入线程结束
    if (inputThread.joinable())
    {
        inputThread.join();
    }

    // 解除信号绑定 (虽然程序马上要退出了，但这是好习惯)
    signal(SIGTERM, SIG_DFL);
}
#endif

void initLogger()
{
    try
    {
        // 默认日志等级
        spdlog::level::level_enum default_level;
#ifdef DEBUG
        default_level = spdlog::level::debug;
#else
        default_level = spdlog::level::err;
#endif
        // 初始化日志系统
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        // 文件日志相关
        const std::string log_file_name = "applog.log";
        const size_t max_size = 1024 * 1024 * 10; // 10 MB
        const size_t max_files = 3;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file_name, max_size, max_files);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};

        // 创建并注册 logger
        auto logger = std::make_shared<spdlog::logger>(LOG_NAME.data(), sinks.begin(), sinks.end());

        // 5. 设置初始日志级别
        logger->set_level(default_level);

        // 注册全局 logger
        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::milliseconds(100));
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        std::cerr << "Global Logger initialization failed: " << ex.what() << "\n";
    }
}

// --- Main Entry Point ---

#if defined(Q_OS_WIN)
Q_DECL_EXPORT int qMain(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    TagLib::setDebugListener(new TrapDebugListener());
#ifdef DEBUG
    av_log_set_level(AV_LOG_INFO);
#endif
#ifdef RELEASE
    av_log_set_level(AV_LOG_QUIET);

#endif
    // qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));
#ifdef __linux__
    setenv("PULSE_PROP", "media.role=music", 0); // 0 表示不覆盖，如果已设置则保持
#endif

    // 1. 预解析参数以决定是否启用 GUI (仅 Linux)
    bool useGui = true;
    QString rootDir = "";

#if defined(Q_OS_LINUX)
    // 简单的参数扫描，避免在此时创建 QApplication
    for (int i = 1; i < argc; ++i)
    {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--no-gui")
        {
            useGui = false;
        }
        else if (arg.startsWith("--rootDir="))
        {
            rootDir = arg.mid(10); // 去掉 "--rootDir="
            // 去掉可能存在的引号
            if (rootDir.startsWith('"') && rootDir.endsWith('"'))
            {
                rootDir = rootDir.mid(1, rootDir.length() - 2);
            }
        }
    }
#endif

    // 创建logger
    initLogger();
    // 2. 分支处理：无 GUI 模式
    if (!useGui)
    {
#if defined(Q_OS_LINUX)
        // 使用 QCoreApplication，无图形界面依赖
        QCoreApplication app(argc, argv);
        app.setOrganizationName("MusicPlayer3");
        app.setApplicationName("MusicPlayer");

        runTerminalMode(app, rootDir);

        return 0;
#else
        // 非 Linux 平台强制回退到 GUI
        useGui = true;
#endif
    }

    // 3. 分支处理：GUI 模式 (原逻辑)
    if (useGui)
    {
        // QGuiApplication app(argc, argv);
        QApplication app(argc, argv);
        QQuickStyle::setStyle("Basic");
        QQmlApplicationEngine engine;
        app.setOrganizationName("MusicPlayer3");
        app.setApplicationName("MusicPlayer");
        app.setDesktopFileName("music-player");

        // 注册 Image Provider
        engine.addImageProvider(QStringLiteral("covercache"), new CoverImageProvider);

        // 实例化 UI 控制器
        UIController playerController;
        engine.rootContext()->setContextProperty("playerController", &playerController);

        // 实例化模型并加载初始数据
        MusicListModel *musicModel = new MusicListModel(&app);

        // 如果在 GUI 模式下也想利用 rootDir，可以在这里添加逻辑
        // if (!rootDir.isEmpty()) { ... }

        // musicModel->loadInitialData();
        engine.rootContext()->setContextProperty("musicListModel", musicModel);

        QObject::connect(
            &engine,
            &QQmlApplicationEngine::objectCreationFailed,
            &app,
            []()
            { QCoreApplication::exit(-1); },
            Qt::QueuedConnection);

        engine.loadFromModule("MusicPlayer", "Main");
        return app.exec();
    }

    return 0;
}