#pragma once

#include <drogon/HttpController.h>

namespace trdp {

class ConfigController : public drogon::HttpController<ConfigController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ConfigController::loadConfig, "/api/config/load", drogon::Post);
    ADD_METHOD_TO(ConfigController::listDatasets, "/api/config/datasets", drogon::Get);
    ADD_METHOD_TO(ConfigController::listTelegrams, "/api/config/telegrams", drogon::Get);
    METHOD_LIST_END

    void loadConfig(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void listDatasets(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
    void listTelegrams(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback);
};

} // namespace trdp

