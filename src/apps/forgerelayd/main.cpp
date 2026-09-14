// forgerelayd main.cpp - 服务端守护进程入口（Application 层，§3.2）。
// 职责仅限：配置加载/校验（CFG-01/02）、组件组装、信号驱动的优雅退出（LIFE-05）。
#include <atomic>
#include <csignal>
#include <cstring>
#include <thread>
#include <iostream>
#include <memory>
#include <string>

#include "forgerelay/config.hpp"
#include "forgerelay/log.hpp"
#include "forgerelay/service.hpp"
#include "forgerelay/storage/error.hpp"
#include "forgerelay/storage/storage.hpp"
#include "forgerelay/transport.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD type)
{
    (void)type;
    g_stop.store(true);
    return TRUE;
}
#else
void term_handler(int)
{
    g_stop.store(true);
}
#endif

void install_signal_handlers()
{
#if defined(_WIN32)
    (void)SetConsoleCtrlHandler(console_handler, TRUE);
#else
    (void)std::signal(SIGTERM, term_handler);
    (void)std::signal(SIGINT, term_handler);
#endif
}

void print_usage()
{
    std::cout << "forgerelayd - ForgeRelay artifact cache server\n"
                 "usage:\n"
                 "  forgerelayd --config <path> [--listen host:port] [--log-level level]\n"
                 "  forgerelayd --create-admin <name> --config <path>\n"
                 "               (bootstrap: create admin user, print a one-time token)\n"
                 "examples:\n"
                 "  forgerelayd --config /etc/forgerelay/server.toml\n";
}

/** 引导：创建 Admin 用户并打印一次性令牌（FR-AUTH-03）。 */
int create_admin(const std::string &config_path, const std::string &name)
{
    const fr::ServerSettings settings = fr::load_server_config(config_path);
    const std::string db = settings.database_path.empty()
                               ? (settings.storage.root / "metadata.db").string()
                               : settings.database_path;
    std::filesystem::create_directories(settings.storage.root);
    fr::AuthRegistry auth(db);
    if (!auth.has_no_users()) {
        std::cerr << "error: users already exist; use an existing admin token\n";
        return 1;
    }
    auth.user_add(name, fr::Role::Admin);
    const std::string token = auth.token_create(name, 0);
    std::cout << "admin user '" << name << "' created.\n"
              << "token (shown once, store it safely):\n"
              << token << "\n";
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    std::string config_path;
    std::string listen_override;
    std::string log_level_override;
    std::string create_admin_name;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--listen" && i + 1 < argc) {
            listen_override = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            log_level_override = argv[++i];
        } else if (arg == "--create-admin" && i + 1 < argc) {
            create_admin_name = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }
    if (config_path.empty()) {
        std::cerr << "error: --config is required\n";
        print_usage();
        return 2;
    }

    try {
        if (!create_admin_name.empty()) {
            return create_admin(config_path, create_admin_name);
        }

        fr::ServerSettings settings = fr::load_server_config(config_path); // CFG-01
        fr::apply_cli_overrides(settings, listen_override, log_level_override); // CFG-02
#if !defined(FR_HAVE_OPENSSL)
        if (settings.tls.enabled) {
            std::cerr << "error: TLS backend unavailable in this build (no OpenSSL); "
                         "use a loopback listener with tls.enabled = false\n";
            return 1;
        }
#endif

        fr::Logger logger;
        logger.open(settings.log.file, fr::LogLevel::Info);
        fr::Logger *log = &logger;

        auto storage = fr::Storage::open(settings.storage);
        const std::string db_path = settings.database_path.empty()
                                        ? (settings.storage.root / "metadata.db").string()
                                        : settings.database_path;
        auto auth = std::make_unique<fr::AuthRegistry>(db_path);

        auto hub = std::make_shared<fr::StatusHub>();
        fr::Dispatcher dispatcher(*storage, *auth, settings, *log, hub);
        auto server = fr::Server::start(settings, *storage, dispatcher, *log, hub.get());
        log->info("forgerelayd started (M3/M4)");

        install_signal_handlers();
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        log->info("stop signal received, shutting down (LIFE-05)");
        server->stop();
        log->info("forgerelayd stopped");
        return 0;
    } catch (const fr::Error &err) {
        std::cerr << "fatal: [" << fr_status_name(err.code()) << "] " << err.what() << "\n";
        return 1;
    } catch (const std::exception &err) {
        std::cerr << "fatal: " << err.what() << "\n";
        return 1;
    }
}
