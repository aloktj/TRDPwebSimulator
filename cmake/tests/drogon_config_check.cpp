#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    std::atomic<bool> started{false};

    try
    {
        auto &app = drogon::app();
        app.setLogLevel(trantor::Logger::kWarn);
        app.setThreadNum(1);
        app.addListener("127.0.0.1", 0);
        app.registerBeginningAdvice([&started]() { started = true; });

        std::thread runner([&app]() { app.run(); });
        for (int i = 0; i < 50 && !started.load(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        app.quit();
        runner.join();
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Drogon library detected but startup failed: " << ex.what() << std::endl;
        return 1;
    }

    if (!started.load())
    {
        std::cerr << "Drogon library detected but startup advice never fired." << std::endl;
        return 1;
    }

    std::cout << "Drogon framework detected and started successfully." << std::endl;
    return 0;
}
