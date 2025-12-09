#include "plugins/TelegramHub.h"
#include "trdp_engine.h"
#include "telegram_model.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct CliOptions {
    std::uint16_t port{8080};
    std::string xmlPath;
    std::string trdpRxIface;
    std::string trdpTxIface;
    std::string trdpHostsFile;
    std::string dnrMode{"common"};
    bool enableUriCache{true};
    std::uint32_t cacheTtlMs{30000};
    std::uint32_t cacheEntries{128};
    std::string staticRoot;
    std::uint16_t threads{0};
    bool enableDnr{false};
    bool enableEcsp{false};
    std::uint32_t ecspPollMs{1000};
    std::uint32_t ecspConfirmTimeoutMs{5000};
    bool showHelp{false};
};

std::optional<std::uint16_t> parsePort(const std::string &value) {
    try {
        const auto num = std::stoul(value);
        if (num > 0 && num <= 65535) {
            return static_cast<std::uint16_t>(num);
        }
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

std::optional<std::uint32_t> parseUint(const std::string &value) {
    try {
        return static_cast<std::uint32_t>(std::stoul(value));
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

bool parseBool(const std::string &value) {
    std::string lowered(value.size(), '\0');
    std::transform(value.begin(), value.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

std::optional<std::string> readEnv(const char *name) {
    if (const char *value = std::getenv(name)) {
        return std::string(value);
    }
    return std::nullopt;
}

bool portAvailable(std::uint16_t port) {
    const auto sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return true;
    }

    int reuse = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    const bool available = ::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
    ::close(sock);
    return available;
}

void printUsage(const char *exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "Options:\n"
              << "  --port <port>           TCP port for Drogon listener (env: PORT or DROGON_PORT)\n"
              << "  --xml <path>           Path to TRDP XML config (env: TRDP_XML_PATH)\n"
              << "  --trdp-rx-iface <if>   Interface name for RX (env: TRDP_RX_IFACE)\n"
              << "  --trdp-tx-iface <if>   Interface name for TX (env: TRDP_TX_IFACE)\n"
              << "  --trdp-hosts-file <f>  Hosts file for DNR lookups (env: TRDP_HOSTS_FILE)\n"
              << "  --dnr-mode <mode>      DNR thread mode: common|dedicated (env: TRDP_DNR_MODE)\n"
              << "  --cache-ttl-ms <ms>    Cache TTL for URI/label lookups (env: TRDP_CACHE_TTL_MS)\n"
              << "  --cache-entries <n>    Maximum cached URI/label entries (env: TRDP_CACHE_ENTRIES)\n"
              << "  --disable-cache        Disable DNR lookup caching (env: TRDP_DISABLE_CACHE)\n"
              << "  --enable-dnr           Enable TAU DNR initialisation (env: TRDP_ENABLE_DNR)\n"
              << "  --enable-ecsp          Enable TAU ECSP control (env: TRDP_ENABLE_ECSP)\n"
              << "  --ecsp-poll-ms <ms>    Poll interval for ECSP status (env: TRDP_ECSP_POLL_MS)\n"
              << "  --ecsp-confirm-ms <ms> Confirm timeout for ECSP control (env: TRDP_ECSP_CONFIRM_MS)\n"
              << "  --static-root <path>   Directory for UI assets (env: TRDP_STATIC_ROOT)\n"
              << "  --threads <n>          Worker threads for Drogon (default: hardware concurrency)\n"
              << "  --help                 Show this help message\n";
}

CliOptions parseArgs(int argc, char **argv) {
    CliOptions opts;

    if (auto envPort = readEnv("PORT"); envPort && parsePort(*envPort)) {
        opts.port = *parsePort(*envPort);
    } else if (auto envPortD = readEnv("DROGON_PORT"); envPortD && parsePort(*envPortD)) {
        opts.port = *parsePort(*envPortD);
    }

    if (auto envXml = readEnv("TRDP_XML_PATH")) {
        opts.xmlPath = *envXml;
    }
    if (auto envRx = readEnv("TRDP_RX_IFACE")) {
        opts.trdpRxIface = *envRx;
    }
    if (auto envTx = readEnv("TRDP_TX_IFACE")) {
        opts.trdpTxIface = *envTx;
    }
    if (auto envHosts = readEnv("TRDP_HOSTS_FILE")) {
        opts.trdpHostsFile = *envHosts;
    }
    if (auto envDnrMode = readEnv("TRDP_DNR_MODE")) {
        opts.dnrMode = *envDnrMode;
    }
    if (auto envCacheTtl = readEnv("TRDP_CACHE_TTL_MS")) {
        if (auto parsed = parseUint(*envCacheTtl)) {
            opts.cacheTtlMs = *parsed;
        }
    }
    if (auto envCacheEntries = readEnv("TRDP_CACHE_ENTRIES")) {
        if (auto parsed = parseUint(*envCacheEntries)) {
            opts.cacheEntries = *parsed;
        }
    }
    if (auto envDisableCache = readEnv("TRDP_DISABLE_CACHE")) {
        opts.enableUriCache = !parseBool(*envDisableCache);
    }
    if (auto envDnr = readEnv("TRDP_ENABLE_DNR")) {
        opts.enableDnr = parseBool(*envDnr);
    }
    if (auto envEcsp = readEnv("TRDP_ENABLE_ECSP")) {
        opts.enableEcsp = parseBool(*envEcsp);
    }
    if (auto envEcspPoll = readEnv("TRDP_ECSP_POLL_MS")) {
        if (auto parsed = parseUint(*envEcspPoll)) {
            opts.ecspPollMs = *parsed;
        }
    }
    if (auto envEcspConfirm = readEnv("TRDP_ECSP_CONFIRM_MS")) {
        if (auto parsed = parseUint(*envEcspConfirm)) {
            opts.ecspConfirmTimeoutMs = *parsed;
        }
    }
    if (auto envStatic = readEnv("TRDP_STATIC_ROOT")) {
        opts.staticRoot = *envStatic;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            opts.showHelp = true;
            break;
        } else if (arg == "--port" && i + 1 < argc) {
            if (auto parsed = parsePort(argv[i + 1])) {
                opts.port = *parsed;
            }
            ++i;
        } else if (arg == "--xml" && i + 1 < argc) {
            opts.xmlPath = argv[i + 1];
            ++i;
        } else if (arg == "--trdp-rx-iface" && i + 1 < argc) {
            opts.trdpRxIface = argv[i + 1];
            ++i;
        } else if (arg == "--trdp-tx-iface" && i + 1 < argc) {
            opts.trdpTxIface = argv[i + 1];
            ++i;
        } else if (arg == "--trdp-hosts-file" && i + 1 < argc) {
            opts.trdpHostsFile = argv[i + 1];
            ++i;
        } else if (arg == "--dnr-mode" && i + 1 < argc) {
            opts.dnrMode = argv[i + 1];
            ++i;
        } else if (arg == "--cache-ttl-ms" && i + 1 < argc) {
            if (auto parsed = parseUint(argv[i + 1])) {
                opts.cacheTtlMs = *parsed;
            }
            ++i;
        } else if (arg == "--cache-entries" && i + 1 < argc) {
            if (auto parsed = parseUint(argv[i + 1])) {
                opts.cacheEntries = *parsed;
            }
            ++i;
        } else if (arg == "--disable-cache") {
            opts.enableUriCache = false;
        } else if (arg == "--enable-dnr") {
            opts.enableDnr = true;
        } else if (arg == "--enable-ecsp") {
            opts.enableEcsp = true;
        } else if (arg == "--ecsp-poll-ms" && i + 1 < argc) {
            if (auto parsed = parseUint(argv[i + 1])) {
                opts.ecspPollMs = *parsed;
            }
            ++i;
        } else if (arg == "--ecsp-confirm-ms" && i + 1 < argc) {
            if (auto parsed = parseUint(argv[i + 1])) {
                opts.ecspConfirmTimeoutMs = *parsed;
            }
            ++i;
        } else if (arg == "--threads" && i + 1 < argc) {
            if (auto parsed = parsePort(argv[i + 1])) {
                opts.threads = *parsed;
            }
            ++i;
        } else if (arg == "--static-root" && i + 1 < argc) {
            opts.staticRoot = argv[i + 1];
            ++i;
        }
    }

    return opts;
}

void applyTrdpEnv(const CliOptions &opts) {
    if (!opts.trdpRxIface.empty()) {
        setenv("TRDP_RX_IFACE", opts.trdpRxIface.c_str(), 1);
    }
    if (!opts.trdpTxIface.empty()) {
        setenv("TRDP_TX_IFACE", opts.trdpTxIface.c_str(), 1);
    }
    if (!opts.trdpHostsFile.empty()) {
        setenv("TRDP_HOSTS_FILE", opts.trdpHostsFile.c_str(), 1);
    }
    if (!opts.dnrMode.empty()) {
        setenv("TRDP_DNR_MODE", opts.dnrMode.c_str(), 1);
    }
    if (!opts.enableUriCache) {
        setenv("TRDP_DISABLE_CACHE", "1", 1);
    }
    if (opts.cacheTtlMs > 0U) {
        const auto ttl = std::to_string(opts.cacheTtlMs);
        setenv("TRDP_CACHE_TTL_MS", ttl.c_str(), 1);
    }
    if (opts.cacheEntries > 0U) {
        const auto entries = std::to_string(opts.cacheEntries);
        setenv("TRDP_CACHE_ENTRIES", entries.c_str(), 1);
    }
    if (opts.enableDnr) {
        setenv("TRDP_ENABLE_DNR", "1", 1);
    }
    if (opts.enableEcsp) {
        setenv("TRDP_ENABLE_ECSP", "1", 1);
    }
    if (opts.ecspPollMs > 0U) {
        const auto poll = std::to_string(opts.ecspPollMs);
        setenv("TRDP_ECSP_POLL_MS", poll.c_str(), 1);
    }
    if (opts.ecspConfirmTimeoutMs > 0U) {
        const auto confirm = std::to_string(opts.ecspConfirmTimeoutMs);
        setenv("TRDP_ECSP_CONFIRM_MS", confirm.c_str(), 1);
    }
}

std::filesystem::path resolveStaticRoot(const CliOptions &opts, const char *argv0) {
    std::vector<std::filesystem::path> candidates;

    if (!opts.staticRoot.empty()) {
        candidates.emplace_back(opts.staticRoot);
    }

    // Current working directory
    candidates.emplace_back(std::filesystem::current_path() / "static");

    // Directory next to the executable (e.g., when running from an install prefix)
    const auto exeDir = std::filesystem::weakly_canonical(std::filesystem::path(argv0)).parent_path();
    if (!exeDir.empty()) {
        candidates.emplace_back(exeDir / "static");
        candidates.emplace_back(exeDir / ".." / "static");
    }

    for (const auto &path : candidates) {
        std::error_code existsEc;
        if (std::filesystem::exists(path, existsEc) && std::filesystem::is_directory(path, existsEc)) {
            std::error_code canonicalEc;
            const auto canonical = std::filesystem::canonical(path, canonicalEc);
            if (!canonicalEc) {
                return canonical;
            }
            return path;
        }
    }

    return candidates.front();
}

} // namespace

int main(int argc, char **argv) {
    using namespace trdp;

    const auto opts = parseArgs(argc, argv);
    if (opts.showHelp) {
        printUsage(argv[0]);
        return 0;
    }

    if (!portAvailable(opts.port)) {
        std::cerr << "Port " << opts.port
                  << " is already in use. Choose a different port with --port or by setting PORT/DROGON_PORT."
                  << std::endl;
        return 1;
    }

    if (!opts.xmlPath.empty()) {
        setDefaultXmlConfig(opts.xmlPath);
    }
    applyTrdpEnv(opts);

    TelegramHub telegramHub;
    telegramHub.initAndStart(Json::Value());

    auto &app = drogon::app();
    app.addListener("0.0.0.0", opts.port);
    const auto staticRoot = resolveStaticRoot(opts, argv[0]);
    std::error_code staticExistsEc;
    if (!std::filesystem::exists(staticRoot, staticExistsEc) ||
        !std::filesystem::is_directory(staticRoot, staticExistsEc)) {
        std::cerr << "Warning: static assets not found at " << staticRoot
                  << ". HTTP requests for the UI will return 404." << std::endl;
    }
    std::cout << "Using static assets from: " << staticRoot << std::endl;
    app.setDocumentRoot(staticRoot.string());
    if (opts.threads > 0) {
        app.setThreadNum(opts.threads);
    }

    TrdpEngine::TrdpConfig trdpConfig{};
    trdpConfig.rxInterface = opts.trdpRxIface;
    trdpConfig.txInterface = opts.trdpTxIface;
    trdpConfig.hostsFile = opts.trdpHostsFile;
    trdpConfig.enableDnr = opts.enableDnr;
    trdpConfig.dnrMode = opts.dnrMode == "dedicated" ? TrdpEngine::DnrMode::DedicatedThread : TrdpEngine::DnrMode::CommonThread;
    trdpConfig.cacheConfig.enableUriCache = opts.enableUriCache;
    trdpConfig.cacheConfig.uriCacheTtl = std::chrono::milliseconds(opts.cacheTtlMs);
    trdpConfig.cacheConfig.uriCacheEntries = opts.cacheEntries;
    trdpConfig.ecspConfig.enable = opts.enableEcsp;
    trdpConfig.ecspConfig.pollInterval = std::chrono::milliseconds(opts.ecspPollMs);
    trdpConfig.ecspConfig.confirmTimeout = std::chrono::milliseconds(opts.ecspConfirmTimeoutMs);

    if (!TrdpEngine::instance().start(trdpConfig)) {
        std::cerr << "Failed to start TRDP engine" << std::endl;
        return 1;
    }

    app.run();
    telegramHub.shutdown();
    return 0;
}

