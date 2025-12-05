#include "trdp_engine.h"

#include "plugins/TelegramHub.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

namespace trdp {
namespace {

/**
 * Lightweight façade over the TRDP/TAU stack calls we described during analysis.
 *
 * The implementation remains a stub so the application can build without the
 * native stack present, but the methods mirror the intended flow:
 *  - initialise() represents tlc_init/tau_init and session bring-up
 *  - bindPd/bindMd mirror tlp_publish/tlp_subscribe/tlm_addListener
 *  - getProcessInterval/processTick mirror tlc_getInterval/tlc_process
 */
class TrdpStackFacade {
  public:
    bool initialise()
    {
        if (initialised) {
            return true;
        }
        std::cout << "[TRDP] tlc_init/tau_init stubbed initialisation" << std::endl;
        initialised = true;
        lastProcess = std::chrono::steady_clock::now();
        return true;
    }

    void shutdown()
    {
        if (!initialised) {
            return;
        }
        std::cout << "[TRDP] tlc_terminate/tau_terminate stubbed shutdown" << std::endl;
        initialised = false;
    }

    bool bindPdEndpoint(const TelegramDef &telegram)
    {
        if (!initialised) {
            return false;
        }
        std::cout << "[TRDP] tlp_publish/tlp_subscribe stub for ComId " << telegram.comId << std::endl;
        return true;
    }

    bool bindMdEndpoint(const TelegramDef &telegram)
    {
        if (!initialised) {
            return false;
        }
        std::cout << "[TRDP] tlm_addListener/tlm_request stub for ComId " << telegram.comId << std::endl;
        return true;
    }

    bool sendTelegram(const TelegramDef &telegram, const std::vector<std::uint8_t> &buffer)
    {
        if (!initialised) {
            return false;
        }
        const char *type = telegram.type == TelegramType::MD ? "MD" : "PD";
        std::cout << "[TRDP] " << type << " send stub ComId=" << telegram.comId << " bytes=" << buffer.size()
                  << std::endl;
        return true;
    }

    std::chrono::milliseconds getProcessInterval()
    {
        // emulate tlc_getInterval/tlp_getInterval returning the next wait value
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProcess);
        if (elapsed >= defaultInterval) {
            return std::chrono::milliseconds(0);
        }
        return defaultInterval - elapsed;
    }

    void processTick()
    {
        if (!initialised) {
            return;
        }
        std::cout << "[TRDP] tlc_process/tlp_processReceive stub tick" << std::endl;
        lastProcess = std::chrono::steady_clock::now();
    }

  private:
    bool initialised{false};
    std::chrono::steady_clock::time_point lastProcess{};
    const std::chrono::milliseconds defaultInterval{50};
};

TrdpStackFacade stackFacade;

template <typename T> T readLe(const std::uint8_t *data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

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

std::vector<std::uint8_t> encodeFieldsToBuffer(const TelegramRuntime &runtime,
                                               const std::map<std::string, FieldValue> &fields) {
    return encodeFields(runtime.dataset(), fields);
}

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

} // namespace

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
    // Use the façade to mimic the lifecycle of tlc_init()/tlc_openSession().
    if (!stackFacade.initialise()) {
        std::cerr << "[TRDP] Stack initialisation failed" << std::endl;
        return false;
    }

    pdSessionInitialised = true;
    mdSessionInitialised = true;
    std::cout << "[TRDP] PD session handle ready" << std::endl;
    std::cout << "[TRDP] MD session handle ready" << std::endl;
    return true;
}

void TrdpEngine::buildEndpoints() {
    endpoints.clear();

    for (const auto &telegram : TelegramRegistry::instance().listTelegrams()) {
        auto runtime = TelegramRegistry::instance().getOrCreateRuntime(telegram.comId);
        if (!runtime) {
            std::cerr << "[TRDP] Failed to allocate runtime for ComId " << telegram.comId << std::endl;
            continue;
        }
        EndpointHandle handle{.def = telegram, .runtime = runtime};

        if (telegram.type == TelegramType::MD) {
            handle.mdHandleReady = mdSessionInitialised && stackFacade.bindMdEndpoint(telegram);
            if (handle.mdHandleReady) {
                std::cout << "[TRDP] Binding MD endpoint for ComId " << telegram.comId << std::endl;
            } else {
                std::cerr << "[TRDP] MD session not initialised; unable to bind ComId " << telegram.comId
                          << std::endl;
            }
        } else {
            handle.pdHandleReady = pdSessionInitialised && stackFacade.bindPdEndpoint(telegram);
            if (handle.pdHandleReady) {
                std::cout << "[TRDP] Binding PD endpoint for ComId " << telegram.comId << std::endl;
            } else {
                std::cerr << "[TRDP] PD session not initialised; unable to bind ComId " << telegram.comId
                          << std::endl;
            }
        }
        endpoints.emplace(telegram.comId, std::move(handle));
    }
}

bool TrdpEngine::start(const TrdpConfig &cfg) {
    std::lock_guard lock(stateMtx);
    if (running.load()) {
        return true;
    }

    config = cfg;
#ifdef TRDP_STACK_PRESENT
    stackAvailable = true;
#else
    stackAvailable = false;
#endif

    if (!bootstrapRegistry()) {
        return false;
    }
    if (!initialiseTrdpStack()) {
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
    pdSessionInitialised = false;
    mdSessionInitialised = false;
    endpoints.clear();
    stackFacade.shutdown();
    std::cout << "[TRDP] MD session handle released" << std::endl;
    std::cout << "[TRDP] PD session handle released" << std::endl;
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

    // TODO: Replace with actual tlp_put()/tlm_put() calls once TRDP bindings are available.
    if (endpoint->def.type == TelegramType::MD) {
        if (!endpoint->mdHandleReady) {
            std::cerr << "[TRDP] MD session not available; drop TX ComId " << comId << std::endl;
            return false;
        }
        if (!stackFacade.sendTelegram(endpoint->def, buffer)) {
            std::cerr << "[TRDP] MD send failed for ComId " << comId << std::endl;
            return false;
        }
    } else {
        if (!endpoint->pdHandleReady) {
            std::cerr << "[TRDP] PD session not available; drop TX ComId " << comId << std::endl;
            return false;
        }
        if (!stackFacade.sendTelegram(endpoint->def, buffer)) {
            std::cerr << "[TRDP] PD send failed for ComId " << comId << std::endl;
            return false;
        }
    }

    if (auto *hub = TelegramHub::instance()) {
        hub->publishTxConfirmation(comId, mergedFields);
    }
    return true;
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

void TrdpEngine::processingLoop() {
    std::cout << "[TRDP] Worker thread started" << std::endl;
    std::unique_lock lock(stateMtx);
    while (!stopRequested.load()) {
        const auto waitFor = stackFacade.getProcessInterval();
        cv.wait_for(lock, waitFor, [this]() { return stopRequested.load(); });
        lock.unlock();
        stackFacade.processTick();
        lock.lock();
    }
    std::cout << "[TRDP] Worker thread exiting" << std::endl;
}

} // namespace trdp

