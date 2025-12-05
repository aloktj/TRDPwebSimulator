#pragma once

#include "telegram_model.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef TRDP_STACK_PRESENT
#include <trdp/api/trdp_if_light.h>
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

    struct TrdpConfig {
        std::string rxInterface;
        std::string txInterface;
        std::string hostsFile;
        bool enableDnr{false};
        bool enableEcsp{false};
        // How often the worker thread should wake up when no events are pending.
        std::chrono::milliseconds idleInterval{std::chrono::milliseconds(50)};
    };

    // Start TRDP stack and background worker. Idempotent.
    bool start(const TrdpConfig &config = {});

    // Stop worker thread and tear down handles. Safe to call multiple times.
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return running.load(); }

    // Push updated TX field values to the network. Returns false on failure.
    bool sendTxTelegram(std::uint32_t comId, const std::map<std::string, FieldValue> &txFields);

    // Feed a freshly received PD telegram into the registry/runtime.
    void handleRxTelegram(std::uint32_t comId, const std::vector<std::uint8_t> &payload);

    // Feed a freshly received MD telegram into the registry/runtime.
    void handleRxMdTelegram(std::uint32_t comId, const std::vector<std::uint8_t> &payload);

  private:
    TrdpEngine() = default;
    TrdpEngine(const TrdpEngine &) = delete;
    TrdpEngine &operator=(const TrdpEngine &) = delete;

    struct EndpointHandle {
        TelegramDef def;
        std::shared_ptr<TelegramRuntime> runtime;
        bool pdHandleReady{false};
        bool mdHandleReady{false};
#ifdef TRDP_STACK_PRESENT
        TRDP_PUB_T pdPublishHandle{};
        TRDP_SUB_T pdSubscribeHandle{};
        TRDP_LIS_T mdListenerHandle{};
        TRDP_REQUEST_T mdRequestHandle{};
#endif
    };

    bool bootstrapRegistry();
    bool initialiseTrdpStack();
    void teardownTrdpStack();
    std::chrono::milliseconds stackIntervalHint() const;
    void markTopologyChanged();
    bool processStackOnce();
    void buildEndpoints();
    void processingLoop();

    EndpointHandle *findEndpoint(std::uint32_t comId);

    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    bool pdSessionInitialised{false};
    bool mdSessionInitialised{false};
    bool stackAvailable{false};
#ifdef TRDP_STACK_PRESENT
    TRDP_APP_SESSION_T pdSession{};
    TRDP_APP_SESSION_T mdSession{};
    bool tauInitialised{false};
    std::vector<UINT8> heapStorage;
#endif
    std::uint32_t etbTopoCounter{0};
    std::uint32_t opTrainTopoCounter{0};
    bool topologyCountersDirty{false};
    TrdpConfig config;
    std::thread worker;
    std::mutex stateMtx;
    std::condition_variable cv;
    std::map<std::uint32_t, EndpointHandle> endpoints;
};

} // namespace trdp

