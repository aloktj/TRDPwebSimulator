#include "trdp_engine.h"

#include "plugins/TelegramHub.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <type_traits>
#include <utility>

#ifdef TRDP_STACK_PRESENT
#if __has_include(<tau_dnr.h>)
#include <tau_dnr.h>
#define TRDP_HAS_TAU_DNR 1
#endif
#if __has_include(<tau_ecsp.h>)
#include <tau_ecsp.h>
#define TRDP_HAS_TAU_ECSP 1
#endif
#include <tau_ctrl.h>
#include <trdp/api/trdp_if_light.h>
#endif

namespace trdp {
namespace {

#ifdef TRDP_STACK_PRESENT
void pdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_PD_INFO_T *pInfo, const UINT8 *pData,
                       UINT32 dataSize);
void mdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_MD_INFO_T *pInfo, const UINT8 *pData,
                       UINT32 dataSize);
#endif

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

#ifdef TRDP_HAS_TAU_DNR
template <typename T, typename = void> struct hasHostsFile : std::false_type {};
template <typename T> struct hasHostsFile<T, std::void_t<decltype(std::declval<T>().pHostsFile)>> : std::true_type {};
template <typename T, typename = void> struct hasThreadModel : std::false_type {};
template <typename T> struct hasThreadModel<T, std::void_t<decltype(std::declval<T>().threadModel)>> : std::true_type {};
template <typename T, typename = void> struct hasCacheEntries : std::false_type {};
template <typename T> struct hasCacheEntries<T, std::void_t<decltype(std::declval<T>().maxNoCacheEntries)>>
    : std::true_type {};
template <typename T, typename = void> struct hasCacheTimeout : std::false_type {};
template <typename T> struct hasCacheTimeout<T, std::void_t<decltype(std::declval<T>().cacheTimeout)>> : std::true_type {};
template <typename T, typename = void> struct hasEnableCache : std::false_type {};
template <typename T> struct hasEnableCache<T, std::void_t<decltype(std::declval<T>().enableCache)>> : std::true_type {};

template <typename Config> void setDnrHostsFile(Config &cfg, const char *hostsFile) {
    if constexpr (hasHostsFile<Config>::value) {
        cfg.pHostsFile = hostsFile;
    }
}

template <typename Config> void setDnrThreadModel(Config &cfg, TrdpEngine::DnrMode mode) {
    if constexpr (hasThreadModel<Config>::value) {
        cfg.threadModel = (mode == TrdpEngine::DnrMode::DedicatedThread) ? TAU_DNR_THREAD_DEDICATED : TAU_DNR_THREAD_COMMON;
    }
}

template <typename Config> void setDnrCacheEntries(Config &cfg, std::size_t entries) {
    if constexpr (hasCacheEntries<Config>::value) {
        cfg.maxNoCacheEntries = static_cast<UINT32>(entries);
    }
}

template <typename Config> void setDnrCacheTimeout(Config &cfg, std::chrono::milliseconds timeout) {
    if constexpr (hasCacheTimeout<Config>::value) {
        cfg.cacheTimeout = static_cast<UINT32>(timeout.count());
    }
}

template <typename Config> void setDnrCacheEnabled(Config &cfg, bool enable) {
    if constexpr (hasEnableCache<Config>::value) {
        cfg.enableCache = enable ? TRUE : FALSE;
    }
}
#endif

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

    if (config.enableDnr && !initialiseDnr()) {
        tlc_terminate();
        return false;
    }

    if (config.ecspConfig.enable) {
        initialiseEcsp();
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
        dnrInitialised = false;
        ecspInitialised = false;
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
        const auto toDuration = [](const TRDP_TIME_T &time) {
            return std::chrono::milliseconds(time.tv_usec / 1000 + time.tv_sec * 1000);
        };
        const auto resolveInterval = [&](TRDP_APP_SESSION_T session) -> std::optional<std::chrono::milliseconds> {
            if (!session) {
                return std::nullopt;
            }

            TRDP_ERR_T err = tlc_getInterval(session, &interval, nullptr);
            if (err != TRDP_NO_ERR) {
                err = tlp_getInterval(session, &interval);
            }

            if (err == TRDP_NO_ERR) {
                return toDuration(interval);
            }

            std::cerr << "[TRDP] Failed to obtain stack interval: " << err << std::endl;
            return std::nullopt;
        };

        if (pdSessionInitialised) {
            if (const auto pdInterval = resolveInterval(pdSession)) {
                return *pdInterval;
            }
        }
        if (mdSessionInitialised) {
            if (const auto mdInterval = resolveInterval(mdSession)) {
                return *mdInterval;
            }
        }
#endif
    }
    if (config.idleInterval.count() > 0) {
        return config.idleInterval;
    }
    return std::chrono::milliseconds(100);
}

void TrdpEngine::logConfigError(const std::string &context, TRDP_ERR_T err) const {
    std::cerr << "[TRDP] " << context << " failed: " << err;
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

bool TrdpEngine::initialiseDnr() {
#ifdef TRDP_STACK_PRESENT
    const char *hostsFile = config.hostsFile.empty() ? nullptr : config.hostsFile.c_str();

#ifdef TRDP_HAS_TAU_DNR
    TRDP_DNR_CONFIG_T dnrConfig{};
    setDnrHostsFile(dnrConfig, hostsFile);
    setDnrThreadModel(dnrConfig, config.dnrMode);
    setDnrCacheEntries(dnrConfig, config.cacheConfig.uriCacheEntries);
    setDnrCacheTimeout(dnrConfig, config.cacheConfig.uriCacheTtl);
    setDnrCacheEnabled(dnrConfig, config.cacheConfig.enableUriCache);
    const TRDP_ERR_T dnrErr = tau_initDnr(&dnrConfig);
#else
    const TRDP_ERR_T dnrErr = tau_initDnr(nullptr);
#endif
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

bool TrdpEngine::processStackOnce() {
    if (!running.load()) {
        return false;
    }

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
                setTopologyCounters(pdSession);
            }
            if (mdSessionInitialised) {
                setTopologyCounters(mdSession);
            }
            topologyCountersDirty = false;
        }

        TRDP_TIME_T rcvTime{};
        if (pdSessionInitialised) {
            const TRDP_ERR_T pdErr = tlc_process(pdSession, &rcvTime, nullptr);
            if (pdErr != TRDP_NO_ERR) {
                std::cerr << "[TRDP] tlc_process (PD) failed: " << pdErr << std::endl;
            }
        }
        if (mdSessionInitialised) {
            const TRDP_ERR_T mdErr = tlc_process(mdSession, &rcvTime, nullptr);
            if (mdErr != TRDP_NO_ERR) {
                std::cerr << "[TRDP] tlc_process (MD) failed: " << mdErr << std::endl;
            }
        }
        if (config.ecspConfig.enable) {
            pollEcspStatus();
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
#ifdef TRDP_STACK_PRESENT
            if (handle.mdHandleReady && stackAvailable) {
                TRDP_SEND_PARAM_T sendParam{};
                TRDP_UUID_T uuid{};
                TRDP_IP_ADDR_T anyAddr = 0U;
                TRDP_ERR_T mdErr = tlm_addListener(mdSession, &handle.mdListenerHandle, &uuid, &uuid, anyAddr,
                                                   telegram.comId, etbTopoCounter, opTrainTopoCounter, anyAddr,
                                                   mdReceiveCallback, this, 0U, anyAddr, &sendParam);
                handle.mdHandleReady = (mdErr == TRDP_NO_ERR);
                if (mdErr != TRDP_NO_ERR) {
                    std::cerr << "[TRDP] tlm_addListener failed for ComId " << telegram.comId << ": " << mdErr
                              << std::endl;
                }
            }
#endif
            if (handle.mdHandleReady) {
                std::cout << "[TRDP] Binding MD endpoint for ComId " << telegram.comId << std::endl;
            } else {
                std::cerr << "[TRDP] MD session not initialised; unable to bind ComId " << telegram.comId
                          << std::endl;
            }
        } else {
            handle.pdHandleReady = pdSessionInitialised;
#ifdef TRDP_STACK_PRESENT
            if (handle.pdHandleReady && stackAvailable) {
                TRDP_SEND_PARAM_T sendParam{};
                TRDP_UUID_T uuid{};
                TRDP_IP_ADDR_T anyAddr = 0U;
                const auto buffer = runtime->getBufferCopy();
                TRDP_ERR_T pdErr{};
                if (telegram.direction == Direction::Tx) {
                    pdErr = tlp_publish(pdSession, &handle.pdPublishHandle, &uuid, &uuid, anyAddr, anyAddr,
                                        telegram.comId, etbTopoCounter, opTrainTopoCounter, 0U, 0U,
                                        static_cast<UINT32>(buffer.size()), buffer.data(), FALSE, FALSE, &sendParam);
                } else {
                    pdErr = tlp_subscribe(pdSession, &handle.pdSubscribeHandle, &uuid, anyAddr, telegram.comId,
                                           anyAddr, etbTopoCounter, opTrainTopoCounter, 0U, pdReceiveCallback, this,
                                           0U, anyAddr, &sendParam);
                }
                handle.pdHandleReady = (pdErr == TRDP_NO_ERR);
                if (pdErr != TRDP_NO_ERR) {
                    std::cerr << "[TRDP] PD binding failed for ComId " << telegram.comId << ": " << pdErr
                              << std::endl;
                }
            }
#endif
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
#ifdef TRDP_STACK_PRESENT
        if (stackAvailable) {
            TRDP_SEND_PARAM_T sendParam{};
            TRDP_UUID_T uuid{};
            TRDP_IP_ADDR_T anyAddr = 0U;
            TRDP_ERR_T err = tlm_request(mdSession, &endpoint->mdRequestHandle, &uuid, &uuid, anyAddr, anyAddr,
                                         comId, etbTopoCounter, opTrainTopoCounter, 0U, 0U, buffer.data(),
                                         static_cast<UINT32>(buffer.size()), mdReceiveCallback, this, &sendParam);
            if (err != TRDP_NO_ERR) {
                std::cerr << "[TRDP] tlm_request failed for ComId " << comId << ": " << err << std::endl;
                return false;
            }
        }
#endif
        std::cout << "[TRDP] MD send ComId=" << comId << " bytes=" << buffer.size() << std::endl;
    } else {
        if (!endpoint->pdHandleReady) {
            std::cerr << "[TRDP] PD session not available; drop TX ComId " << comId << std::endl;
            return false;
        }
#ifdef TRDP_STACK_PRESENT
        if (stackAvailable) {
            TRDP_ERR_T err = tlp_put(pdSession, endpoint->pdPublishHandle, buffer.data(),
                                     static_cast<UINT32>(buffer.size()));
            if (err != TRDP_NO_ERR) {
                std::cerr << "[TRDP] tlp_put failed for ComId " << comId << ": " << err << std::endl;
                return false;
            }
        }
#endif
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

std::optional<std::uint32_t> TrdpEngine::uriToIp(const std::string &uri, bool useCache) {
    trimCaches();
    if (useCache && config.cacheConfig.enableUriCache) {
        if (const auto cached = fetchCached<std::uint32_t>(uriCache, uri)) {
            return cached;
        }
    }

#if defined(TRDP_STACK_PRESENT) && defined(TRDP_HAS_TAU_DNR)
    TRDP_IP_ADDR_T addr{};
    const TRDP_ERR_T err = tau_uri2Addr(uri.c_str(), &addr, useCache ? TRUE : FALSE);
    if (err != TRDP_NO_ERR) {
        logConfigError("tau_uri2Addr", err);
        return std::nullopt;
    }
    if (config.cacheConfig.enableUriCache && useCache) {
        setCacheEntry(uriCache[uri], static_cast<std::uint32_t>(addr));
    }
    return static_cast<std::uint32_t>(addr);
#else
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

#if defined(TRDP_STACK_PRESENT) && defined(TRDP_HAS_TAU_DNR)
    std::array<char, 256> uriBuffer{};
    const TRDP_ERR_T err = tau_addr2Uri(static_cast<TRDP_IP_ADDR_T>(ipAddr), uriBuffer.data(), uriBuffer.size(),
                                        useCache ? TRUE : FALSE);
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

#if defined(TRDP_STACK_PRESENT) && defined(TRDP_HAS_TAU_DNR)
    TRDP_LABELS_T ids{};
    const TRDP_ERR_T err = tau_label2Ids(label.c_str(), &ids, nullptr, nullptr);
    if (err != TRDP_NO_ERR) {
        logConfigError("tau_label2Ids", err);
        return std::nullopt;
    }
    auto resolved = std::make_tuple(static_cast<std::uint32_t>(ids.cst), static_cast<std::uint32_t>(ids.veh),
                                    static_cast<std::uint32_t>(ids.func));
    if (config.cacheConfig.enableUriCache && useCache) {
        setCacheEntry(labelCache[label], resolved);
    }
    return resolved;
#else
    (void)useCache;
    (void)label;
    return std::nullopt;
#endif
}

#ifdef TRDP_STACK_PRESENT
void pdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_PD_INFO_T *pInfo, const UINT8 *pData,
                       UINT32 dataSize) {
    if (refCon == nullptr || pInfo == nullptr || pData == nullptr) {
        return;
    }
    if (pInfo->result != TRDP_NO_ERR) {
        std::cerr << "[TRDP] PD receive error for ComId " << pInfo->comId << ": " << pInfo->result << std::endl;
        return;
    }
    auto *engine = static_cast<TrdpEngine *>(refCon);
    std::vector<std::uint8_t> payload(pData, pData + dataSize);
    engine->handleRxTelegram(pInfo->comId, payload);
}

void mdReceiveCallback(void *refCon, TRDP_APP_SESSION_T /*session*/, const TRDP_MD_INFO_T *pInfo, const UINT8 *pData,
                       UINT32 dataSize) {
    if (refCon == nullptr || pInfo == nullptr) {
        return;
    }
    auto *engine = static_cast<TrdpEngine *>(refCon);
    if (pInfo->result != TRDP_NO_ERR) {
        std::cerr << "[TRDP] MD receive error for ComId " << pInfo->comId << ": " << pInfo->result << std::endl;
        return;
    }
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

