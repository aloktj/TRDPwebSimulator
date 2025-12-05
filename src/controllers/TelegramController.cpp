#include "controllers/TelegramController.h"

#include "plugins/TelegramHub.h"
#include "telegram_model.h"
#include "trdp_engine.h"

#include <drogon/drogon.h>

namespace trdp {

namespace {
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

std::optional<FieldValue> jsonToFieldValue(const FieldDef &field, const Json::Value &value) {
    try {
        switch (field.type) {
        case FieldType::BOOL:
            if (value.isBool()) {
                return FieldValue{value.asBool()};
            }
            break;
        case FieldType::INT8:
            if (value.isInt()) {
                return FieldValue{static_cast<std::int8_t>(value.asInt())};
            }
            break;
        case FieldType::UINT8:
            if (value.isUInt()) {
                return FieldValue{static_cast<std::uint8_t>(value.asUInt())};
            }
            break;
        case FieldType::INT16:
            if (value.isInt()) {
                return FieldValue{static_cast<std::int16_t>(value.asInt())};
            }
            break;
        case FieldType::UINT16:
            if (value.isUInt()) {
                return FieldValue{static_cast<std::uint16_t>(value.asUInt())};
            }
            break;
        case FieldType::INT32:
            if (value.isInt()) {
                return FieldValue{static_cast<std::int32_t>(value.asInt())};
            }
            break;
        case FieldType::UINT32:
            if (value.isUInt() || value.isUInt64()) {
                return FieldValue{static_cast<std::uint32_t>(value.asUInt())};
            }
            break;
        case FieldType::FLOAT:
            if (value.isDouble() || value.isNumeric()) {
                return FieldValue{static_cast<float>(value.asDouble())};
            }
            break;
        case FieldType::DOUBLE:
            if (value.isDouble() || value.isNumeric()) {
                return FieldValue{value.asDouble()};
            }
            break;
        case FieldType::STRING:
            if (value.isString()) {
                return FieldValue{value.asString()};
            }
            break;
        case FieldType::BYTES:
            if (value.isArray()) {
                std::vector<std::uint8_t> bytes;
                bytes.reserve(value.size());
                for (const auto &v : value) {
                    if (!v.isUInt()) {
                        return std::nullopt;
                    }
                    bytes.push_back(static_cast<std::uint8_t>(v.asUInt()));
                }
                return FieldValue{bytes};
            }
            break;
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

Json::Value fieldsToJson(const std::map<std::string, FieldValue> &fields) {
    Json::Value json(Json::objectValue);
    for (const auto &[name, value] : fields) {
        json[name] = fieldValueToJson(value);
    }
    return json;
}

Json::Value telegramToJson(const TelegramDef &telegram, const std::shared_ptr<TelegramRuntime> &runtime) {
    Json::Value json;
    json["comId"] = telegram.comId;
    json["name"] = telegram.name;
    json["dataset"] = telegram.datasetName;
    json["direction"] = telegram.direction == Direction::Tx ? "Tx" : "Rx";
    json["type"] = telegram.type == TelegramType::PD ? "PD" : "MD";
    if (runtime) {
        json["fields"] = fieldsToJson(runtime->snapshotFields());
    }
    return json;
}
} // namespace

void TelegramController::getTelegram(const drogon::HttpRequestPtr &,
                                     std::function<void(const drogon::HttpResponsePtr &)> &&callback, std::uint32_t comId) {
    if (!ensureRegistryInitialized()) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
        resp->setStatusCode(drogon::k500InternalServerError);
        (*resp->getJsonObject())["error"] = "TRDP registry is not initialised";
        callback(resp);
        return;
    }

    const auto telegram = TelegramRegistry::instance().getTelegramCopy(comId);
    if (!telegram.has_value()) {
        callback(drogon::HttpResponse::newNotFoundResponse());
        return;
    }
    const auto runtime = TelegramRegistry::instance().getOrCreateRuntime(comId);
    callback(drogon::HttpResponse::newHttpJsonResponse(telegramToJson(*telegram, runtime)));
}

void TelegramController::updateFields(const drogon::HttpRequestPtr &req,
                                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                      std::uint32_t comId) {
    if (!ensureRegistryInitialized()) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
        resp->setStatusCode(drogon::k500InternalServerError);
        (*resp->getJsonObject())["error"] = "TRDP registry is not initialised";
        callback(resp);
        return;
    }

    const auto telegram = TelegramRegistry::instance().getTelegramCopy(comId);
    if (!telegram.has_value()) {
        callback(drogon::HttpResponse::newNotFoundResponse());
        return;
    }

    const auto dataset = TelegramRegistry::instance().getDatasetCopy(telegram->datasetName);
    if (!dataset.has_value()) {
        callback(drogon::HttpResponse::newNotFoundResponse());
        return;
    }

    const auto json = req->getJsonObject();
    if (!json) {
        callback(drogon::HttpResponse::newHttpResponse());
        return;
    }

    auto runtime = TelegramRegistry::instance().getOrCreateRuntime(comId);
    if (!runtime) {
        callback(drogon::HttpResponse::newHttpResponse());
        return;
    }

    for (const auto &memberName : json->getMemberNames()) {
        const auto *fieldDef = dataset->findField(memberName);
        if (fieldDef == nullptr) {
            continue;
        }

        const auto &value = (*json)[memberName];
        if (value.isNull()) {
            runtime->setFieldValue(memberName, defaultValueForField(*fieldDef));
            continue;
        }

        const auto parsed = jsonToFieldValue(*fieldDef, value);
        if (!parsed.has_value()) {
            continue;
        }
        runtime->setFieldValue(memberName, parsed.value());
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(fieldsToJson(runtime->snapshotFields())));
}

void TelegramController::sendTelegram(const drogon::HttpRequestPtr &req,
                                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                                      std::uint32_t comId) {
    if (!ensureRegistryInitialized()) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
        resp->setStatusCode(drogon::k500InternalServerError);
        (*resp->getJsonObject())["error"] = "TRDP registry is not initialised";
        callback(resp);
        return;
    }

    const auto telegram = TelegramRegistry::instance().getTelegramCopy(comId);
    if (!telegram.has_value()) {
        callback(drogon::HttpResponse::newNotFoundResponse());
        return;
    }

    const auto dataset = TelegramRegistry::instance().getDatasetCopy(telegram->datasetName);
    if (!dataset.has_value()) {
        callback(drogon::HttpResponse::newNotFoundResponse());
        return;
    }

    const auto json = req->getJsonObject();
    std::map<std::string, FieldValue> overrides;
    if (json) {
        for (const auto &memberName : json->getMemberNames()) {
            const auto *fieldDef = dataset->findField(memberName);
            if (fieldDef == nullptr) {
                continue;
            }

            const auto &value = (*json)[memberName];
            if (value.isNull()) {
                overrides.emplace(memberName, defaultValueForField(*fieldDef));
                continue;
            }

            const auto parsed = jsonToFieldValue(*fieldDef, value);
            if (parsed.has_value()) {
                overrides.emplace(memberName, parsed.value());
            }
        }
    }

    const bool success = TrdpEngine::instance().sendTxTelegram(comId, overrides);
    auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
    (*resp->getJsonObject())["ok"] = success;
    if (!success) {
        resp->setStatusCode(drogon::k500InternalServerError);
    }
    callback(resp);
}

} // namespace trdp

