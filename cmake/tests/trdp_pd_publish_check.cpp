#include <trdp/api/trdp_if_light.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ifaddrs.h>
#include <iostream>
#include <arpa/inet.h>
#include <string>
#include <vector>

namespace {
std::string formatIp(std::uint32_t ip)
{
    in_addr addr{};
    addr.s_addr = htonl(ip);
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &addr, buffer, sizeof(buffer)) == nullptr)
    {
        return "<invalid IPv4>";
    }
    return buffer;
}

std::uint32_t parseIp(const std::string &text)
{
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1)
    {
        return 0U;
    }
    return ntohl(addr.s_addr);
}

bool ipAssignedToLocalInterface(std::uint32_t ip)
{
    if (ip == 0U)
    {
        return true;
    }

    ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr)
    {
        return false;
    }

    bool found = false;
    for (auto *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        const auto *addr = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
        if (static_cast<std::uint32_t>(ntohl(addr->sin_addr.s_addr)) == ip)
        {
            found = true;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

std::string formatError(TRDP_ERR_T err)
{
    switch (err)
    {
    case TRDP_NO_ERR:
        return "TRDP_NO_ERR";
    case TRDP_PARAM_ERR:
        return "TRDP_PARAM_ERR";
    case TRDP_INIT_ERR:
        return "TRDP_INIT_ERR";
    case TRDP_NOINIT_ERR:
        return "TRDP_NOINIT_ERR";
    case TRDP_SOCK_ERR:
        return "TRDP_SOCK_ERR";
    case TRDP_TIMEOUT_ERR:
        return "TRDP_TIMEOUT_ERR";
    case TRDP_MEM_ERR:
        return "TRDP_MEM_ERR";
    default:
        return "TRDP error code " + std::to_string(static_cast<int>(err));
    }
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <ComId> <source IPv4> <destination IPv4> [source port=17224] [interval ms=1000]" << std::endl;
        return 1;
    }

    const auto comId = static_cast<UINT32>(std::stoul(argv[1]));
    const auto sourceIp = parseIp(argv[2]);
    const auto destIp = parseIp(argv[3]);
    const std::uint16_t sourcePort = static_cast<std::uint16_t>(argc > 4 ? std::stoul(argv[4]) : 17224U);
    const UINT32 intervalUs = static_cast<UINT32>((argc > 5 ? std::stoul(argv[5]) : 1000U) * 1000U);

    if (sourceIp == 0U || destIp == 0U)
    {
        std::cerr << "Invalid IPv4 argument; ensure dotted-quad notation is used." << std::endl;
        return 1;
    }

    if (!ipAssignedToLocalInterface(sourceIp))
    {
        std::cerr << "Source IP " << formatIp(sourceIp)
                  << " is not configured on this host. Choose a local interface address or set TRDP_TX_IFACE."
                  << std::endl;
        return 1;
    }

    /*
     * Use the same heap size as the simulator stack initialisation to avoid
     * TRDP_MEM_ERR (-8) from tlc_openSession when the heap is too small for
     * default PD configuration structures.
     */
    constexpr std::size_t kHeapSize = 64 * 1024;
    std::array<UINT8, kHeapSize> heap{};
    TRDP_MEM_CONFIG_T memConfig{};
    memConfig.p = heap.data();
    memConfig.size = static_cast<UINT32>(heap.size());

    const TRDP_ERR_T initErr = tlc_init(nullptr, nullptr, &memConfig);
    if (initErr != TRDP_NO_ERR)
    {
        std::cerr << "tlc_init failed: " << formatError(initErr) << std::endl;
        return 1;
    }

    TRDP_PD_CONFIG_T pdConfig{};
    pdConfig.port = sourcePort;
    pdConfig.sendParam = TRDP_PD_DEFAULT_SEND_PARAM;
    pdConfig.sendParam.ttl = 64U;
    pdConfig.flags = TRDP_FLAGS_NONE;
    pdConfig.timeout = TRDP_PD_DEFAULT_TIMEOUT;
    pdConfig.toBehavior = TRDP_TO_SET_TO_ZERO;

    TRDP_APP_SESSION_T session{};
    const TRDP_ERR_T sessionErr = tlc_openSession(&session, sourceIp, 0U, nullptr, &pdConfig, nullptr, nullptr);
    if (sessionErr != TRDP_NO_ERR)
    {
        std::cerr << "tlc_openSession failed: " << formatError(sessionErr) << std::endl;
        tlc_terminate();
        return 1;
    }

    TRDP_SEND_PARAM_T sendParam = TRDP_PD_DEFAULT_SEND_PARAM;
    sendParam.ttl = 64U;

    constexpr UINT32 kRedundancyId = 0U;
    const TRDP_FLAGS_T pdFlags = TRDP_FLAGS_NONE;
    std::vector<std::uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};

    TRDP_PUB_T pub{};
    const TRDP_ERR_T publishErr = tlp_publish(session, &pub, nullptr, nullptr, 0U, comId, 0U, 0U, sourceIp, destIp,
                                              intervalUs, kRedundancyId, pdFlags, &sendParam, payload.data(),
                                              static_cast<UINT32>(payload.size()));

    if (publishErr != TRDP_NO_ERR)
    {
        std::cerr << "tlp_publish failed: " << formatError(publishErr) << std::endl;
    }
    else
    {
        std::cout << "PD publish succeeded for ComId " << comId << " to " << argv[3] << ':' << sourcePort
                  << " with payload size " << payload.size() << std::endl;
    }

    tlp_unpublish(session, pub);
    tlc_closeSession(session);
    tlc_terminate();
    return publishErr == TRDP_NO_ERR ? 0 : 2;
}
