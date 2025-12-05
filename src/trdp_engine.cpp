#include "trdp_engine.h"

#include "plugins/TelegramHub.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#ifdef TRDP_STACK_PRESENT
#include <tau_ctrl.h>
#include <trdp/api/trdp_if_light.h>
#endif

namespace trdp {
namespace {

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

    const TRDP_ERR_T tauErr = tau_init();
    if (tauErr != TRDP_NO_ERR) {
        std::cerr << "[TRDP] tau_init failed: " << tauErr << std::endl;
    } else {
        tauInitialised = true;
    }

    const TRDP_ERR_T pdErr = tlc_openSession(&pdSession, nullptr, nullptr, nullptr, nullptr);
    if (pdErr != TRDP_NO_ERR) {
        std::cerr << "[TRDP] tlc_openSession for PD failed: " << pdErr << std::endl;
        tlc_terminate();
        if (tauInitialised) {
            tau_terminate();
            tauInitialised = false;
        }
        return false;
    }
    pdSessionInitialised = true;

    const TRDP_ERR_T mdErr = tlc_openSession(&mdSession, nullptr, nullptr, nullptr, nullptr);
    if (mdErr != TRDP_NO_ERR) {
        std::cerr << "[TRDP] tlc_openSession for MD failed: " << mdErr << std::endl;
        tlc_closeSession(pdSession);
        pdSessionInitialised = false;
        tlc_terminate();
        if (tauInitialised) {
            tau_terminate();
            tauInitialised = false;
        }
        return false;
    }
    mdSessionInitialised = true;

    // Apply any updated session defaults or interface selections.
    (void)tlc_configSession(pdSession, nullptr, nullptr, nullptr);
    (void)tlc_configSession(mdSession, nullptr, nullptr, nullptr);
    (void)tlc_updateSession(pdSession, &pdSession, 0U);
    (void)tlc_updateSession(mdSession, &mdSession, 0U);
#else
    pdSessionInitialised = true;
    mdSessionInitialised = true;
#endif

    std::cout << "[TRDP] PD session handle ready" << std::endl;
    std::cout << "[TRDP] MD session handle ready" << std::endl;
    return true;
}

void TrdpEngine::teardownTrdpStack() {
    if (!pdSessionInitialised && !mdSessionInitialised) {
        return;
    }

    if (stackAvailable) {
#ifdef TRDP_STACK_PRESENT
        if (mdSessionInitialised) {
            tlc_configSession(mdSession, nullptr, nullptr, nullptr);
            (void)tlc_closeSession(mdSession);
        }
        if (pdSessionInitialised) {
            tlc_configSession(pdSession, nullptr, nullptr, nullptr);
            (void)tlc_closeSession(pdSession);
        }
        tlc_terminate();
        if (tauInitialised) {
            tau_terminate();
            tauInitialised = false;
        }
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
        TRDP_TIME_T interval{};
        if (pdSessionInitialised && tlc_getInterval(pdSession, &interval, nullptr) == TRDP_NO_ERR) {
            return std::chrono::milliseconds(interval.tv_usec / 1000 + interval.tv_sec * 1000);
        }
#endif
    }
    if (config.idleInterval.count() > 0) {
        return config.idleInterval;
    }
    return std::chrono::milliseconds(100);
}

bool TrdpEngine::processStackOnce() {
    if (!running.load()) {
        return false;
    }

    if (stackAvailable) {
#ifdef TRDP_STACK_PRESENT
        // Topology counters should be updated before processing when they change.
        (void)etbTopoCounter;
        (void)opTrainTopoCounter;
        TRDP_TIME_T rcvTime{};
        if (pdSessionInitialised) {
            tlc_process(pdSession, &rcvTime, nullptr);
        }
        if (mdSessionInitialised) {
            tlc_process(mdSession, &rcvTime, nullptr);
        }
#endif
    }

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
            handle.mdHandleReady = mdSessionInitialised;
            if (handle.mdHandleReady) {
                std::cout << "[TRDP] Binding MD endpoint for ComId " << telegram.comId << std::endl;
            } else {
                std::cerr << "[TRDP] MD session not initialised; unable to bind ComId " << telegram.comId
                          << std::endl;
            }
        } else {
            handle.pdHandleReady = pdSessionInitialised;
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

    if (endpoint->def.type == TelegramType::MD) {
        if (!endpoint->mdHandleReady) {
            std::cerr << "[TRDP] MD session not available; drop TX ComId " << comId << std::endl;
            return false;
        }
        std::cout << "[TRDP] MD send ComId=" << comId << " bytes=" << buffer.size() << std::endl;
    } else {
        if (!endpoint->pdHandleReady) {
            std::cerr << "[TRDP] PD session not available; drop TX ComId " << comId << std::endl;
            return false;
        }
        std::cout << "[TRDP] PD send ComId=" << comId << " bytes=" << buffer.size() << std::endl;
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
        const auto waitDuration = stackIntervalHint();
        cv.wait_for(lock, waitDuration, [this]() { return stopRequested.load(); });
        if (stopRequested.load()) {
            break;
        }

        // Release the lock while doing any heavier processing or callbacks.
        lock.unlock();
        processStackOnce();
        lock.lock();
    }
    std::cout << "[TRDP] Worker thread exiting" << std::endl;
}

} // namespace trdp

