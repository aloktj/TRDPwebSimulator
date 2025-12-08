#pragma once

#include <drogon/HttpController.h>

namespace trdp {

class TelegramController : public drogon::HttpController<TelegramController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TelegramController::getTelegram, "/api/telegrams/{1}", drogon::Get);
    ADD_METHOD_TO(TelegramController::updateFields, "/api/telegrams/{1}/fields", drogon::Post);
    ADD_METHOD_TO(TelegramController::sendTelegram, "/api/telegrams/{1}/send", drogon::Post);
    ADD_METHOD_TO(TelegramController::stopTelegram, "/api/telegrams/{1}/stop", drogon::Post);
    METHOD_LIST_END

    void getTelegram(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                     std::uint32_t comId);

    void updateFields(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::uint32_t comId);

    void sendTelegram(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::uint32_t comId);

    void stopTelegram(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      std::uint32_t comId);
};

} // namespace trdp

