#include "server/configuration.hpp"

#include "server/logger.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace ipcm::server {
namespace {

std::string_view require_value(int& index, int argc, char** argv, std::string_view option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument("missing value for " + std::string(option));
    }
    ++index;
    return argv[index];
}

std::size_t parse_size(std::string_view value, std::string_view option) {
    std::size_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    // Не допускаем частичный разбор вроде "100abc", переполнение и нулевой лимит.
    if (error != std::errc{} || end != value.data() + value.size() || result == 0) {
        throw std::invalid_argument("invalid positive integer for " + std::string(option));
    }
    return result;
}

unsigned int parse_mode(std::string_view value) {
    // Права Unix задаются в восьмеричной системе; разрешаем только биты rwx.
    unsigned int result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result, 8);
    if (error != std::errc{} || end != value.data() + value.size() || result > 0777U) {
        throw std::invalid_argument("invalid octal mode for --socket-mode");
    }
    return result;
}

} // namespace

Configuration::Configuration() : log_level(LogLevel::info) {}

Configuration Configuration::parse(int argc, char** argv) {
    Configuration result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--socket") {
            result.socket_path = require_value(index, argc, argv, option);
        } else if (option == "--max-message-size") {
            const auto value = parse_size(require_value(index, argc, argv, option), option);
            // Размер тела передаётся в протоколе как uint32, даже на 64-битной системе.
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("--max-message-size is too large");
            }
            result.maximum_message_size = static_cast<std::uint32_t>(value);
        } else if (option == "--max-client-buffer") {
            result.maximum_client_buffer = parse_size(require_value(index, argc, argv, option), option);
        } else if (option == "--max-sessions") {
            result.maximum_sessions = parse_size(require_value(index, argc, argv, option), option);
        } else if (option == "--socket-mode") {
            result.socket_mode = parse_mode(require_value(index, argc, argv, option));
        } else if (option == "--log-level") {
            result.log_level = parse_log_level(require_value(index, argc, argv, option));
        } else if (option == "--help" || option == "-h") {
            throw std::invalid_argument("help requested");
        } else {
            throw std::invalid_argument("unknown option: " + std::string(option));
        }
    }

    // Иначе допустимый по протоколу кадр никогда не поместится в буфер сессии.
    if (result.maximum_client_buffer < result.maximum_message_size + protocol::kHeaderSize) {
        throw std::invalid_argument("--max-client-buffer must fit at least one maximum-sized frame");
    }
    return result;
}

std::string Configuration::help(std::string_view executable) {
    return "Usage: " + std::string(executable) + " [options]\n"
           "  --socket PATH              Unix socket path (default: /tmp/ipcm-server.sock)\n"
           "  --socket-mode OCTAL        Socket permissions (default: 0660)\n"
           "  --max-message-size BYTES   Maximum frame body (default: 65536)\n"
           "  --max-client-buffer BYTES  Per-client input/output limit (default: 1048576)\n"
           "  --max-sessions COUNT       Connection limit (default: 4096)\n"
           "  --log-level LEVEL          error, warn, info, debug or trace\n"
           "  -h, --help                 Show this help\n";
}

} // namespace ipcm::server
