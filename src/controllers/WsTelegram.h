#pragma once

#include <drogon/WebSocketController.h>

namespace trdp {

class WsTelegram : public drogon::WebSocketController<WsTelegram> {
  public:
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws/telegrams", {"*"});
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr &req, const drogon::WebSocketConnectionPtr &conn) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override;
    void handleNewMessage(const drogon::WebSocketConnectionPtr &conn, std::string &&message,
                          const drogon::WebSocketMessageType &type) override;
};

} // namespace trdp

