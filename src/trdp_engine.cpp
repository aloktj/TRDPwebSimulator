#include "trdp_engine.h"

#include "plugins/TelegramHub.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <type_traits>
#include <utility>

#ifdef TRDP_STACK_PRESENT
#include <trdp/api/tau_dnr.h>
#if __has_include(<trdp/api/tau_ecsp.h>)
#include <trdp/api/tau_ecsp.h>
#define TRDP_HAS_TAU_ECSP 1
#endif
#include <trdp/api/tau_ctrl.h>
#include <trdp/api/trdp_if_light.h>
#include <trdp/api/iec61375-2-3.h>
#endif

#ifndef TRDP_HAS_TAU_DNR
#define TRDP_HAS_TAU_DNR 0
#endif

namespace trdp {

#ifdef TRDP_STACK_PRESENT
void pdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_PD_INFO_T *pInfo, UINT8 *pData,
                       UINT32 dataSize);
void mdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_MD_INFO_T *pInfo, UINT8 *pData,
                       UINT32 dataSize);
#endif

namespace {
#if defined(TRDP_STACK_PRESENT) && TRDP_HAS_TAU_DNR
constexpr bool kDnrCompiledIn = true;
#else
constexpr bool kDnrCompiledIn = false;
#endif

void logDnrUnavailable(const std::string &reason)
{
    static bool warned = false;
    static std::string lastReason;
    if (!warned || reason != lastReason) {
        std::cerr << "[TRDP] DNR unavailable: " << reason << std::endl;
        warned = true;
        lastReason = reason;
    }
}

template <typename T> T readLe(const std::uint8_t *data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

#ifdef TRDP_STACK_PRESENT
std::chrono::milliseconds toDuration(const TRDP_TIME_T &time) {
    return std::chrono::milliseconds(time.tv_usec / 1000 + time.tv_sec * 1000);
}

#endif

template <typename T> void writeLe(std::uint8_t *dest, T value) { std::memcpy(dest, &value, sizeof(T)); }

std::size_t fieldWidth(const FieldDef &field) {
    switch (field.type) {
    case FieldType::BOOL:
    case FieldType::INT8:
    case FieldType::UINT8:
        return 1U * field.arrayLength;
    case FieldType::INT16:
    case FieldType::UINT16:
        return 2U * field.arrayLength;
    case FieldType::INT32:
    case FieldType::UINT32:
    case FieldType::FLOAT:
        return 4U * field.arrayLength;
    case FieldType::DOUBLE:
        return 8U * field.arrayLength;
    case FieldType::STRING:
    case FieldType::BYTES:
        return field.size > 0 ? field.size : 0U;
    }
    return 0U;
}

FieldValue decodeSingleValue(const FieldDef &field, const std::uint8_t *ptr, std::size_t remaining) {
    if (remaining < fieldWidth(field)) {
        return {};
    }

    switch (field.type) {
    case FieldType::BOOL:
        return static_cast<bool>(*ptr != 0U);
    case FieldType::INT8:
        return static_cast<std::int8_t>(*ptr);
    case FieldType::UINT8:
        return *ptr;
    case FieldType::INT16:
        return readLe<std::int16_t>(ptr);
    case FieldType::UINT16:
        return readLe<std::uint16_t>(ptr);
    case FieldType::INT32:
        return readLe<std::int32_t>(ptr);
    case FieldType::UINT32:
        return readLe<std::uint32_t>(ptr);
    case FieldType::FLOAT:
        return readLe<float>(ptr);
    case FieldType::DOUBLE:
        return readLe<double>(ptr);
    case FieldType::STRING: {
        const auto len = field.size > 0 ? std::min<std::size_t>(field.size, remaining) : remaining;
        return std::string(reinterpret_cast<const char *>(ptr), len);
    }
    case FieldType::BYTES: {
        const auto len = field.size > 0 ? std::min<std::size_t>(field.size, remaining) : remaining;
        return std::vector<std::uint8_t>(ptr, ptr + len);
    }
    }
    return {};
}

void encodeSingleValue(const FieldDef &field, const FieldValue &value, std::uint8_t *dest, std::size_t destSize) {
    if (dest == nullptr || destSize < fieldWidth(field)) {
        return;
    }

    auto fillBytes = [&](const std::vector<std::uint8_t> &src) {
        const auto len = std::min(src.size(), destSize);
        std::memcpy(dest, src.data(), len);
        if (len < destSize) {
            std::memset(dest + len, 0, destSize - len);
        }
    };

    switch (field.type) {
    case FieldType::BOOL:
        writeLe<std::uint8_t>(dest, std::get<bool>(value) ? 1U : 0U);
        break;
    case FieldType::INT8:
        writeLe<std::int8_t>(dest, std::get<std::int8_t>(value));
        break;
    case FieldType::UINT8:
        writeLe<std::uint8_t>(dest, std::get<std::uint8_t>(value));
        break;
    case FieldType::INT16:
        writeLe<std::int16_t>(dest, std::get<std::int16_t>(value));
        break;
    case FieldType::UINT16:
        writeLe<std::uint16_t>(dest, std::get<std::uint16_t>(value));
        break;
    case FieldType::INT32:
        writeLe<std::int32_t>(dest, std::get<std::int32_t>(value));
        break;
    case FieldType::UINT32:
        writeLe<std::uint32_t>(dest, std::get<std::uint32_t>(value));
        break;
    case FieldType::FLOAT:
        writeLe<float>(dest, std::get<float>(value));
        break;
    case FieldType::DOUBLE:
        writeLe<double>(dest, std::get<double>(value));
        break;
    case FieldType::STRING: {
        const auto &str = std::get<std::string>(value);
        std::memset(dest, 0, destSize);
        std::memcpy(dest, str.data(), std::min(str.size(), destSize));
        break;
    }
    case FieldType::BYTES:
        fillBytes(std::get<std::vector<std::uint8_t>>(value));
        break;
    }
}

std::vector<std::uint8_t> encodeFields(const DatasetDef &dataset, const std::map<std::string, FieldValue> &fields) {
    const auto bufferSize = dataset.computeSize();
    std::vector<std::uint8_t> buffer(bufferSize, 0U);

    for (const auto &field : dataset.fields) {
        const auto it = fields.find(field.name);
        if (it == fields.end() || std::holds_alternative<std::monostate>(it->second)) {
            continue;
        }
        const auto width = fieldWidth(field);
        if (field.offset + width > buffer.size()) {
            continue;
        }
        encodeSingleValue(field, it->second, buffer.data() + field.offset, width);
    }

    return buffer;
}

#ifdef TRDP_STACK_PRESENT
std::string formatIp(std::uint32_t ip) {
    in_addr addr{};
    addr.s_addr = htonl(ip);
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &addr, buffer, sizeof(buffer)) == nullptr) {
        return "<invalid IPv4>";
    }
    return buffer;
}

bool ipAssignedToLocalInterface(std::uint32_t ip) {
    if (ip == 0U) {
        return true;
    }

    ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
        return false;
    }

    bool found = false;
    for (auto *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto *addr = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
        if (addr->sin_addr.s_addr == htonl(ip)) {
            found = true;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

std::optional<std::uint32_t> resolveInterfaceIp(const std::string &ifName) {
    if (ifName.empty()) {
        return std::nullopt;
    }

    ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0 || ifaddr == nullptr) {
        return std::nullopt;
    }

    std::optional<std::uint32_t> ip;
    for (auto *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == nullptr || ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifName == ifa->ifa_name) {
            const auto *addr = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
            ip = static_cast<std::uint32_t>(ntohl(addr->sin_addr.s_addr));
            break;
        }
    }

    freeifaddrs(ifaddr);
    return ip;
}

std::string describeTrdpError(TRDP_ERR_T err) {
    switch (err) {
    case TRDP_NO_ERR:
        return "TRDP_NO_ERR";
    case TRDP_PARAM_ERR:
        return "TRDP_PARAM_ERR (parameter missing or out of range)";
    case TRDP_INIT_ERR:
        return "TRDP_INIT_ERR (stack not initialised)";
    case TRDP_NOINIT_ERR:
        return "TRDP_NOINIT_ERR (invalid handle)";
    case TRDP_TIMEOUT_ERR:
        return "TRDP_TIMEOUT_ERR (operation timed out)";
    case TRDP_SOCK_ERR:
        return "TRDP_SOCK_ERR (socket error or unsupported option)";
    case TRDP_IO_ERR:
        return "TRDP_IO_ERR (socket I/O error)";
    case TRDP_STATE_ERR:
        return "TRDP_STATE_ERR (call in wrong state)";
    case TRDP_UNKNOWN_ERR:
        return "TRDP_UNKNOWN_ERR";
    default:
        return std::string{"TRDP error code "} + std::to_string(err);
    }
}
#endif

std::vector<std::uint8_t> encodeFieldsToBuffer(const TelegramRuntime &runtime,
                                               const std::map<std::string, FieldValue> &fields) {
    return encodeFields(runtime.dataset(), fields);
}

#ifdef TRDP_STACK_PRESENT
template <typename T, typename = void> struct has_qos_field : std::false_type {};
template <typename T> struct has_qos_field<T, std::void_t<decltype(std::declval<T>().qos)>> : std::true_type {};

template <typename T, typename = void> struct has_src_port_field : std::false_type {};
template <typename T>
struct has_src_port_field<T, std::void_t<decltype(std::declval<T>().srcPort)>> : std::true_type {};

template <typename T, typename = void> struct has_dest_port_field : std::false_type {};
template <typename T>
struct has_dest_port_field<T, std::void_t<decltype(std::declval<T>().destPort)>> : std::true_type {};

template <typename Param> void applyTelegramQos(const TelegramDef &telegram, Param &param) {
    if constexpr (has_qos_field<Param>::value) {
        param.qos = telegram.qos;
    }
}

template <typename Param> void applyTelegramPorts(const TelegramDef &telegram, Param &param) {
    if constexpr (has_src_port_field<Param>::value) {
        param.srcPort = telegram.srcPort;
    }
    if constexpr (has_dest_port_field<Param>::value) {
        param.destPort = telegram.destPort;
    }
}
#endif

void decodeFieldsIntoRuntime(const DatasetDef &dataset, TelegramRuntime &runtime, const std::vector<std::uint8_t> &payload) {
    runtime.overwriteBuffer(payload);
    for (const auto &field : dataset.fields) {
        const auto width = fieldWidth(field);
        if (field.offset + width > payload.size()) {
            continue;
        }
        const auto value = decodeSingleValue(field, payload.data() + field.offset, payload.size() - field.offset);
        runtime.setFieldValue(field.name, value);
    }
}

std::map<std::string, FieldValue> mergeRuntimeFields(const TelegramRuntime &runtime,
                                                     const std::map<std::string, FieldValue> &overrides) {
    auto result = runtime.snapshotFields();
    for (const auto &[key, value] : overrides) {
        result[key] = value;
    }
    return result;
}

#if defined(TRDP_STACK_PRESENT) && TRDP_HAS_TAU_DNR
static TRDP_DNR_OPTS_T mapDnrMode(TrdpEngine::DnrMode mode) {
    return (mode == TrdpEngine::DnrMode::DedicatedThread) ? TRDP_DNR_OWN_THREAD : TRDP_DNR_COMMON_THREAD;
}
#endif

} // namespace

#ifdef TRDP_STACK_PRESENT
constexpr std::uint16_t kDefaultTrdpPort = 17224;

std::uint16_t resolveDefaultPort(TelegramType type) {
    for (const auto &telegram : TelegramRegistry::instance().listTelegrams()) {
        if (telegram.type == type && telegram.destPort != 0U) {
            return telegram.destPort;
        }
    }
    return kDefaultTrdpPort;
}

TRDP_APP_SESSION_T TrdpEngine::defaultPdSession() const {
    if (pdSessions.empty()) {
        return nullptr;
    }
    return pdSessions.begin()->second;
}

TRDP_APP_SESSION_T TrdpEngine::defaultMdSession() const {
    if (mdSessions.empty()) {
        return nullptr;
    }
    return mdSessions.begin()->second;
}

TRDP_APP_SESSION_T TrdpEngine::pdSessionForPort(std::uint16_t port) const {
    if (auto it = pdSessions.find(port); it != pdSessions.end()) {
        return it->second;
    }
    return defaultPdSession();
}

TRDP_APP_SESSION_T TrdpEngine::mdSessionForPort(std::uint16_t port) const {
    if (auto it = mdSessions.find(port); it != mdSessions.end()) {
        return it->second;
    }
    return defaultMdSession();
}

std::uint16_t TrdpEngine::resolvePortForEndpoint(const TelegramDef &telegram) const {
    // Prefer the local/source port when transmitting and the destination port when receiving.
    if (telegram.direction == Direction::Tx && telegram.srcPort != 0U) {
        return telegram.srcPort;
    }
    if (telegram.destPort != 0U) {
        return telegram.destPort;
    }
    return telegram.srcPort;
}

std::string TrdpEngine::formatMdSessionKey(const MdSessionKey &key) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (i > 0 && i % 2 == 0) {
            oss << ':';
        }
        oss << std::setw(2) << static_cast<unsigned int>(key[i]);
    }
    return oss.str();
}

TrdpEngine::MdSessionKey TrdpEngine::mdSessionKeyFromId(const TRDP_UUID_T &sessionId) const {
    MdSessionKey key{};
    static_assert(sizeof(sessionId) == sizeof(MdSessionKey), "Unexpected TRDP_UUID_T size");
    std::memcpy(key.data(), &sessionId, key.size());
    return key;
}

void TrdpEngine::trackMdRequest(const MdSessionKey &sessionKey, const EndpointHandle &endpoint) {
    if (endpoint.def.expectedReplies == 0 && endpoint.def.confirmTimeout.count() == 0) {
        return;
    }

    MdRequestState state{};
    state.comId = endpoint.def.comId;
    state.expectedReplies = endpoint.def.expectedReplies;
    state.sentAt = std::chrono::steady_clock::now();
    state.confirmObserved = endpoint.def.confirmTimeout.count() == 0;

    if (endpoint.def.replyTimeout.count() > 0) {
        state.replyDeadline = state.sentAt + endpoint.def.replyTimeout;
    }
    if (endpoint.def.confirmTimeout.count() > 0) {
        state.confirmDeadline = state.sentAt + endpoint.def.confirmTimeout;
    }

    std::lock_guard lock(mdRequestMtx);
    mdRequestStates[sessionKey] = state;
}

void TrdpEngine::registerMdReply(const TRDP_MD_INFO_T *pInfo) {
    const auto sessionKey = mdSessionKeyFromId(pInfo->sessionId);
    std::lock_guard lock(mdRequestMtx);
    auto it = mdRequestStates.find(sessionKey);
    if (it == mdRequestStates.end()) {
        return;
    }

    auto &state = it->second;
    state.confirmObserved = true;
    if (state.expectedReplies > 0) {
        ++state.receivedReplies;
    }

    const bool repliesSatisfied = state.expectedReplies == 0 || state.receivedReplies >= state.expectedReplies;
    const bool confirmSatisfied = state.confirmObserved || state.confirmDeadline.time_since_epoch().count() == 0;

    if (repliesSatisfied && confirmSatisfied) {
        std::cout << "[TRDP] MD session " << formatMdSessionKey(sessionKey) << " received all expected replies ("
                  << state.receivedReplies << ") for ComId " << state.comId << std::endl;
        mdRequestStates.erase(it);
    }
}

void TrdpEngine::pruneMdTimeouts(std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mdRequestMtx);
    for (auto it = mdRequestStates.begin(); it != mdRequestStates.end();) {
        const auto &state = it->second;
        const bool replyExpired = state.expectedReplies > state.receivedReplies &&
                                  state.replyDeadline.time_since_epoch().count() > 0 && now >= state.replyDeadline;
        const bool confirmExpired = !state.confirmObserved &&
                                    state.confirmDeadline.time_since_epoch().count() > 0 &&
                                    now >= state.confirmDeadline;

        if (replyExpired || confirmExpired) {
            std::cerr << "[TRDP] MD session " << formatMdSessionKey(it->first) << " for ComId " << state.comId;
            if (replyExpired) {
                const auto missing = state.expectedReplies > state.receivedReplies
                                         ? state.expectedReplies - state.receivedReplies
                                         : 0U;
                std::cerr << " missing " << missing << " reply(ies) before timeout";
            }
            if (confirmExpired) {
                std::cerr << (replyExpired ? "; " : " ") << "confirm not received before timeout";
            }
            std::cerr << std::endl;
            it = mdRequestStates.erase(it);
            continue;
        }

        const bool repliesSatisfied = state.expectedReplies == 0 || state.receivedReplies >= state.expectedReplies;
        const bool confirmSatisfied = state.confirmObserved || state.confirmDeadline.time_since_epoch().count() == 0;
        if (repliesSatisfied && confirmSatisfied) {
            it = mdRequestStates.erase(it);
        } else {
            ++it;
        }
    }
}
#endif

TrdpEngine &TrdpEngine::instance() {
    static TrdpEngine engine;
    return engine;
}

bool TrdpEngine::bootstrapRegistry() {
    if (!ensureRegistryInitialized()) {
        std::cerr << "TRDP registry failed to initialise from XML" << std::endl;
        return false;
    }
    return true;
}

bool TrdpEngine::initialiseTrdpStack() {
    std::cout << "[TRDP] Initialising stack..." << std::endl;
    if (!stackAvailable) {
        std::cout << "[TRDP] Stack not available at build time; running in stub mode" << std::endl;
        pdSessionInitialised = true;
        mdSessionInitialised = true;
        return true;
    }

#ifdef TRDP_STACK_PRESENT
    constexpr std::size_t kHeapSize = 64 * 1024;
    heapStorage.assign(kHeapSize, 0U);

    TRDP_MEM_CONFIG_T memConfig{};
    memConfig.p = heapStorage.data();
    memConfig.size = static_cast<UINT32>(heapStorage.size());

    const TRDP_ERR_T initErr = tlc_init(nullptr, nullptr, &memConfig);
    if (initErr != TRDP_NO_ERR) {
        std::cerr << "[TRDP] tlc_init failed: " << initErr << std::endl;
        return false;
    }

    std::uint32_t sessionIp = 0U;
    const auto txIp = resolveInterfaceIp(config.txInterface);
    const auto rxIp = resolveInterfaceIp(config.rxInterface);
    if (config.txInterface.empty() && config.rxInterface.empty()) {
        sessionIp = 0U;
    } else if (txIp) {
        sessionIp = *txIp;
    } else if (rxIp) {
        sessionIp = *rxIp;
    }

    if (!config.txInterface.empty() && !txIp) {
        std::cerr << "[TRDP] Unable to resolve TX interface '" << config.txInterface
                  << "'; falling back to default stack selection" << std::endl;
    }
    if (!config.rxInterface.empty() && !rxIp) {
        std::cerr << "[TRDP] Unable to resolve RX interface '" << config.rxInterface
                  << "'; falling back to default stack selection" << std::endl;
    }

    if (sessionIp != 0U) {
        std::cout << "[TRDP] Binding TRDP sessions to interface IP " << formatIp(sessionIp) << std::endl;
    }

    std::set<std::uint16_t> pdPorts;
    std::set<std::uint16_t> mdPorts;
    bool hasPdTelegrams = false;
    bool hasMdTelegrams = false;
    for (const auto &telegram : TelegramRegistry::instance().listTelegrams()) {
        const auto addPort = [](std::set<std::uint16_t> &ports, std::uint16_t port) {
            if (port != 0U) {
                ports.insert(port);
            }
        };
        if (telegram.type == TelegramType::PD) {
            hasPdTelegrams = true;
            addPort(pdPorts, telegram.srcPort);
            addPort(pdPorts, telegram.destPort);
        } else {
            hasMdTelegrams = true;
            addPort(mdPorts, telegram.srcPort);
            addPort(mdPorts, telegram.destPort);
        }
    }

    if (!hasPdTelegrams) {
        std::cout << "[TRDP] No PD telegrams configured; skipping PD stack setup" << std::endl;
    }
    if (!hasMdTelegrams) {
        std::cout << "[TRDP] No MD telegrams configured; skipping MD stack setup" << std::endl;
    }

    if (hasPdTelegrams && pdPorts.empty()) {
        pdPorts.insert(resolveDefaultPort(TelegramType::PD));
    }
    if (hasMdTelegrams && mdPorts.empty()) {
        mdPorts.insert(resolveDefaultPort(TelegramType::MD));
    }

    auto openPdSession = [&](std::uint16_t port) {
        TRDP_PD_CONFIG_T pdDefault{};
        pdDefault.port = port;
        pdDefault.sendParam = TRDP_PD_DEFAULT_SEND_PARAM;
        pdDefault.sendParam.ttl = 64U;
        pdDefault.flags = TRDP_FLAGS_NONE;
        pdDefault.timeout = TRDP_PD_DEFAULT_TIMEOUT;
        pdDefault.toBehavior = TRDP_TO_SET_TO_ZERO;

        TRDP_APP_SESSION_T session{};
        const TRDP_ERR_T pdErr = tlc_openSession(&session, sessionIp, 0U, nullptr, &pdDefault, nullptr, nullptr);
        if (pdErr != TRDP_NO_ERR) {
            std::cerr << "[TRDP] tlc_openSession for PD port " << port << " failed: " << pdErr << std::endl;
            return false;
        }
        pdSessions.emplace(port, session);
        return true;
    };

    auto openMdSession = [&](std::uint16_t port) {
        TRDP_MD_CONFIG_T mdDefault{};
        mdDefault.pfCbFunction = nullptr;
        mdDefault.pRefCon = nullptr;
        mdDefault.sendParam = TRDP_MD_DEFAULT_SEND_PARAM;
        mdDefault.flags = TRDP_FLAGS_NONE;
        mdDefault.replyTimeout = TRDP_MD_DEFAULT_REPLY_TIMEOUT;
        mdDefault.confirmTimeout = TRDP_MD_DEFAULT_CONFIRM_TIMEOUT;
        mdDefault.connectTimeout = TRDP_MD_DEFAULT_CONNECTION_TIMEOUT;
        mdDefault.sendingTimeout = TRDP_MD_DEFAULT_SENDING_TIMEOUT;
        mdDefault.udpPort = port;
        mdDefault.tcpPort = port;
        mdDefault.maxNumSessions = TRDP_MD_MAX_NUM_SESSIONS;

        TRDP_APP_SESSION_T session{};
        const TRDP_ERR_T mdErr = tlc_openSession(&session, sessionIp, 0U, nullptr, nullptr, &mdDefault, nullptr);
        if (mdErr != TRDP_NO_ERR) {
            std::cerr << "[TRDP] tlc_openSession for MD port " << port << " failed: " << mdErr << std::endl;
            return false;
        }

        TRDP_TIME_T interval{};
        TRDP_FDS_T readFds{};
        TRDP_SOCK_T maxFd = 0;
        if (tlc_getInterval(session, &interval, &readFds, &maxFd) == TRDP_NO_ERR && maxFd >= 0) {
            std::set<std::uint16_t> boundPorts;
            for (TRDP_SOCK_T fd = 0; fd <= maxFd; ++fd) {
                if (!FD_ISSET(fd, &readFds)) {
                    continue;
                }

                sockaddr_storage addr{};
                socklen_t addrLen = sizeof(addr);
                if (getsockname(static_cast<int>(fd), reinterpret_cast<sockaddr *>(&addr), &addrLen) != 0) {
                    continue;
                }

                if (addr.ss_family == AF_INET) {
                    const auto *inAddr = reinterpret_cast<const sockaddr_in *>(&addr);
                    boundPorts.insert(ntohs(inAddr->sin_port));
                }
            }

            if (!boundPorts.empty() && (boundPorts.count(port) == 0U || boundPorts.size() != 1U)) {
                std::cerr << "[TRDP] Requested MD port " << port << ", bound to ";
                bool first = true;
                for (auto boundPort : boundPorts) {
                    if (!first) {
                        std::cerr << ',' << ' ';
                    }
                    std::cerr << boundPort;
                    first = false;
                }
                std::cerr << std::endl;
            }
        }

        mdSessions.emplace(port, session);
        return true;
    };

    for (auto port : pdPorts) {
        openPdSession(port);
    }
    for (auto port : mdPorts) {
        openMdSession(port);
    }

    pdSessionInitialised = hasPdTelegrams && !pdSessions.empty();
    mdSessionInitialised = hasMdTelegrams && !mdSessions.empty();

    const bool pdStackReady = !hasPdTelegrams || pdSessionInitialised;
    const bool mdStackReady = !hasMdTelegrams || mdSessionInitialised;

    if (!pdStackReady || !mdStackReady) {
        if (!pdStackReady && hasPdTelegrams) {
            std::cerr << "[TRDP] PD stack failed to initialise for configured telegrams" << std::endl;
        }
        if (!mdStackReady && hasMdTelegrams) {
            std::cerr << "[TRDP] MD stack failed to initialise for configured telegrams" << std::endl;
        }
        return false;
    }

    const bool anySessionInitialised = pdSessionInitialised || mdSessionInitialised;

    if (config.enableDnr) {
        if (anySessionInitialised) {
            if (!initialiseDnr()) {
                teardownTrdpStack();
                return false;
            }
        } else {
            std::cout << "[TRDP] DNR enabled in config but no TRDP sessions are active; skipping initialisation" << std::endl;
        }
    }

    if (config.ecspConfig.enable) {
        if (anySessionInitialised) {
            initialiseEcsp();
        } else {
            std::cout << "[TRDP] ECSP enabled in config but no TRDP sessions are active; skipping initialisation" << std::endl;
        }
    }

    // Apply any updated session defaults or interface selections.
    for (auto &[port, session] : pdSessions) {
        (void)port;
        (void)tlc_configSession(session, nullptr, nullptr, nullptr, nullptr);
        (void)tlc_updateSession(session);
    }
    for (auto &[port, session] : mdSessions) {
        (void)port;
        (void)tlc_configSession(session, nullptr, nullptr, nullptr, nullptr);
        (void)tlc_updateSession(session);
    }
#else
    pdSessionInitialised = true;
    mdSessionInitialised = true;
#endif

    if (pdSessionInitialised) {
        std::cout << "[TRDP] PD session handle ready on ports";
        for (const auto &[port, session] : pdSessions) {
            (void)session;
            std::cout << ' ' << port;
        }
        std::cout << std::endl;
    } else {
        std::cout << "[TRDP] PD stack inactive" << std::endl;
    }
    if (mdSessionInitialised) {
        std::cout << "[TRDP] MD session handle ready on ports";
        for (const auto &[port, session] : mdSessions) {
            (void)session;
            std::cout << ' ' << port;
        }
        std::cout << std::endl;
    } else {
        std::cout << "[TRDP] MD stack inactive" << std::endl;
    }
    return true;
}

void TrdpEngine::teardownTrdpStack() {
    if (!pdSessionInitialised && !mdSessionInitialised) {
        return;
    }

    if (stackAvailable) {
#ifdef TRDP_STACK_PRESENT
        for (auto &[port, session] : mdSessions) {
            tlc_configSession(session, nullptr, nullptr, nullptr, nullptr);
            (void)tlc_closeSession(session);
            (void)port;
        }
        for (auto &[port, session] : pdSessions) {
            tlc_configSession(session, nullptr, nullptr, nullptr, nullptr);
            (void)tlc_closeSession(session);
            (void)port;
        }
        if (dnrInitialised) {
            if constexpr (kDnrCompiledIn) {
                tau_deInitDnr(defaultPdSession() ? defaultPdSession() : defaultMdSession());
            }
            dnrInitialised = false;
        }
        tlc_terminate();
        ecspInitialised = false;
        mdSessions.clear();
        pdSessions.clear();
#else
        std::cout << "[TRDP] Stack not available; stub teardown" << std::endl;
#endif
    }

    mdSessionInitialised = false;
    pdSessionInitialised = false;
}

std::chrono::milliseconds TrdpEngine::stackIntervalHint() const {
    if (stackAvailable) {
#ifdef TRDP_STACK_PRESENT
        const auto resolveInterval = [&](TRDP_APP_SESSION_T session) -> std::optional<std::chrono::milliseconds> {
            if (!session) {
                return std::nullopt;
            }

            TRDP_TIME_T interval{};
            TRDP_FDS_T readFds{};
            TRDP_FDS_T writeFds{};
            INT32 noDesc = 0;
            TRDP_ERR_T err = tlc_getInterval(session, &interval, &readFds, &noDesc);
            if (err != TRDP_NO_ERR) {
                err = tlp_getInterval(session, &interval, &writeFds, &noDesc);
            }

            if (err == TRDP_NO_ERR) {
                return toDuration(interval);
            }

            std::cerr << "[TRDP] Failed to obtain stack interval: " << err << std::endl;
            return std::nullopt;
        };

        std::optional<std::chrono::milliseconds> minInterval;
        if (pdSessionInitialised) {
            for (const auto &[port, session] : pdSessions) {
                (void)port;
                if (const auto pdInterval = resolveInterval(session)) {
                    if (!minInterval || *pdInterval < *minInterval) {
                        minInterval = *pdInterval;
                    }
                }
            }
        }
        if (mdSessionInitialised) {
            for (const auto &[port, session] : mdSessions) {
                (void)port;
                if (const auto mdInterval = resolveInterval(session)) {
                    if (!minInterval || *mdInterval < *minInterval) {
                        minInterval = *mdInterval;
                    }
                }
            }
        }
        if (minInterval) {
            return *minInterval;
        }
#endif
    }
    if (config.idleInterval.count() > 0) {
        return config.idleInterval;
    }
    return std::chrono::milliseconds(100);
}

#ifdef TRDP_STACK_PRESENT
std::optional<TrdpEngine::StackSelectContext> TrdpEngine::prepareSelectContext(TRDP_APP_SESSION_T session) const {
    if (!stackAvailable || session == nullptr) {
        return std::nullopt;
    }

    StackSelectContext context{};
    TRDP_ERR_T err = tlc_getInterval(session, &context.interval, &context.readFds, &context.maxFd);
    if (err != TRDP_NO_ERR) {
        err = tlp_getInterval(session, &context.interval, &context.writeFds, &context.maxFd);
    }

    if (err != TRDP_NO_ERR) {
        std::cerr << "[TRDP] Failed to obtain stack interval: " << err << std::endl;
        return std::nullopt;
    }

    context.valid = true;
    return context;
}
#endif

void TrdpEngine::logConfigError(const std::string &context, TRDP_ERR_T err) const {
    std::cerr << "[TRDP] " << context << " failed: " <<
#ifdef TRDP_STACK_PRESENT
        describeTrdpError(err)
#else
        err
#endif
        ;
    if (!config.hostsFile.empty()) {
        std::cerr << " (hosts file: " << config.hostsFile << ")";
    }
    std::cerr << std::endl;
}

void TrdpEngine::trimCaches() {
    if (!config.cacheConfig.enableUriCache) {
        uriCache.clear();
        ipCache.clear();
        labelCache.clear();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto eraseExpired = [&](auto &cache) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (now >= it->second.expiresAt) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    };

    eraseExpired(uriCache);
    eraseExpired(ipCache);
    eraseExpired(labelCache);
    updateCacheLimits();
}

void TrdpEngine::updateCacheLimits() {
    const auto enforceLimit = [&](auto &cache) {
        while (cache.size() > config.cacheConfig.uriCacheEntries) {
            cache.erase(cache.begin());
        }
    };

    enforceLimit(uriCache);
    enforceLimit(ipCache);
    enforceLimit(labelCache);
}

void TrdpEngine::setCacheEntry(CacheEntry &entry, CacheEntry::Payload value) {
    entry.payload = std::move(value);
    if (config.cacheConfig.uriCacheTtl.count() > 0) {
        entry.expiresAt = std::chrono::steady_clock::now() + config.cacheConfig.uriCacheTtl;
    } else {
        entry.expiresAt = std::chrono::steady_clock::now();
    }
}

void TrdpEngine::markTopologyChanged() {
    ++etbTopoCounter;
    ++opTrainTopoCounter;
    topologyCountersDirty = true;
    std::cout << "[TRDP] Topology change detected; ETB=" << etbTopoCounter
              << " OpTrain=" << opTrainTopoCounter << std::endl;
}

bool TrdpEngine::publishPdBuffer(EndpointHandle &endpoint, const std::vector<std::uint8_t> &buffer) {
    if (!endpoint.pdHandleReady) {
        std::cerr << "[TRDP] PD session not available; drop TX ComId " << endpoint.def.comId << std::endl;
        return false;
    }
#ifdef TRDP_STACK_PRESENT
    if (stackAvailable) {
        TRDP_ERR_T err = tlp_put(endpoint.pdSessionHandle, endpoint.pdPublishHandle, buffer.data(),
                                 static_cast<UINT32>(buffer.size()));
        if (err != TRDP_NO_ERR) {
            std::cerr << "[TRDP] tlp_put failed for ComId " << endpoint.def.comId << ": " << err << std::endl;
            return false;
        }
    }
#endif
    std::cout << "[TRDP] PD send ComId=" << endpoint.def.comId << " bytes=" << buffer.size() << std::endl;
    return true;
}

bool TrdpEngine::initialiseDnr() {
#if defined(TRDP_STACK_PRESENT) && TRDP_HAS_TAU_DNR
    if constexpr (!kDnrCompiledIn) {
        logDnrUnavailable("TAU DNR APIs not available in detected stack; host lookups are disabled");
        return true;
    }

    if constexpr (kDnrCompiledIn) {
        const char *hostsFile = config.hostsFile.empty() ? nullptr : config.hostsFile.c_str();

        const TRDP_APP_SESSION_T session = pdSessionInitialised ? defaultPdSession() : defaultMdSession();
        const TRDP_DNR_OPTS_T dnrMode = mapDnrMode(config.dnrMode);
        const TRDP_ERR_T dnrErr = tau_initDnr(session, 0U, 0U, hostsFile, dnrMode, TRUE);
        if (dnrErr != TRDP_NO_ERR) {
            logConfigError("tau_initDnr", dnrErr);
            return false;
        }
        dnrInitialised = true;
        std::cout << "[TRDP] DNR initialised";
        if (hostsFile != nullptr) {
            std::cout << " (hosts file: " << hostsFile << ")";
        }
        std::cout << std::endl;
    }
#else
    if (!kDnrCompiledIn) {
        logDnrUnavailable("TAU DNR APIs not available in detected stack; host lookups are disabled");
    } else {
        logDnrUnavailable("TRDP stack not present in this build; host lookups are disabled");
    }
#endif
    return true;
}

void TrdpEngine::initialiseEcsp() {
#if defined(TRDP_STACK_PRESENT) && defined(TRDP_HAS_TAU_ECSP)
    TRDP_ECSPCTRL_CONFIG_T ecspConfig{};
    ecspConfig.confirmTimeout = static_cast<UINT32>(config.ecspConfig.confirmTimeout.count());
    const TRDP_ERR_T initErr = tau_initEcspCtrl(&ecspConfig);
    if (initErr != TRDP_NO_ERR) {
        logConfigError("tau_initEcspCtrl", initErr);
        return;
    }
    ecspInitialised = true;
    updateEcspControl();
#endif
}

void TrdpEngine::updateEcspControl() {
#if defined(TRDP_STACK_PRESENT) && defined(TRDP_HAS_TAU_ECSP)
    if (!ecspInitialised) {
        return;
    }
    TRDP_ECSPCTRL_PARMS_T parms{};
    parms.enable = config.ecspConfig.enable ? TRUE : FALSE;
    parms.confirmTimeout = static_cast<UINT32>(config.ecspConfig.confirmTimeout.count());
    const TRDP_ERR_T setErr = tau_setEcspCtrl(&parms);
    if (setErr != TRDP_NO_ERR) {
        logConfigError("tau_setEcspCtrl", setErr);
    }
#endif
}

void TrdpEngine::pollEcspStatus() {
#if defined(TRDP_STACK_PRESENT) && defined(TRDP_HAS_TAU_ECSP)
    if (!ecspInitialised) {
        return;
    }
    static auto lastPoll = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (lastPoll.time_since_epoch().count() != 0 &&
        now - lastPoll < std::max(config.ecspConfig.pollInterval, std::chrono::milliseconds(10))) {
        return;
    }
    lastPoll = now;
    TRDP_ECSPCTRL_STAT_T status{};
    const TRDP_ERR_T statErr = tau_getEcspStat(&status);
    if (statErr != TRDP_NO_ERR) {
        logConfigError("tau_getEcspStat", statErr);
    }
#endif
}

bool TrdpEngine::processStackOnce(
#ifdef TRDP_STACK_PRESENT
    const StackSelectContext *pdContext, const StackSelectContext *mdContext
#else
    void *pdContext, void *mdContext
#endif
) {
    if (!running.load()) {
        return false;
    }

#ifndef TRDP_STACK_PRESENT
    (void)pdContext;
    (void)mdContext;
#endif

    if (stackAvailable) {
#ifdef TRDP_STACK_PRESENT
        const auto setTopologyCounters = [&](TRDP_APP_SESSION_T session) {
            if (!session) {
                return;
            }
            const TRDP_ERR_T etbErr = tlc_setETBTopoCount(session, etbTopoCounter);
            const TRDP_ERR_T opErr = tlc_setOpTrainTopoCount(session, opTrainTopoCounter);
            if (etbErr != TRDP_NO_ERR || opErr != TRDP_NO_ERR) {
                std::cerr << "[TRDP] Failed to update topology counters; ETB err=" << etbErr
                          << " OpTrain err=" << opErr << std::endl;
            }
        };

        if (topologyCountersDirty) {
            if (pdSessionInitialised) {
                for (const auto &[port, session] : pdSessions) {
                    (void)port;
                    setTopologyCounters(session);
                }
            }
            if (mdSessionInitialised) {
                for (const auto &[port, session] : mdSessions) {
                    (void)port;
                    setTopologyCounters(session);
                }
            }
            topologyCountersDirty = false;
        }

        if (pdSessionInitialised) {
            bool firstPd = true;
            for (const auto &[port, session] : pdSessions) {
                (void)port;
                const auto *ctx = (firstPd && pdContext) ? pdContext : nullptr;
                TRDP_FDS_T pdFds = ctx ? ctx->readFds : TRDP_FDS_T{};
                INT32 pdDesc = ctx ? ctx->maxFd : 0;
                const TRDP_ERR_T pdErr = tlc_process(session, ctx ? &pdFds : nullptr, ctx ? &pdDesc : nullptr);
                if (pdErr != TRDP_NO_ERR) {
                    std::cerr << "[TRDP] tlc_process (PD) failed: " << pdErr << std::endl;
                }
                firstPd = false;
            }
        }
        if (mdSessionInitialised) {
            bool firstMd = true;
            for (const auto &[port, session] : mdSessions) {
                (void)port;
                const auto *ctx = (firstMd && mdContext) ? mdContext : nullptr;
                TRDP_FDS_T mdFds = ctx ? ctx->readFds : TRDP_FDS_T{};
                INT32 mdDesc = ctx ? ctx->maxFd : 0;
                const TRDP_ERR_T mdErr = tlc_process(session, ctx ? &mdFds : nullptr, ctx ? &mdDesc : nullptr);
                if (mdErr != TRDP_NO_ERR) {
                    std::cerr << "[TRDP] tlc_process (MD) failed: " << mdErr << std::endl;
                }
                firstMd = false;
            }
        }
        if (config.ecspConfig.enable) {
            pollEcspStatus();
        }
#endif
    }

    return true;
}

void TrdpEngine::dispatchCyclicTransmissions(std::chrono::steady_clock::time_point now) {
    for (auto &[comId, endpoint] : endpoints) {
        if (endpoint.def.type != TelegramType::PD || endpoint.def.direction != Direction::Tx) {
            continue;
        }
        if (!endpoint.txCyclicActive || endpoint.cycle.count() <= 0) {
            continue;
        }
        if (endpoint.nextSend.time_since_epoch().count() == 0) {
            endpoint.nextSend = now + endpoint.cycle;
            continue;
        }
        if (now < endpoint.nextSend) {
            continue;
        }

        const auto buffer = endpoint.runtime->getBufferCopy();
        if (publishPdBuffer(endpoint, buffer)) {
            endpoint.nextSend = now + endpoint.cycle;
            if (auto *hub = TelegramHub::instance()) {
                hub->publishTxConfirmation(comId, endpoint.runtime->snapshotFields());
            }
        } else {
            endpoint.txCyclicActive = false;
        }
    }
}

void TrdpEngine::buildEndpoints() {
    endpoints.clear();

    for (const auto &telegram : TelegramRegistry::instance().listTelegrams()) {
        auto runtime = TelegramRegistry::instance().getOrCreateRuntime(telegram.comId);
        if (!runtime) {
            std::cerr << "[TRDP] Failed to allocate runtime for ComId " << telegram.comId << std::endl;
            continue;
        }
        EndpointHandle handle{.def = telegram, .runtime = runtime, .cycle = telegram.cycle};

        if (telegram.type == TelegramType::MD) {
            handle.mdSessionHandle = mdSessionForPort(resolvePortForEndpoint(telegram));
            const bool hasRequestedPort = mdSessions.find(resolvePortForEndpoint(telegram)) != mdSessions.end();
            const std::uint16_t boundPort = hasRequestedPort ? resolvePortForEndpoint(telegram)
                                                             : (mdSessions.empty() ? 0U : mdSessions.begin()->first);
            handle.mdHandleReady = mdSessionInitialised && handle.mdSessionHandle != nullptr;
#ifdef TRDP_STACK_PRESENT
            std::cout << "[TRDP] MD endpoint selection for ComId " << telegram.comId << " requestedPort="
                      << resolvePortForEndpoint(telegram) << " boundPort=" << boundPort << " flags=0x" << std::hex
                      << telegram.trdpFlags << std::dec << " qos=" << static_cast<unsigned>(telegram.qos)
                      << std::endl;
            if (boundPort != resolvePortForEndpoint(telegram)) {
                std::cerr << "[TRDP] MD session port mismatch for ComId " << telegram.comId << " (requested "
                          << resolvePortForEndpoint(telegram) << ", bound " << boundPort
                          << ")" << std::endl;
            }
            if (handle.mdHandleReady && stackAvailable) {
                TRDP_URI_USER_T emptyUri{};
                TRDP_ERR_T mdErr =
                    tlm_addListener(handle.mdSessionHandle, &handle.mdListenerHandle, this, mdReceiveCallback, TRUE,
                                    telegram.comId, etbTopoCounter, opTrainTopoCounter, telegram.srcIp, telegram.srcIp,
                                    telegram.destIp, 0U, emptyUri, emptyUri);
                handle.mdHandleReady = (mdErr == TRDP_NO_ERR);
                if (mdErr != TRDP_NO_ERR) {
                    std::cerr << "[TRDP] tlm_addListener failed for ComId " << telegram.comId << ": " << mdErr
                              << std::endl;
                }
            }
#endif
            if (handle.mdHandleReady) {
                std::cout << "[TRDP] Binding MD endpoint for ComId " << telegram.comId << std::endl;
            } else if (!mdSessionInitialised) {
                std::cerr << "[TRDP] MD session not initialised; unable to bind ComId " << telegram.comId
                          << std::endl;
            } else {
                std::cerr << "[TRDP] Failed to bind MD endpoint for ComId " << telegram.comId
                          << "; see previous errors" << std::endl;
            }
        } else {
            const auto requestedPort = resolvePortForEndpoint(telegram);
            const bool hasRequestedPort = pdSessions.find(requestedPort) != pdSessions.end();
            const std::uint16_t boundPort = hasRequestedPort ? requestedPort
                                                             : (pdSessions.empty() ? 0U : pdSessions.begin()->first);
            handle.pdSessionHandle = pdSessionForPort(requestedPort);
            handle.pdHandleReady = pdSessionInitialised && handle.pdSessionHandle != nullptr;
#ifdef TRDP_STACK_PRESENT
            std::cout << "[TRDP] PD endpoint selection for ComId " << telegram.comId << " requestedPort="
                      << requestedPort << " boundPort=" << boundPort << " flags=0x" << std::hex << telegram.trdpFlags
                      << std::dec << " qos=" << static_cast<unsigned>(telegram.qos) << std::endl;
            if (boundPort != requestedPort) {
                std::cerr << "[TRDP] PD session port mismatch for ComId " << telegram.comId << " (requested "
                          << requestedPort << ", bound " << boundPort << ")" << std::endl;
            }
            if (handle.pdHandleReady && stackAvailable) {
                if (telegram.direction == Direction::Tx && !ipAssignedToLocalInterface(telegram.srcIp)) {
                    std::cerr << "[TRDP] Source IP " << formatIp(telegram.srcIp) << " for ComId " << telegram.comId
                              << " is not configured on this host; TRDP will reject the publish request. "
                              << "Update the XML or set TRDP_TX_IFACE/TRDP_RX_IFACE to match a local IPv4 address."
                              << std::endl;
                    handle.pdHandleReady = false;
                }
            }
            if (handle.pdHandleReady && stackAvailable) {
                auto destIp = telegram.destIp;
                if (telegram.direction == Direction::Rx && destIp != 0U && !ipAssignedToLocalInterface(destIp)) {
                    std::cerr << "[TRDP] Destination IP " << formatIp(destIp) << " for ComId " << telegram.comId
                              << " is not configured on this host; subscribing with a wildcard address."
                              << std::endl;
                    destIp = 0U;
                }
                TRDP_SEND_PARAM_T sendParam = TRDP_PD_DEFAULT_SEND_PARAM;
                sendParam.ttl = telegram.ttl;
                TRDP_COM_PARAM_T recvParams = TRDP_PD_DEFAULT_SEND_PARAM;
                recvParams.ttl = telegram.ttl;
                applyTelegramQos(telegram, sendParam);
                applyTelegramPorts(telegram, sendParam);
                applyTelegramQos(telegram, recvParams);
                applyTelegramPorts(telegram, recvParams);
                const TRDP_FLAGS_T pdFlags = static_cast<TRDP_FLAGS_T>(telegram.trdpFlags);
                const auto buffer = runtime->getBufferCopy();
                TRDP_ERR_T pdErr{};
                if (telegram.direction == Direction::Tx) {
                    const auto intervalMs = static_cast<UINT32>(telegram.cycle.count());
                    constexpr UINT32 redundancyId = 0U;
                    pdErr = tlp_publish(handle.pdSessionHandle, &handle.pdPublishHandle, this, nullptr, 0U,
                                        telegram.comId, etbTopoCounter, opTrainTopoCounter, telegram.srcIp,
                                        telegram.destIp, intervalMs, redundancyId, pdFlags, &sendParam, buffer.data(),
                                        static_cast<UINT32>(buffer.size()));
                } else {
                    constexpr UINT32 redundancyId = 0U;
                    pdErr = tlp_subscribe(handle.pdSessionHandle, &handle.pdSubscribeHandle, this, pdReceiveCallback, 0U,
                                           telegram.comId, etbTopoCounter, opTrainTopoCounter, telegram.srcIp,
                                           redundancyId, destIp, pdFlags, &recvParams, 0U,
                                           static_cast<TRDP_TO_BEHAVIOR_T>(0U));
                }
                handle.pdHandleReady = (pdErr == TRDP_NO_ERR);
                if (pdErr != TRDP_NO_ERR) {
                    std::cerr << "[TRDP] PD binding failed for ComId " << telegram.comId << ": "
                              << describeTrdpError(pdErr) << std::endl;
                }
            }
#endif
            if (handle.pdHandleReady) {
                std::cout << "[TRDP] Binding PD endpoint for ComId " << telegram.comId << std::endl;
            } else if (!pdSessionInitialised) {
                std::cerr << "[TRDP] PD session not initialised; unable to bind ComId " << telegram.comId
                          << std::endl;
            } else {
                std::cerr << "[TRDP] Failed to bind PD endpoint for ComId " << telegram.comId
                          << "; see previous errors" << std::endl;
            }
        }
        endpoints.emplace(telegram.comId, std::move(handle));
    }
}

bool TrdpEngine::start() {
    return start(TrdpConfig{});
}

bool TrdpEngine::start(const TrdpConfig &cfg) {
    std::lock_guard lock(stateMtx);
    const bool configChanged = !running.load() || config.rxInterface != cfg.rxInterface || config.txInterface != cfg.txInterface ||
                               config.hostsFile != cfg.hostsFile || config.enableDnr != cfg.enableDnr ||
                               config.dnrMode != cfg.dnrMode || config.cacheConfig.enableUriCache != cfg.cacheConfig.enableUriCache ||
                               config.cacheConfig.uriCacheTtl != cfg.cacheConfig.uriCacheTtl ||
                               config.cacheConfig.uriCacheEntries != cfg.cacheConfig.uriCacheEntries ||
                               config.ecspConfig.enable != cfg.ecspConfig.enable ||
                               config.ecspConfig.pollInterval != cfg.ecspConfig.pollInterval ||
                               config.ecspConfig.confirmTimeout != cfg.ecspConfig.confirmTimeout ||
                               config.idleInterval != cfg.idleInterval;

    if (running.load()) {
        if (configChanged) {
            config = cfg;
            markTopologyChanged();
            trimCaches();
            updateEcspControl();
        }
        return true;
    }

    config = cfg;
    if (configChanged) {
        markTopologyChanged();
    }
#ifdef TRDP_STACK_PRESENT
    stackAvailable = true;
#else
    stackAvailable = false;
#endif

    if (config.enableDnr && (!stackAvailable || !kDnrCompiledIn)) {
        if (!stackAvailable) {
            logDnrUnavailable("TRDP stack not present in this build; TAU DNR lookups are disabled");
        } else {
            logDnrUnavailable("TAU DNR APIs not available in detected stack; host lookups are disabled");
        }
    }

    if (!bootstrapRegistry()) {
        return false;
    }
    if (!initialiseTrdpStack()) {
        teardownTrdpStack();
        return false;
    }

    buildEndpoints();
    if (endpoints.empty()) {
        std::cerr << "[TRDP] No telegrams registered; nothing to start" << std::endl;
    }
    stopRequested.store(false);
    running.store(true);
    worker = std::thread([this]() { processingLoop(); });
    return true;
}

void TrdpEngine::stop() {
    {
        std::lock_guard lock(stateMtx);
        if (!running.load()) {
            return;
        }
        stopRequested.store(true);
    }
    cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    running.store(false);
#ifdef TRDP_STACK_PRESENT
    {
        std::lock_guard mdLock(mdRequestMtx);
        mdRequestStates.clear();
    }
#endif
    teardownTrdpStack();
    endpoints.clear();
    std::cout << "[TRDP] Stack stopped" << std::endl;
}

TrdpEngine::EndpointHandle *TrdpEngine::findEndpoint(std::uint32_t comId) {
    auto it = endpoints.find(comId);
    if (it == endpoints.end()) {
        return nullptr;
    }
    return &it->second;
}

bool TrdpEngine::sendTxTelegram(std::uint32_t comId, const std::map<std::string, FieldValue> &txFields) {
    std::lock_guard lock(stateMtx);
    auto *endpoint = findEndpoint(comId);
    if (endpoint == nullptr) {
        std::cerr << "[TRDP] Unknown TX ComId " << comId << std::endl;
        return false;
    }
    if (endpoint->def.direction != Direction::Tx) {
        std::cerr << "[TRDP] ComId " << comId << " is not marked as TX" << std::endl;
        return false;
    }

    for (const auto &[name, value] : txFields) {
        endpoint->runtime->setFieldValue(name, value);
    }

    const auto mergedFields = mergeRuntimeFields(*endpoint->runtime, txFields);
    const auto buffer = encodeFieldsToBuffer(*endpoint->runtime, mergedFields);
    endpoint->runtime->overwriteBuffer(buffer);

    bool sent = false;
    if (endpoint->def.type == TelegramType::MD) {
        if (!endpoint->mdHandleReady) {
            std::cerr << "[TRDP] MD session not available; drop TX ComId " << comId << std::endl;
            return false;
        }
#ifdef TRDP_STACK_PRESENT
        if (stackAvailable) {
            TRDP_SEND_PARAM_T sendParam = TRDP_MD_DEFAULT_SEND_PARAM;
            sendParam.ttl = endpoint->def.ttl;
            applyTelegramQos(endpoint->def, sendParam);
            applyTelegramPorts(endpoint->def, sendParam);
            const auto numReplies = static_cast<UINT32>(endpoint->def.expectedReplies);
            const auto replyTimeout = static_cast<UINT32>(endpoint->def.replyTimeout.count());
            const auto confirmTimeout = static_cast<UINT32>(endpoint->def.confirmTimeout.count());
            TRDP_ERR_T err = tlm_request(endpoint->mdSessionHandle, this, mdReceiveCallback, &endpoint->mdSessionId, comId,
                                         etbTopoCounter, opTrainTopoCounter, endpoint->def.srcIp, endpoint->def.destIp,
                                         numReplies, replyTimeout, confirmTimeout, &sendParam, buffer.data(),
                                         static_cast<UINT32>(buffer.size()), nullptr, nullptr);
            if (err != TRDP_NO_ERR) {
                std::cerr << "[TRDP] tlm_request failed for ComId " << comId << ": " << err << std::endl;
                return false;
            }
            trackMdRequest(mdSessionKeyFromId(endpoint->mdSessionId), *endpoint);
        }
#endif
        std::cout << "[TRDP] MD send ComId=" << comId << " bytes=" << buffer.size() << std::endl;
        sent = true;
    } else {
        sent = publishPdBuffer(*endpoint, buffer);
    }

    if (sent) {
        if (auto *hub = TelegramHub::instance()) {
            hub->publishTxConfirmation(comId, mergedFields);
        }
    }

    if (sent && endpoint->def.type == TelegramType::PD && endpoint->cycle.count() > 0) {
        endpoint->txCyclicActive = true;
        endpoint->nextSend = std::chrono::steady_clock::now() + endpoint->cycle;
    }

    return sent;
}

void TrdpEngine::handleRxTelegram(std::uint32_t comId, const std::vector<std::uint8_t> &payload) {
    auto *endpoint = findEndpoint(comId);
    if (endpoint == nullptr) {
        std::cerr << "[TRDP] Received unknown ComId " << comId << std::endl;
        return;
    }
    if (endpoint->def.direction != Direction::Rx) {
        std::cerr << "[TRDP] Received RX telegram for TX ComId " << comId << std::endl;
        return;
    }

    decodeFieldsIntoRuntime(endpoint->runtime->dataset(), *endpoint->runtime, payload);

    if (auto *hub = TelegramHub::instance()) {
        hub->publishRxUpdate(comId, endpoint->runtime->snapshotFields());
    }
}

void TrdpEngine::handleRxMdTelegram(std::uint32_t comId, const std::vector<std::uint8_t> &payload) {
    std::cout << "[TRDP] MD telegram callback ComId=" << comId << " bytes=" << payload.size() << std::endl;
    handleRxTelegram(comId, payload);
}

std::optional<std::uint32_t> TrdpEngine::uriToIp(const std::string &uri, bool useCache) {
    trimCaches();
    if (useCache && config.cacheConfig.enableUriCache) {
        if (const auto cached = fetchCached<std::uint32_t>(uriCache, uri)) {
            return cached;
        }
    }

#if defined(TRDP_STACK_PRESENT) && TRDP_HAS_TAU_DNR
    if (!dnrInitialised) {
        logDnrUnavailable("DNR not initialised; URI lookups are disabled");
        return std::nullopt;
    }
    if (!pdSessionInitialised && !mdSessionInitialised) {
        return std::nullopt;
    }
    TRDP_IP_ADDR_T addr{};
    const TRDP_APP_SESSION_T session = pdSessionInitialised ? defaultPdSession() : defaultMdSession();
    const TRDP_ERR_T err = tau_uri2Addr(session, &addr, uri.c_str());
    if (err != TRDP_NO_ERR) {
        logConfigError("tau_uri2Addr", err);
        return std::nullopt;
    }
    if (config.cacheConfig.enableUriCache && useCache) {
        setCacheEntry(uriCache[uri], static_cast<std::uint32_t>(addr));
    }
    return static_cast<std::uint32_t>(addr);
#else
    if (!kDnrCompiledIn) {
        logDnrUnavailable("TAU DNR APIs not available in detected stack; URI lookups are disabled");
    } else {
        logDnrUnavailable("TRDP stack not present in this build; URI lookups are disabled");
    }
    (void)useCache;
    (void)uri;
    return std::nullopt;
#endif
}

std::optional<std::string> TrdpEngine::ipToUri(std::uint32_t ipAddr, bool useCache) {
    trimCaches();
    if (useCache && config.cacheConfig.enableUriCache) {
        if (const auto cached = fetchCached<std::string>(ipCache, ipAddr)) {
            return cached;
        }
    }

#if defined(TRDP_STACK_PRESENT) && TRDP_HAS_TAU_DNR
    if (!dnrInitialised) {
        logDnrUnavailable("DNR not initialised; URI lookups are disabled");
        return std::nullopt;
    }
    if (!pdSessionInitialised && !mdSessionInitialised) {
        return std::nullopt;
    }
    std::array<char, 256> uriBuffer{};
    const TRDP_APP_SESSION_T session = pdSessionInitialised ? defaultPdSession() : defaultMdSession();
    const TRDP_ERR_T err = tau_addr2Uri(session, uriBuffer.data(), static_cast<TRDP_IP_ADDR_T>(ipAddr));
    if (err != TRDP_NO_ERR) {
        logConfigError("tau_addr2Uri", err);
        return std::nullopt;
    }
    std::string resolved(uriBuffer.data());
    if (config.cacheConfig.enableUriCache && useCache) {
        setCacheEntry(ipCache[ipAddr], resolved);
    }
    return resolved;
#else
    if (!kDnrCompiledIn) {
        logDnrUnavailable("TAU DNR APIs not available in detected stack; URI lookups are disabled");
    } else {
        logDnrUnavailable("TRDP stack not present in this build; URI lookups are disabled");
    }
    (void)useCache;
    (void)ipAddr;
    return std::nullopt;
#endif
}

std::optional<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> TrdpEngine::labelToIds(const std::string &label,
                                                                                              bool useCache) {
    trimCaches();
    if (useCache && config.cacheConfig.enableUriCache) {
        if (const auto cached = fetchCached<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>(labelCache, label)) {
            return cached;
        }
    }

#if defined(TRDP_STACK_PRESENT) && TRDP_HAS_TAU_DNR
    if (!dnrInitialised) {
        logDnrUnavailable("DNR not initialised; label lookups are disabled");
        return std::nullopt;
    }
    if (!pdSessionInitialised && !mdSessionInitialised) {
        return std::nullopt;
    }
    TRDP_APP_SESSION_T session = pdSessionInitialised ? defaultPdSession() : defaultMdSession();
    UINT8 tcnVeh{};
    UINT8 tcnCst{};
    UINT8 opCst{};
    TRDP_ERR_T err = ::tau_label2TcnVehNo(session, &tcnVeh, &tcnCst, label.c_str(), nullptr);
    if (err == TRDP_NO_ERR) {
        err = ::tau_label2OpCstNo(session, &opCst, label.c_str());
    }
    if (err != TRDP_NO_ERR) {
        logConfigError("tau_label2Ids", err);
        return std::nullopt;
    }
    auto resolved = std::make_tuple(static_cast<std::uint32_t>(tcnCst), static_cast<std::uint32_t>(tcnVeh),
                                    static_cast<std::uint32_t>(opCst));
    if (config.cacheConfig.enableUriCache && useCache) {
        setCacheEntry(labelCache[label], resolved);
    }
    return resolved;
#else
    if (!kDnrCompiledIn) {
        logDnrUnavailable("TAU DNR APIs not available in detected stack; label lookups are disabled");
    } else {
        logDnrUnavailable("TRDP stack not present in this build; label lookups are disabled");
    }
    (void)useCache;
    (void)label;
    return std::nullopt;
#endif
}

#ifdef TRDP_STACK_PRESENT
void pdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_PD_INFO_T *pInfo, UINT8 *pData,
                       UINT32 dataSize) {
    if (refCon == nullptr || pInfo == nullptr || pData == nullptr) {
        return;
    }
    if (pInfo->resultCode != TRDP_NO_ERR) {
        std::cerr << "[TRDP] PD receive error for ComId " << pInfo->comId << ": " << pInfo->resultCode << std::endl;
        return;
    }
    auto *engine = static_cast<TrdpEngine *>(refCon);
    std::vector<std::uint8_t> payload(pData, pData + dataSize);
    engine->handleRxTelegram(pInfo->comId, payload);
}

void mdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_MD_INFO_T *pInfo, UINT8 *pData,
                       UINT32 dataSize) {
    if (refCon == nullptr || pInfo == nullptr) {
        return;
    }
    auto *engine = static_cast<TrdpEngine *>(refCon);
    if (pInfo->resultCode != TRDP_NO_ERR) {
        std::cerr << "[TRDP] MD receive error for ComId " << pInfo->comId << ": " << pInfo->resultCode << std::endl;
        return;
    }
    engine->registerMdReply(pInfo);
    const std::uint8_t *payloadPtr = pData;
    const UINT32 payloadSize = dataSize;
    if (payloadPtr != nullptr && payloadSize > 0U) {
        engine->handleRxMdTelegram(pInfo->comId, std::vector<std::uint8_t>(payloadPtr, payloadPtr + payloadSize));
    }
}
#endif

void TrdpEngine::processingLoop() {
    std::cout << "[TRDP] Worker thread started" << std::endl;
    std::unique_lock lock(stateMtx);
    while (!stopRequested.load()) {
#ifdef TRDP_STACK_PRESENT
        const auto pdContext = pdSessionInitialised ? prepareSelectContext(defaultPdSession()) : std::nullopt;
        const auto mdContext = mdSessionInitialised ? prepareSelectContext(defaultMdSession()) : std::nullopt;
#endif
        dispatchCyclicTransmissions(std::chrono::steady_clock::now());
#ifdef TRDP_STACK_PRESENT
        pruneMdTimeouts(std::chrono::steady_clock::now());
#endif
        const auto waitDuration = stackIntervalHint();

        // Release the lock while doing any heavier processing or callbacks.
        lock.unlock();

#ifdef TRDP_STACK_PRESENT
        if (stackAvailable && ((pdContext && pdContext->valid) || (mdContext && mdContext->valid))) {
            auto waitOnContext = [](StackSelectContext &context, const char *label) {
                timeval tv{};
                tv.tv_sec = static_cast<time_t>(context.interval.tv_sec);
                tv.tv_usec = static_cast<suseconds_t>(context.interval.tv_usec);
                const int rv = select(context.maxFd + 1, &context.readFds, &context.writeFds, nullptr, &tv);
                if (rv < 0 && errno != EINTR) {
                    std::cerr << "[TRDP] select(" << label << ") failed: " << errno << std::endl;
                }
            };

            StackSelectContext pdActiveContext{};
            StackSelectContext mdActiveContext{};
            const StackSelectContext *pdPtr = nullptr;
            const StackSelectContext *mdPtr = nullptr;

            if (pdContext && pdContext->valid) {
                pdActiveContext = *pdContext;
                waitOnContext(pdActiveContext, "PD");
                pdPtr = &pdActiveContext;
            }
            if (mdContext && mdContext->valid) {
                mdActiveContext = *mdContext;
                waitOnContext(mdActiveContext, "MD");
                mdPtr = &mdActiveContext;
            }

            processStackOnce(pdPtr, mdPtr);
        } else {
            std::this_thread::sleep_for(waitDuration);
            processStackOnce(nullptr, nullptr);
        }
#else
        std::this_thread::sleep_for(waitDuration);
        processStackOnce(nullptr, nullptr);
#endif

        lock.lock();
    }
    std::cout << "[TRDP] Worker thread exiting" << std::endl;
}

} // namespace trdp
