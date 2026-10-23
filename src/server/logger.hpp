#pragma once

#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ipcm::server {

enum class LogLevel {
    // Чем выше значение, тем подробнее журнал; level_ задаёт верхнюю границу.
    error = 0,
    warning = 1,
    info = 2,
    debug = 3,
    trace = 4,
};

[[nodiscard]] LogLevel parse_log_level(std::string_view value);

class Logger {
public:
    explicit Logger(LogLevel level) noexcept;

    template <typename... Values>
    void log(LogLevel level, Values&&... values) {
        // Отсеиваем запись до форматирования, чтобы не создавать строку без необходимости.
        if (level > level_) {
            return;
        }
        std::ostringstream message;
        // Fold expression собирает аргументы разных типов в одно сообщение.
        (message << ... << std::forward<Values>(values));
        write(level, message.str());
    }

private:
    void write(LogLevel level, std::string_view message);

    LogLevel level_;
    std::mutex mutex_;
};

} // namespace ipcm::server
