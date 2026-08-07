// logger.cpp
#include "logger.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

void logError(const std::string& message) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    const std::string line = "[" + std::string(ts) + "] " + message + "\n";

    std::ofstream file("error.log", std::ios::app);
    if (file) {
        file << line;
    } else {
        std::cerr << line;
    }
}
