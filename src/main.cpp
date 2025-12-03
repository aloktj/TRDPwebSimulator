#include "plugins/TelegramHub.h"
#include "trdp_engine.h"
#include "telegram_model.h"

#include <drogon/drogon.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct CliOptions {
    std::uint16_t port{8080};
    std::string xmlPath;
    std::string trdpRxIface;
    std::string trdpTxIface;
    std::uint16_t threads{0};
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

std::optional<std::string> readEnv(const char *name) {
    if (const char *value = std::getenv(name)) {
        return std::string(value);
    }
    return std::nullopt;
}

void printUsage(const char *exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "Options:\n"
              << "  --port <port>           TCP port for Drogon listener (env: PORT or DROGON_PORT)\n"
              << "  --xml <path>           Path to TRDP XML config (env: TRDP_XML_PATH)\n"
              << "  --trdp-rx-iface <if>   Interface name for RX (env: TRDP_RX_IFACE)\n"
              << "  --trdp-tx-iface <if>   Interface name for TX (env: TRDP_TX_IFACE)\n"
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
        } else if (arg == "--threads" && i + 1 < argc) {
            if (auto parsed = parsePort(argv[i + 1])) {
                opts.threads = *parsed;
            }
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
}

} // namespace

int main(int argc, char **argv) {
    using namespace trdp;

    const auto opts = parseArgs(argc, argv);
    if (opts.showHelp) {
        printUsage(argv[0]);
        return 0;
    }

    if (!opts.xmlPath.empty()) {
        setDefaultXmlConfig(opts.xmlPath);
    }
    applyTrdpEnv(opts);

    auto &app = drogon::app();
    app.registerPlugin<TelegramHub>();
    app.addListener("0.0.0.0", opts.port);
    if (opts.threads > 0) {
        app.setThreadNum(opts.threads);
    }

    if (!TrdpEngine::instance().start()) {
        std::cerr << "Failed to start TRDP engine" << std::endl;
        return 1;
    }

    app.run();
    return 0;
}

