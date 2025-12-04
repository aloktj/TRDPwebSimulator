#include "controllers/WsTelegram.h"

#include "plugins/TelegramHub.h"

namespace trdp {

void WsTelegram::handleNewConnection(const drogon::HttpRequestPtr &, const drogon::WebSocketConnectionPtr &conn) {
    if (auto *hub = TelegramHub::instance()) {
        hub->subscribe(conn);
    }
}

void WsTelegram::handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) {
    if (auto *hub = TelegramHub::instance()) {
        hub->unsubscribe(conn);
    }
}

void WsTelegram::handleNewMessage(const drogon::WebSocketConnectionPtr &conn, std::string &&message,
                                  const drogon::WebSocketMessageType &type) {
    (void)conn;
    (void)message;
    (void)type;
    // This controller is push-only; incoming messages are ignored for now.
}

} // namespace trdp

