// logger.h
#pragma once
#include <string>

// appends a timestamped error line to error.log (thread-safe).
// falls back to cerr if the file cannot be opened.
void logError(const std::string& message);
