#pragma once

#include "telegram_model.h"

#include <drogon/plugins/Plugin.h>
#include <drogon/WebSocketConnection.h>

#include <mutex>
#include <optional>
#include <set>

namespace trdp {

class TelegramHub : public drogon::Plugin<TelegramHub> {
  public:
    TelegramHub();

    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    void subscribe(const drogon::WebSocketConnectionPtr &conn);
    void unsubscribe(const drogon::WebSocketConnectionPtr &conn);

    void publishRxUpdate(std::uint32_t comId, const std::map<std::string, FieldValue> &fields);
    void publishTxConfirmation(std::uint32_t comId, const std::map<std::string, FieldValue> &fields,
                               std::optional<bool> txActive = std::nullopt);

    void sendSnapshot(const drogon::WebSocketConnectionPtr &conn);

    static TelegramHub *instance();

  private:
    void broadcast(const Json::Value &payload);
    Json::Value fieldsToJson(const std::map<std::string, FieldValue> &fields) const;
    Json::Value telegramToJson(const TelegramDef &telegram) const;

    std::mutex connMtx;
    std::set<drogon::WebSocketConnectionPtr> connections;
};

} // namespace trdp

