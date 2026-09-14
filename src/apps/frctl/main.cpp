// frctl main.cpp - 命令行客户端入口（§10、CLI-01..04）。
//
// 退出码（CLI-03，见 fr/client.hpp ExitCode）：
//   0 成功 | 2 参数错误 | 3 认证失败 | 4 权限不足 | 5 网络 | 6 服务端 | 7 本地文件
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "forgerelay/client.hpp"
#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"

namespace {

struct Globals {
    std::string host = "127.0.0.1";
    uint16_t port = 7443;
    int timeout = 30;
    std::string token;
    bool json = false;
    bool verbose = false;
    bool confirm = false;
    bool use_tls = false;    // --tls/--no-tls 显式指定
    bool tls_explicit = false; // 用户是否显式指定 TLS 开关
} g;

struct CommandError {
    int code;
    std::string message;
};

[[noreturn]] void usage_error(const std::string &message)
{
    throw CommandError{2, message};
}

fr::ClientOptions client_options()
{
    fr::ClientOptions options;
    options.host = g.host;
    options.port = g.port;
    options.timeout_seconds = g.timeout;
    if (g.token.empty()) {
        const char *env = std::getenv("FR_TOKEN");
        if (env != nullptr) {
            g.token = env;
        }
    }
    options.token = g.token;
#if defined(FR_HAVE_OPENSSL)
    /* §5.1：远程默认 TLS（D-20）。--no-tls 显式关闭（回环调试场景）。 */
    options.use_tls = true; // §5.1 远程默认 TLS；--no-tls 显式关闭（D-20）
#endif
    if (g.tls_explicit) {
        options.use_tls = g.use_tls;
    }
    return options;
}

/** 上传进度：按块打印（CLI-04；--json 时不输出动态进度）。 */
class ProgressPrinter {
public:
    explicit ProgressPrinter(uint64_t total, bool enabled)
        : total_(total), enabled_(enabled)
    {
    }

    void update(uint64_t done)
    {
        if (!enabled_) {
            return;
        }
        const int percent = total_ == 0 ? 100 : static_cast<int>(done * 100 / total_);
        std::cout << "\r" << percent << "% (" << done << "/" << total_ << ")" << std::flush;
        if (done >= total_) {
            std::cout << "\n";
        }
    }

private:
    uint64_t total_;
    bool enabled_;
};

/** 把本地文件切块上传（FR-UP 流程；互斥锁保护 Windows CRT 全局 i/o）。 */
void cmd_upload(const std::string &file, const std::string &ns, const std::string &name,
                const std::string &version)
{
    std::ifstream input(file, std::ios::binary);
    if (!input.is_open()) {
        throw CommandError{7, "cannot open local file: " + file};
    }
    input.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(input.tellg());
    input.seekg(0);

    /* 流式计算整体摘要（TEST-03：不整载）。 */
    fr::Sha256Stream digest_stream;
    std::vector<char> buffer(1024ull * 1024);
    uint64_t hashed = 0;
    while (hashed < size) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const size_t got = static_cast<size_t>(input.gcount());
        if (got == 0) {
            throw CommandError{7, "read failed: " + file};
        }
        digest_stream.update(buffer.data(), got);
        hashed += got;
    }
    const std::string digest = fr::sha256_to_hex(digest_stream.finish());
    input.clear();
    input.seekg(0);

    fr::Client client = fr::Client(client_options());
    client.connect();
    const fr::msg::CreateUploadOk session =
        client.create_upload(ns, name, version, size, digest);
    /* 按服务端会话的块大小读取（否则读入固定 1 MiB 缓冲会溢出——冒烟测试发现）。 */
    buffer.resize(session.chunk_size);

    ProgressPrinter progress(size, !g.json);
    uint64_t ordinal = 0;
    uint64_t sent = 0;
    while (sent < size) {
        const uint64_t want = size - sent < session.chunk_size ? size - sent : session.chunk_size;
        input.read(buffer.data(), static_cast<std::streamsize>(want));
        const size_t got = static_cast<size_t>(input.gcount());
        if (got != want) {
            throw CommandError{7, "read failed: " + file};
        }
        (void)client.put_chunk(session.session_id, ordinal, buffer.data(), got);
        sent += got;
        ordinal++;
        progress.update(sent);
    }
    const fr::msg::CommitUploadOk published = client.commit_upload(session.session_id);
    if (g.json) {
        std::cout << "{\"ok\":true,\"ns\":\"" << published.ref.ns << "\",\"name\":\""
                  << published.ref.name << "\",\"version\":\"" << published.ref.version
                  << "\",\"size\":" << published.size << ",\"digest\":\"" << published.digest
                  << "\"}\n";
    } else {
        std::cout << "published " << published.ref.ns << "/" << published.ref.name << ":"
                  << published.ref.version << " (" << published.size << " bytes, "
                  << published.digest.substr(0, 16) << "…)\n";
    }
}

void cmd_download(const std::string &ref, const std::string &output, bool resume)
{
    const size_t slash = ref.find('/');
    const size_t colon = ref.rfind(':');
    if (slash == std::string::npos || colon == std::string::npos || colon < slash) {
        usage_error("artifact ref must be <ns>/<name>:<version>");
    }
    const std::string ns = ref.substr(0, slash);
    const std::string name = ref.substr(slash + 1, colon - slash - 1);
    const std::string version = ref.substr(colon + 1);

    fr::Client client = fr::Client(client_options());
    client.connect();
    const fr::msg::ShowArtifactOk info = client.show_artifact(ns, name, version);
    uint64_t offset = 0;
    uint64_t length = info.info.size;

    if (resume) {
        /* FR-DL-05：按本地临时文件长度续传剩余范围。 */
        std::error_code ec;
        const uint64_t have =
            static_cast<uint64_t>(std::filesystem::file_size(output + ".part", ec));
        if (!ec && have < length) {
            offset = have;
        }
    }

    std::ofstream out(output + ".part", std::ios::binary | (offset != 0 ? std::ios::app
                                                                        : std::ios::trunc));
    if (!out.is_open()) {
        throw CommandError{7, "cannot open output file: " + output};
    }
    ProgressPrinter progress(length, !g.json);
    uint64_t received = 0;
    const uint64_t total = client.download(ns, name, version, offset, length - offset,
                                           [&](const void *data, size_t len) {
                                               out.write(static_cast<const char *>(data),
                                                         static_cast<std::streamsize>(len));
                                               if (!out.good()) {
                                                   throw CommandError{7, "write failed: " + output};
                                               }
                                               received += len;
                                               progress.update(offset + received);
                                           });
    out.close();
    /* FR-DL-02：完整校验后原子替换目标文件（先重命名再替换）。 */
    std::error_code ec;
    std::filesystem::rename(output + ".part", output, ec);
    if (ec) {
        std::filesystem::remove(output, ec);
        std::filesystem::rename(output + ".part", output, ec);
    }
    if (g.json) {
        std::cout << "{\"ok\":true,\"file\":\"" << output << "\",\"bytes\":" << total << "}\n";
    } else {
        std::cout << "downloaded " << ref << " -> " << output << " (" << total << " bytes)\n";
    }
}

void cmd_artifact(const std::vector<std::string> &args)
{
    if (args.empty()) {
        usage_error("artifact requires list|show|delete");
    }
    fr::Client client = fr::Client(client_options());
    client.connect();
    const std::string sub = args[0];
    if (sub == "list") {
        std::optional<std::string> ns;
        std::optional<std::string> name;
        uint32_t limit = 50;
        for (size_t i = 1; i + 1 < args.size(); i += 2) {
            if (args[i] == "--namespace") {
                ns = args[i + 1];
            } else if (args[i] == "--name") {
                name = args[i + 1];
            } else if (args[i] == "--limit") {
                limit = static_cast<uint32_t>(std::stoul(args[i + 1]));
            }
        }
        const std::vector<fr::msg::ArtifactSummary> items =
            client.list_artifacts(ns, name, limit, 0);
        if (g.json) {
            std::cout << "{\"count\":" << items.size() << "}\n";
            return;
        }
        for (const fr::msg::ArtifactSummary &item : items) {
            std::cout << item.ns << "/" << item.name << ":" << item.version << "  " << item.size
                      << "B  " << item.creator << "  " << item.digest.substr(0, 16) << "…\n";
        }
        std::cout << items.size() << " artifact(s)\n";
        return;
    }
    if (sub == "show") {
        if (args.size() < 2) {
            usage_error("artifact show requires <ns>/<name>:<version>");
        }
        const std::string &ref = args[1];
        const size_t slash = ref.find('/');
        const size_t colon = ref.rfind(':');
        const fr::msg::ShowArtifactOk info = client.show_artifact(
            ref.substr(0, slash), ref.substr(slash + 1, colon - slash - 1), ref.substr(colon + 1));
        std::cout << "ns/name:version : " << info.info.ns << "/" << info.info.name << ":"
                  << info.info.version << "\n"
                  << "size            : " << info.info.size << "\n"
                  << "digest          : " << info.info.digest << "\n"
                  << "chunk_size      : " << info.info.chunk_size << "\n"
                  << "creator         : " << info.info.creator << "\n"
                  << "chunks          : " << info.info.chunks.size() << "\n";
        return;
    }
    if (sub == "delete") {
        if (args.size() < 2) {
            usage_error("artifact delete requires <ns>/<name>:<version>");
        }
        if (!g.confirm) {
            throw CommandError{2, "refusing to delete without --confirm (CLI-02)"};
        }
        const std::string &ref = args[1];
        const size_t slash = ref.find('/');
        const size_t colon = ref.rfind(':');
        const int64_t marked = client.delete_artifact(
            ref.substr(0, slash), ref.substr(slash + 1, colon - slash - 1), ref.substr(colon + 1));
        std::cout << "deleted; " << marked << " chunk(s) marked for cleanup\n";
        return;
    }
    usage_error("unknown artifact subcommand: " + sub);
}

void cmd_session(const std::vector<std::string> &args)
{
    fr::Client client = fr::Client(client_options());
    client.connect();
    if (args.empty() || args[0] == "list") {
        const std::vector<fr::msg::SessionItem> items = client.session_list();
        for (const fr::msg::SessionItem &item : items) {
            std::cout << item.session_id << "  " << item.owner << "  " << item.ns << "/"
                      << item.name << ":" << item.version << "  state=" << static_cast<int>(item.state)
                      << "  " << item.expected_size << "B\n";
        }
        std::cout << items.size() << " session(s)\n";
        return;
    }
    if (args[0] == "abort") {
        if (args.size() < 2) {
            usage_error("session abort requires <session-id>");
        }
        if (!g.confirm) {
            throw CommandError{2, "refusing to abort without --confirm (CLI-02)"};
        }
        client.session_abort(args[1]);
        std::cout << "session aborted\n";
        return;
    }
    usage_error("unknown session subcommand: " + args[0]);
}

void cmd_gc(const std::vector<std::string> &args)
{
    bool dry_run = false;
    for (const std::string &arg : args) {
        if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--confirm") {
            g.confirm = true;
        }
    }
    if (!dry_run && !g.confirm) {
        throw CommandError{2, "refusing to run real GC without --confirm (CLI-02)"};
    }
    fr::Client client = fr::Client(client_options());
    client.connect();
    const fr::msg::RunGcOk report = client.gc(dry_run);
    std::cout << (dry_run ? "dry-run: " : "collected: ") << report.deleted_count
              << " chunk(s), " << report.deleted_bytes << " bytes (scanned " << report.scanned
              << ")\n";
}

void cmd_user(const std::vector<std::string> &args)
{
    if (args.empty()) {
        usage_error("user requires add|disable|list");
    }
    fr::Client client = fr::Client(client_options());
    client.connect();
    const std::string sub = args[0];
    if (sub == "add") {
        if (args.size() < 3) {
            usage_error("user add requires <name> <Reader|Publisher|Admin>");
        }
        fr::Role role = fr::Role::Reader;
        if (!fr::role_from(args[2], role)) {
            usage_error("role must be Reader|Publisher|Admin");
        }
        client.user_add(args[1], role);
        std::cout << "user added: " << args[1] << " (" << args[2] << ")\n";
        return;
    }
    if (sub == "disable") {
        if (args.size() < 2) {
            usage_error("user disable requires <name>");
        }
        client.user_disable(args[1]);
        std::cout << "user disabled: " << args[1] << "\n";
        return;
    }
    if (sub == "list") {
        const std::vector<fr::msg::UserItem> items = client.user_list();
        for (const fr::msg::UserItem &item : items) {
            std::cout << item.username << "  " << fr::role_name(static_cast<fr::Role>(item.role))
                      << "  " << (item.enabled ? "enabled" : "disabled") << "\n";
        }
        std::cout << items.size() << " user(s)\n";
        return;
    }
    usage_error("unknown user subcommand: " + sub);
}

void cmd_token(const std::vector<std::string> &args)
{
    if (args.empty()) {
        usage_error("token requires create|revoke|list");
    }
    fr::Client client = fr::Client(client_options());
    client.connect();
    const std::string sub = args[0];
    if (sub == "create") {
        if (args.size() < 2) {
            usage_error("token create requires <username> [ttl-hours]");
        }
        uint32_t ttl = 0;
        if (args.size() >= 3) {
            ttl = static_cast<uint32_t>(std::stoul(args[2]));
        }
        const std::string token = client.token_create(args[1], ttl);
        std::cout << "token (shown once, store it safely):\n" << token << "\n";
        return;
    }
    if (sub == "revoke") {
        if (args.size() < 2) {
            usage_error("token revoke requires <token-id>");
        }
        client.token_revoke(std::stoull(args[1]));
        std::cout << "token revoked\n";
        return;
    }
    if (sub == "list") {
        if (args.size() < 2) {
            usage_error("token list requires <username>");
        }
        const std::vector<fr::msg::TokenItem> items = client.token_list(args[1]);
        for (const fr::msg::TokenItem &item : items) {
            std::cout << "id=" << item.id << "  " << item.username << "  "
                      << (item.revoked ? "revoked" : "active") << "\n";
        }
        std::cout << items.size() << " token(s)\n";
        return;
    }
    usage_error("unknown token subcommand: " + sub);
}

void cmd_status()
{
    fr::Client client = fr::Client(client_options());
    client.connect();
    const fr::msg::StatusOk ok = client.status();
    if (g.json) {
        std::cout << "{\"version\":\"" << ok.version << "\",\"uptime\":" << ok.uptime_seconds
                  << ",\"connections\":" << ok.connections << ",\"active_sessions\":"
                  << ok.active_sessions << ",\"artifacts\":" << ok.artifacts
                  << ",\"used_bytes\":" << ok.used_bytes << ",\"capacity_bytes\":"
                  << ok.capacity_bytes << "}\n";
        return;
    }
    std::cout << "version         : " << ok.version << "\n"
              << "uptime (s)      : " << ok.uptime_seconds << "\n"
              << "connections     : " << ok.connections << "\n"
              << "active sessions : " << ok.active_sessions << "\n"
              << "artifacts       : " << ok.artifacts << "\n"
              << "storage         : " << ok.used_bytes << " / " << ok.capacity_bytes
              << " bytes (high watermark " << static_cast<int>(ok.high_watermark_percent)
              << "%)\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> positional;
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        /* 全局参数（CLI-01）。 */
        std::vector<std::string> command_args;
        for (size_t i = 0; i < args.size(); i++) {
            const std::string &arg = args[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= args.size()) {
                    usage_error("missing value for " + arg);
                }
                return args[++i];
            };
            if (arg == "--server") {
                const std::string server = value();
                const size_t colon = server.rfind(':');
                if (colon == std::string::npos) {
                    usage_error("--server must be host:port");
                }
                g.host = server.substr(0, colon);
                g.port = static_cast<uint16_t>(std::stoul(server.substr(colon + 1)));
            } else if (arg == "--config") {
                (void)value(); // 预留：客户端配置文件
            } else if (arg == "--timeout") {
                g.timeout = std::stoi(value());
            } else if (arg == "--token") {
                g.token = value();
            } else if (arg == "--json") {
                g.json = true;
            } else if (arg == "--verbose") {
                g.verbose = true;
                g.json = false;
            } else if (arg == "--tls") {
                g.use_tls = true;
                g.tls_explicit = true;
            } else if (arg == "--no-tls") {
                g.use_tls = false;
                g.tls_explicit = true;
            } else if (arg == "--confirm") {
                g.confirm = true;
            } else if (arg == "--help" || arg == "-h") {
                std::cout << "usage: frctl [--server host:port] [--token t] [--json|--verbose] "
                             "<command>\n"
                             "commands: upload | download | artifact list|show|delete | "
                             "session list|abort | gc | status | user add|disable|list | "
                             "token create|revoke|list\n";
                return 0;
            } else {
                command_args.push_back(arg);
            }
        }
        positional = command_args;
        if (positional.empty()) {
            usage_error("no command given");
        }

        const std::string &cmd = positional[0];
        std::vector<std::string> rest(positional.begin() + 1, positional.end());
        if (cmd == "upload") {
            if (rest.size() < 1) {
                usage_error("upload requires <file>");
            }
            std::string ns = "default";
            std::string name;
            std::string version;
            for (size_t i = 1; i + 1 < rest.size(); i += 2) {
                if (rest[i] == "--namespace") {
                    ns = rest[i + 1];
                } else if (rest[i] == "--name") {
                    name = rest[i + 1];
                } else if (rest[i] == "--version") {
                    version = rest[i + 1];
                }
            }
            if (name.empty() || version.empty()) {
                usage_error("upload requires --name and --version");
            }
            cmd_upload(rest[0], ns, name, version);
        } else if (cmd == "download") {
            if (rest.size() < 1) {
                usage_error("download requires <ns>/<name>:<version>");
            }
            std::string output;
            bool resume = false;
            for (size_t i = 1; i < rest.size(); i++) {
                if (rest[i] == "--output" && i + 1 < rest.size()) {
                    output = rest[++i];
                } else if (rest[i] == "--resume") {
                    resume = true;
                }
            }
            if (output.empty()) {
                usage_error("download requires --output <path>");
            }
            cmd_download(rest[0], output, resume);
        } else if (cmd == "artifact") {
            cmd_artifact(rest);
        } else if (cmd == "session") {
            cmd_session(rest);
        } else if (cmd == "gc") {
            cmd_gc(rest);
        } else if (cmd == "status") {
            cmd_status();
        } else if (cmd == "user") {
            cmd_user(rest);
        } else if (cmd == "token") {
            cmd_token(rest);
        } else {
            usage_error("unknown command: " + cmd);
        }
        return static_cast<int>(fr::ExitCode::Ok);
    } catch (const CommandError &err) {
        std::cerr << "error: " << err.message << "\n";
        return err.code;
    } catch (const fr::Error &err) {
        std::cerr << "error: [" << fr_status_name(err.code()) << "] " << err.what() << "\n";
        return static_cast<int>(fr::exit_code_for(err));
    } catch (const std::exception &err) {
        std::cerr << "error: " << err.what() << "\n";
        return static_cast<int>(fr::ExitCode::Network);
    }
}
