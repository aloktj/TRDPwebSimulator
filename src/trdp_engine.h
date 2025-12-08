#pragma once

#include "telegram_model.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#ifdef TRDP_STACK_PRESENT
#include <trdp/api/trdp_if_light.h>
#else
using TRDP_ERR_T = int;
#endif

namespace trdp {

/**
 * Minimal TRDP engine wrapper.
 *
 * Responsible for:
 *  - Initialising the TRDP stack (stubbed for now with logging hooks).
 *  - Creating PD/MD publishers and subscribers based on the TelegramRegistry.
 *  - Mapping RX buffers into TelegramRuntime field values.
 *  - Encoding TX field values into buffers before sending.
 *  - Running a background processing loop to keep the stack alive.
 */
class TrdpEngine {
  public:
    static TrdpEngine &instance();

    enum class DnrMode { CommonThread, DedicatedThread };

    struct CacheConfig {
        bool enableUriCache{true};
        std::chrono::milliseconds uriCacheTtl{std::chrono::seconds(30)};
        std::size_t uriCacheEntries{128};
    };

    struct EcspConfig {
        bool enable{false};
        std::chrono::milliseconds pollInterval{std::chrono::seconds(1)};
        std::chrono::milliseconds confirmTimeout{std::chrono::seconds(5)};
    };

    struct TrdpConfig {
        std::string rxInterface;
        std::string txInterface;
        std::string hostsFile;
        bool enableDnr{false};
        DnrMode dnrMode{DnrMode::CommonThread};
        CacheConfig cacheConfig;
        EcspConfig ecspConfig;
        // How often the worker thread should wake up when no events are pending.
        std::chrono::milliseconds idleInterval{std::chrono::milliseconds(50)};
    };

    // Start TRDP stack and background worker. Idempotent.
    bool start();
    bool start(const TrdpConfig &config);

    // Stop worker thread and tear down handles. Safe to call multiple times.
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return running.load(); }

    // Push updated TX field values to the network. Returns false on failure.
    bool sendTxTelegram(std::uint32_t comId, const std::map<std::string, FieldValue> &txFields);

    // Stop cyclic publishing for a TX PD telegram.
    bool stopTxTelegram(std::uint32_t comId);

    // Feed a freshly received PD telegram into the registry/runtime.
    void handleRxTelegram(std::uint32_t comId, const std::vector<std::uint8_t> &payload);

    // Feed a freshly received MD telegram into the registry/runtime.
    void handleRxMdTelegram(std::uint32_t comId, const std::vector<std::uint8_t> &payload);

    // TAU helper wrappers (no-ops when the TRDP stack is not present)
    std::optional<std::uint32_t> uriToIp(const std::string &uri, bool useCache = true);
    std::optional<std::string> ipToUri(std::uint32_t ipAddr, bool useCache = true);
    std::optional<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> labelToIds(const std::string &label,
                                                                                      bool useCache = true);

  private:
    TrdpEngine() = default;
    TrdpEngine(const TrdpEngine &) = delete;
    TrdpEngine &operator=(const TrdpEngine &) = delete;

    struct EndpointHandle {
        TelegramDef def;
        std::shared_ptr<TelegramRuntime> runtime;
        bool pdHandleReady{false};
        bool mdHandleReady{false};
        std::chrono::milliseconds cycle{0};
        bool txCyclicActive{false};
        std::chrono::steady_clock::time_point nextSend{};
#ifdef TRDP_STACK_PRESENT
        TRDP_PUB_T pdPublishHandle{};
        TRDP_SUB_T pdSubscribeHandle{};
        TRDP_LIS_T mdListenerHandle{};
        TRDP_UUID_T mdSessionId{};
        TRDP_APP_SESSION_T pdSessionHandle{};
        TRDP_APP_SESSION_T mdSessionHandle{};
#endif
    };

    bool bootstrapRegistry();
    bool initialiseTrdpStack();
    void teardownTrdpStack();
    std::chrono::milliseconds stackIntervalHint() const;
#ifdef TRDP_STACK_PRESENT
    struct StackSelectContext {
        TRDP_FDS_T readFds{};
        TRDP_FDS_T writeFds{};
        INT32 maxFd{-1};
        TRDP_TIME_T interval{};
        bool valid{false};
    };
    std::optional<StackSelectContext> prepareSelectContext(TRDP_APP_SESSION_T session) const;
#endif
    void markTopologyChanged();
    bool publishPdBuffer(EndpointHandle &endpoint, const std::vector<std::uint8_t> &buffer);
    bool processStackOnce(
#ifdef TRDP_STACK_PRESENT
        const StackSelectContext *pdContext, const StackSelectContext *mdContext
#else
        void *pdContext, void *mdContext
#endif
    );
    void dispatchCyclicTransmissions(std::chrono::steady_clock::time_point now);
    void buildEndpoints();
    void processingLoop();
    bool initialiseDnr();
    void initialiseEcsp();
    void updateEcspControl();

    EndpointHandle *findEndpoint(std::uint32_t comId);

    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    bool pdSessionInitialised{false};
    bool mdSessionInitialised{false};
    bool stackAvailable{false};
    std::uint32_t resolvedSessionIp{0};
#ifdef TRDP_STACK_PRESENT
    std::map<std::uint16_t, TRDP_APP_SESSION_T> pdSessions;
    std::map<std::uint16_t, TRDP_APP_SESSION_T> mdSessions;
    std::vector<UINT8> heapStorage;
    bool dnrInitialised{false};
    bool ecspInitialised{false};
    TRDP_APP_SESSION_T defaultPdSession() const;
    TRDP_APP_SESSION_T defaultMdSession() const;
    TRDP_APP_SESSION_T pdSessionForPort(std::uint16_t port) const;
    TRDP_APP_SESSION_T mdSessionForPort(std::uint16_t port) const;
    std::uint16_t resolvePortForEndpoint(const TelegramDef &telegram) const;
#endif
    std::uint32_t etbTopoCounter{0};
    std::uint32_t opTrainTopoCounter{0};
    bool topologyCountersDirty{false};
    TrdpConfig config;
    std::thread worker;
    std::mutex stateMtx;
    std::condition_variable cv;
    std::map<std::uint32_t, EndpointHandle> endpoints;

#ifdef TRDP_STACK_PRESENT
    using MdSessionKey = std::array<std::uint8_t, sizeof(TRDP_UUID_T)>;

    struct MdRequestState {
        std::uint32_t comId{0};
        std::uint32_t expectedReplies{0};
        std::uint32_t receivedReplies{0};
        std::chrono::steady_clock::time_point sentAt{};
        std::chrono::steady_clock::time_point replyDeadline{};
        std::chrono::steady_clock::time_point confirmDeadline{};
        bool confirmObserved{false};
    };

    MdSessionKey mdSessionKeyFromId(const TRDP_UUID_T &sessionId) const;
    static std::string formatMdSessionKey(const MdSessionKey &key);
    void trackMdRequest(const MdSessionKey &sessionKey, const EndpointHandle &endpoint);
    void registerMdReply(const TRDP_MD_INFO_T *pInfo);
    void pruneMdTimeouts(std::chrono::steady_clock::time_point now);

    std::mutex mdRequestMtx;
    std::map<MdSessionKey, MdRequestState> mdRequestStates;
#endif

#ifdef TRDP_STACK_PRESENT
    friend void mdReceiveCallback(void *refCon, TRDP_APP_SESSION_T session, const TRDP_MD_INFO_T *pInfo, UINT8 *pData,
                                  UINT32 dataSize);
#endif

    struct CacheEntry {
        using Payload = std::variant<std::uint32_t, std::string, std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>;

        std::chrono::steady_clock::time_point expiresAt;
        Payload payload;
    };

    std::map<std::string, CacheEntry> uriCache;
    std::map<std::uint32_t, CacheEntry> ipCache;
    std::map<std::string, CacheEntry> labelCache;

    void trimCaches();
    void updateCacheLimits();
    void setCacheEntry(CacheEntry &entry, CacheEntry::Payload value);
    template <typename T, typename Map, typename Key>
    std::optional<T> fetchCached(Map &map, const Key &key) const;
    void logConfigError(const std::string &context, TRDP_ERR_T err) const;
    void pollEcspStatus();
};

template <typename T, typename Map, typename Key>
std::optional<T> TrdpEngine::fetchCached(Map &map, const Key &key) const {
    auto it = map.find(key);
    if (it == map.end()) {
        return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= it->second.expiresAt) {
        return std::nullopt;
    }
    if (const auto *value = std::get_if<T>(&it->second.payload)) {
        return *value;
    }
    return std::nullopt;
}

} // namespace trdp

