#include "server/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace ipcm::server {
namespace {

std::string_view label(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::error:
        return "ERROR";
    case LogLevel::warning:
        return "WARN ";
    case LogLevel::info:
        return "INFO ";
    case LogLevel::debug:
        return "DEBUG";
    case LogLevel::trace:
        return "TRACE";
    }
    return "?????";
}

} // namespace

LogLevel parse_log_level(std::string_view value) {
    if (value == "error") {
        return LogLevel::error;
    }
    if (value == "warn" || value == "warning") {
        return LogLevel::warning;
    }
    if (value == "info") {
        return LogLevel::info;
    }
    if (value == "debug") {
        return LogLevel::debug;
    }
    if (value == "trace") {
        return LogLevel::trace;
    }
    throw std::invalid_argument("invalid log level: " + std::string(value));
}

Logger::Logger(LogLevel level) noexcept : level_(level) {}

void Logger::write(LogLevel level, std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    // Результат хранится в локальном tm, а не в общем статическом буфере localtime().
    localtime_r(&time, &local);

    // Защищаем вывод всей записи, чтобы строки разных вызовов не перемешивались.
    std::lock_guard lock(mutex_);
    std::cerr << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << ' ' << label(level) << ' '
              << message << '\n';
}

} // namespace ipcm::server
