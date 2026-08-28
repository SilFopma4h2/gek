// logger.cpp
//
// Thread-safe error log. Keeps a single std::ofstream open for the
// lifetime of the process instead of opening/closing the file on every
// call, which is wasteful when an order book generates bursts of errors.
#include "logger.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace {

std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

std::ofstream& logFile() {
    // leak the ofstream intentionally; lifetime == process lifetime, and
    // its destructor would otherwise run after static destruction where
    // we'd rather not touch the filesystem.
    static std::ofstream* s = nullptr;
    static bool initialised = false;
    if (!initialised) {
        s = new std::ofstream("error.log", std::ios::app);
        initialised = true;
    }
    return *s;
}

std::string timestampLine(const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    std::string line = "[";
    line += ts;
    line += "] ";
    line += message;
    line += '\n';
    return line;
}

} // namespace

void logError(const std::string& message) {
    const std::string line = timestampLine(message);
    std::lock_guard<std::mutex> lock(logMutex());
    auto& file = logFile();
    if (file.is_open()) {
        file << line;
        file.flush();
    } else {
        std::cerr << line;
    }
}
