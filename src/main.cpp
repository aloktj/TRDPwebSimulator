#include "plugins/TelegramHub.h"

#include <drogon/drogon.h>

int main(int argc, char **argv) {
    using namespace trdp;
    drogon::app().registerPlugin<TelegramHub>();
    drogon::app().addListener("0.0.0.0", 8080);
    drogon::app().setThreadNum(2);

    if (argc > 1) {
        trdp::setDefaultXmlConfig(argv[1]);
    }

    drogon::app().run();
    return 0;
}

