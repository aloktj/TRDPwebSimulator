#include "plugins/TelegramHub.h"

#include "trdp_engine.h"

#include <algorithm>
#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>

namespace trdp {

namespace {
TelegramHub *g_instance = nullptr;

Json::Value fieldValueToJson(const FieldValue &value) {
    Json::Value json;
    if (std::holds_alternative<std::monostate>(value)) {
        json = Json::Value();
    } else if (std::holds_alternative<bool>(value)) {
        json = std::get<bool>(value);
    } else if (std::holds_alternative<std::int8_t>(value)) {
        json = static_cast<int>(std::get<std::int8_t>(value));
    } else if (std::holds_alternative<std::uint8_t>(value)) {
        json = static_cast<unsigned int>(std::get<std::uint8_t>(value));
    } else if (std::holds_alternative<std::int16_t>(value)) {
        json = static_cast<int>(std::get<std::int16_t>(value));
    } else if (std::holds_alternative<std::uint16_t>(value)) {
        json = static_cast<unsigned int>(std::get<std::uint16_t>(value));
    } else if (std::holds_alternative<std::int32_t>(value)) {
        json = static_cast<Json::Int64>(std::get<std::int32_t>(value));
    } else if (std::holds_alternative<std::uint32_t>(value)) {
        json = static_cast<Json::UInt64>(std::get<std::uint32_t>(value));
    } else if (std::holds_alternative<float>(value)) {
        json = std::get<float>(value);
    } else if (std::holds_alternative<double>(value)) {
        json = std::get<double>(value);
    } else if (std::holds_alternative<std::string>(value)) {
        json = std::get<std::string>(value);
    } else if (std::holds_alternative<std::vector<std::uint8_t>>(value)) {
        const auto &bytes = std::get<std::vector<std::uint8_t>>(value);
        for (const auto b : bytes) {
            json.append(static_cast<unsigned int>(b));
        }
    }
    return json;
}
} // namespace

TelegramHub::TelegramHub() = default;

void TelegramHub::initAndStart(const Json::Value &) {
    g_instance = this;
    TrdpEngine::instance().start();
}

void TelegramHub::shutdown() {
    {
        std::lock_guard lock(connMtx);
        connections.clear();
    }
    TrdpEngine::instance().stop();
    g_instance = nullptr;
}

TelegramHub *TelegramHub::instance() { return g_instance; }

void TelegramHub::subscribe(const drogon::WebSocketConnectionPtr &conn) {
    {
        std::lock_guard lock(connMtx);
        connections.insert(conn);
    }
    sendSnapshot(conn);
}

void TelegramHub::unsubscribe(const drogon::WebSocketConnectionPtr &conn) {
    std::lock_guard lock(connMtx);
    const auto it = std::find_if(connections.begin(), connections.end(), [conn](const auto &ptr) { return ptr == conn; });
    if (it != connections.end()) {
        connections.erase(it);
    }
}

void TelegramHub::publishRxUpdate(std::uint32_t comId, const std::map<std::string, FieldValue> &fields) {
    Json::Value payload;
    payload["type"] = "rx";
    payload["comId"] = comId;
    payload["fields"] = fieldsToJson(fields);
    broadcast(payload);
}

void TelegramHub::publishTxConfirmation(std::uint32_t comId, const std::map<std::string, FieldValue> &fields,
                                       std::optional<bool> txActive) {
    Json::Value payload;
    payload["type"] = "tx";
    payload["comId"] = comId;
    payload["fields"] = fieldsToJson(fields);
    if (txActive.has_value()) {
        payload["txActive"] = *txActive;
    }
    broadcast(payload);
}

void TelegramHub::sendSnapshot(const drogon::WebSocketConnectionPtr &conn) {
    if (!ensureRegistryInitialized()) {
        Json::Value error;
        error["type"] = "error";
        error["message"] = "TRDP registry is not initialised";
        conn->send(error.toStyledString());
        return;
    }

    Json::Value payload;
    payload["type"] = "snapshot";
    auto &items = payload["telegrams"];
    for (const auto &telegram : TelegramRegistry::instance().listTelegrams()) {
        Json::Value tg = telegramToJson(telegram);
        const auto runtime = TelegramRegistry::instance().getOrCreateRuntime(telegram.comId);
        if (runtime) {
            tg["fields"] = fieldsToJson(runtime->snapshotFields());
        }
        items.append(tg);
    }
    conn->send(payload.toStyledString());
}

void TelegramHub::broadcast(const Json::Value &payload) {
    const auto message = payload.toStyledString();
    std::lock_guard lock(connMtx);
    for (auto it = connections.begin(); it != connections.end();) {
        if ((*it)->connected()) {
            (*it)->send(message);
            ++it;
        } else {
            it = connections.erase(it);
        }
    }
}

Json::Value TelegramHub::fieldsToJson(const std::map<std::string, FieldValue> &fields) const {
    Json::Value json(Json::objectValue);
    for (const auto &[name, value] : fields) {
        json[name] = fieldValueToJson(value);
    }
    return json;
}

Json::Value TelegramHub::telegramToJson(const TelegramDef &telegram) const {
    Json::Value json;
    json["comId"] = telegram.comId;
    json["name"] = telegram.name;
    json["dataset"] = telegram.datasetName;
    json["direction"] = telegram.direction == Direction::Tx ? "Tx" : "Rx";
    json["type"] = telegram.type == TelegramType::PD ? "PD" : "MD";
    json["expectedReplies"] = static_cast<Json::UInt64>(telegram.expectedReplies);
    json["replyTimeoutMs"] = static_cast<Json::UInt64>(telegram.replyTimeout.count());
    json["confirmTimeoutMs"] = static_cast<Json::UInt64>(telegram.confirmTimeout.count());
    if (telegram.direction == Direction::Tx && telegram.type == TelegramType::PD) {
        json["txActive"] = TrdpEngine::instance().txPublishActive(telegram.comId).value_or(false);
    }
    return json;
}

} // namespace trdp

