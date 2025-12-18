#include "AudioPlayer.hpp"
#include "musiclistmodel.h"
#include "uicontroller.h"
#include "CoverImageProvider.hpp"
#include "MediaController.hpp"
#include "CoverCache.hpp"
#include "PCH.h"
#include "SimpleThreadPool.hpp"

// =========================================================
//  Linux Terminal Control & Signal Handling
// =========================================================
#if defined(Q_OS_LINUX)
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/select.h>
#include <taglib/tdebuglistener.h>
#include <csignal>

// 全局变量：用于存储原始终端设置 (必须是全局以便信号处理访问)
static struct termios g_orig_termios;
static std::atomic<bool> g_terminal_modified(false);

// TagLib 调试监听器 (用于捕获底层库错误)
class TrapDebugListener : public TagLib::DebugListener
{
public:
    void printMessage(const TagLib::String &msg) override
    {
#ifdef DEBUG
        spdlog::debug("[TagLib Trap] {}", msg.toCString());
#endif
    }
};

// 恢复终端设置 (Canonical Mode)
void resetTerminalMode()
{
    if (g_terminal_modified.load())
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
}

// 信号处理: 确保被 Kill 时恢复终端显示
void handleSigTerm(int signum)
{
    resetTerminalMode();
    _exit(signum); // 信号处理中只能使用 _exit
}

// 设置终端为 Raw Mode (无缓冲、无回显)
void setConioTerminalMode()
{
    struct termios new_termios;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    memcpy(&new_termios, &g_orig_termios, sizeof(new_termios));
    g_terminal_modified.store(true);

    // 禁用规范模式、回显和信号字符处理
    new_termios.c_lflag &= ~(ICANON | ECHO | ISIG);
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
}

// 非阻塞检测键盘输入
int kbhit()
{
    struct timeval tv = {0L, 0L};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
}

// 读取单个字符
int getch()
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, sizeof(c)) < 0)
        return -1;
    return c;
}

// --- 终端模式主逻辑 ---
void runTerminalMode(QCoreApplication &app, const QString &rootDir)
{
    // 1. 注册信号处理
    signal(SIGTERM, handleSigTerm);

    // 2. [关键修复] 初始化 MediaController
    // 必须在 getInstance() 之前调用，否则会抛出 runtime_error
    spdlog::info("Initializing MediaController (Terminal Mode)...");
    MediaController::init();

    // 获取实例引用
    auto &mediaController = MediaController::getInstance();

    // 3. 扫描逻辑
    if (!rootDir.isEmpty())
    {
        std::cout << "Setting root directory: " << rootDir.toStdString() << '\n';
        mediaController.setRootPath(rootDir.toStdString());

        auto start = std::chrono::high_resolution_clock::now();
        mediaController.startScan();

        std::cout << "Scanning...\n";
        // 简单的自旋等待，避免完全阻塞主线程事件循环（如果需要的话）
        while (!mediaController.isScanCplt())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Scan completed in " << duration.count() << " ms\n";

        std::cout << "Attempting to auto-play...\n";
        mediaController.play();
    }
    else
    {
        std::cerr << "Warning: No rootDir provided. Use --rootDir=\"/path/to/music\"\n";
    }

    // 4. 显示菜单
    std::cout << "==========================================\n";
    std::cout << "   Smallest Music Player - Terminal Mode  \n";
    std::cout << "==========================================\n";
    std::cout << " [p] Play/Pause    [s] Stop\n";
    std::cout << " [,] Previous      [.] Next\n";
    std::cout << " [r] Toggle Random\n";
    std::cout << " [q] Quit\n";
    std::cout << "==========================================\n";

    // 5. 开启键盘输入监听线程
    std::thread inputThread([&]()
                            {
        setConioTerminalMode();
        bool running = true;
        
        while (running) {
            if (kbhit()) {
                int c = getch();
                switch (c) {
                case 'p':
                    std::cout << "> Play/Pause\n";
                    mediaController.playpluse(); 
                    break;
                case 's':
                    std::cout << "> Stop\n";
                    mediaController.stop();
                    break;
                case ',':
                    std::cout << "> Previous\n";
                    mediaController.prev();
                    break;
                case '.':
                    std::cout << "> Next\n";
                    mediaController.next();
                    break;
                case 'r':
                    std::cout << "> Toggle Random\n";
                    mediaController.setShuffle(!mediaController.getShuffle());
                    break;
                case 3: // Ctrl+C
                case 'q':
                    std::cout << "> Quitting...\n";
                    running = false;
                    app.quit(); // 退出 Qt 事件循环
                    break;
                default:
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        resetTerminalMode(); });

    // 6. 运行 Qt 事件循环 (处理音频回调、定时器等)
    app.exec();

    // 7. 清理资源
    if (inputThread.joinable())
    {
        inputThread.join();
    }

    // [关键修复] 显式销毁控制器和线程池
    spdlog::info("Shutting down MediaController...");
    MediaController::destroy();
    SimpleThreadPool::instance().shutdown();

    // 恢复信号处理
    signal(SIGTERM, SIG_DFL);
}
#endif

// =========================================================
//  Logger Initialization
// =========================================================
void initLogger()
{
    try
    {
        spdlog::level::level_enum default_level = spdlog::level::err;
#ifdef DEBUG
        default_level = spdlog::level::debug;
#endif

        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        const std::string log_file_name = "applog.log";
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file_name, 1024 * 1024 * 10, 3);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        auto logger = std::make_shared<spdlog::logger>(LOG_NAME.data(), sinks.begin(), sinks.end());

        logger->set_level(default_level);
        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::milliseconds(100));
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        std::cerr << "Global Logger initialization failed: " << ex.what() << "\n";
    }
}

// =========================================================
//  Main Entry Point
// =========================================================

#if defined(Q_OS_WIN)
Q_DECL_EXPORT int qMain(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
#if TAGLIB_MAJOR_VERSION >= 2
    TagLib::setDebugListener(new TrapDebugListener());
#endif

#ifdef DEBUG
    av_log_set_level(AV_LOG_INFO);
#elif defined(RELEASE)
    av_log_set_level(AV_LOG_QUIET);
#endif

#ifdef __linux__
    // 告诉 PulseAudio 这个流是音乐，以便正确处理音量和策略
    setenv("PULSE_PROP", "media.role=music", 0);
#endif

    // 1. 初始化日志
    initLogger();

    // 2. 解析参数
    bool useGui = true;
    QString rootDir = "";

#if defined(Q_OS_LINUX)
    for (int i = 1; i < argc; ++i)
    {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--no-gui")
        {
            useGui = false;
        }
        else if (arg.startsWith("--rootDir="))
        {
            rootDir = arg.mid(10);
            if (rootDir.startsWith('"') && rootDir.endsWith('"'))
                rootDir = rootDir.mid(1, rootDir.length() - 2);
        }
    }
#endif

    // =========================================================
    //  Mode: Terminal (Linux Only)
    // =========================================================
    if (!useGui)
    {
#if defined(Q_OS_LINUX)
        QCoreApplication app(argc, argv); // 无 GUI 上下文
        app.setOrganizationName("MusicPlayer3");
        app.setApplicationName("MusicPlayer");

        runTerminalMode(app, rootDir);

        // 清理 spdlog
        spdlog::drop_all();
        spdlog::shutdown();
        return 0;
#else
        // Windows/Mac 强制回退到 GUI
        useGui = true;
#endif
    }

    // =========================================================
    //  Mode: GUI (Qt Quick)
    // =========================================================
    if (useGui)
    {
        QApplication app(argc, argv);
        QQuickStyle::setStyle("Basic");
        app.setOrganizationName("MusicPlayer3");
        app.setApplicationName("MusicPlayer");

        spdlog::info("Initializing MediaController (GUI Mode)...");
        MediaController::init();

        QQmlApplicationEngine engine;
        engine.addImageProvider(QStringLiteral("covercache"), new CoverImageProvider);

        UIController playerController;
        engine.rootContext()->setContextProperty("playerController", &playerController);

        MusicListModel *musicModel = new MusicListModel(&app);
        engine.rootContext()->setContextProperty("musicListModel", musicModel);

        // 连接退出信号，确保资源安全释放
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]()
                         {
            spdlog::info("Application about to quit. Starting cleanup...");
            playerController.prepareForQuit();
            
            MediaController::destroy();
            SimpleThreadPool::instance().shutdown();
            
            spdlog::info("Cleanup sequence finished."); });

        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, []()
                         { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

        engine.loadFromModule("MusicPlayer", "Main");

        int ret = app.exec();

        // 双重保险清理
        MediaController::destroy();
        SimpleThreadPool::instance().shutdown();
        spdlog::drop_all();
        spdlog::shutdown();

        return ret;
    }

    return 0;
}