#include "controllers/ConfigController.h"

#include "telegram_model.h"
#include "trdp_engine.h"

#include <drogon/drogon.h>

namespace trdp {

namespace {
Json::Value datasetToJson(const DatasetDef &dataset) {
    Json::Value json;
    json["name"] = dataset.name;
    json["size"] = static_cast<Json::UInt64>(dataset.computeSize());
    for (const auto &field : dataset.fields) {
        Json::Value f;
        f["name"] = field.name;
        f["type"] = static_cast<int>(field.type);
        f["offset"] = static_cast<Json::UInt64>(field.offset);
        f["size"] = static_cast<Json::UInt64>(field.size);
        f["bitOffset"] = static_cast<Json::UInt64>(field.bitOffset);
        f["arrayLength"] = static_cast<Json::UInt64>(field.arrayLength);
        json["fields"].append(f);
    }
    return json;
}

Json::Value telegramToJson(const TelegramDef &telegram) {
    Json::Value json;
    json["comId"] = telegram.comId;
    json["name"] = telegram.name;
    json["dataset"] = telegram.datasetName;
    json["direction"] = telegram.direction == Direction::Tx ? "Tx" : "Rx";
    json["type"] = telegram.type == TelegramType::PD ? "PD" : "MD";
    json["expectedReplies"] = static_cast<Json::UInt64>(telegram.expectedReplies);
    json["replyTimeoutMs"] = static_cast<Json::UInt64>(telegram.replyTimeout.count());
    json["confirmTimeoutMs"] = static_cast<Json::UInt64>(telegram.confirmTimeout.count());
    return json;
}
} // namespace

void ConfigController::loadConfig(const drogon::HttpRequestPtr &req,
                                  std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
    const auto json = req->getJsonObject();
    if (!json || !(*json).isMember("path")) {
        resp->setStatusCode(drogon::k400BadRequest);
        (*resp->getJsonObject())["error"] = "Missing 'path' field";
        callback(resp);
        return;
    }

    const auto path = (*json)["path"].asString();
    TrdpEngine::instance().stop();
    setDefaultXmlConfig(path);
    if (!loadFromTauXml(path)) {
        resp->setStatusCode(drogon::k500InternalServerError);
        (*resp->getJsonObject())["error"] = "Failed to load XML";
        callback(resp);
        return;
    }

    if (!TrdpEngine::instance().start()) {
        resp->setStatusCode(drogon::k500InternalServerError);
        (*resp->getJsonObject())["error"] = "TRDP engine failed to start";
        callback(resp);
        return;
    }

    (*resp->getJsonObject())["status"] = "ok";
    callback(resp);
}

void ConfigController::listDatasets(const drogon::HttpRequestPtr &,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    if (!ensureRegistryInitialized()) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
        resp->setStatusCode(drogon::k500InternalServerError);
        (*resp->getJsonObject())["error"] = "TRDP registry is not initialised";
        callback(resp);
        return;
    }

    Json::Value json;
    for (const auto &dataset : TelegramRegistry::instance().listDatasets()) {
        json.append(datasetToJson(dataset));
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(json));
}

void ConfigController::listTelegrams(const drogon::HttpRequestPtr &,
                                     std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    if (!ensureRegistryInitialized()) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(Json::Value());
        resp->setStatusCode(drogon::k500InternalServerError);
        (*resp->getJsonObject())["error"] = "TRDP registry is not initialised";
        callback(resp);
        return;
    }

    Json::Value json;
    for (const auto &telegram : TelegramRegistry::instance().listTelegrams()) {
        json.append(telegramToJson(telegram));
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(json));
}

} // namespace trdp

