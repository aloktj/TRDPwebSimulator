#pragma once

#include "telegram_model.h"

#include <drogon/plugins/Plugin.h>

#include <mutex>
#include <set>

namespace trdp {

class TelegramHub : public drogon::Plugin<TelegramHub> {
  public:
    TelegramHub();

    void initAndStart(const Json::Value &config) override;
    void shutdownAndCleanup() override;

    void subscribe(const std::shared_ptr<drogon::WebSocketConnection> &conn);
    void unsubscribe(const drogon::WebSocketConnection *conn);

    void publishRxUpdate(std::uint32_t comId, const std::map<std::string, FieldValue> &fields);
    void publishTxConfirmation(std::uint32_t comId, const std::map<std::string, FieldValue> &fields);

    void sendSnapshot(const std::shared_ptr<drogon::WebSocketConnection> &conn);

    static TelegramHub *instance();

  private:
    void broadcast(const Json::Value &payload);
    Json::Value fieldsToJson(const std::map<std::string, FieldValue> &fields) const;
    Json::Value telegramToJson(const TelegramDef &telegram) const;

    std::mutex connMtx;
    std::set<std::shared_ptr<drogon::WebSocketConnection>> connections;
};

} // namespace trdp

