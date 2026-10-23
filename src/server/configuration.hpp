#pragma once

#include "common/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ipcm::server {

enum class LogLevel;

struct Configuration {
    std::string socket_path{"/tmp/ipcm-server.sock"};
    std::uint32_t maximum_message_size{protocol::kDefaultMaximumBodySize};
    std::size_t maximum_client_buffer{1024U * 1024U};
    std::size_t maximum_sessions{4096};
    unsigned int socket_mode{0660};
    LogLevel log_level;

    Configuration();

    [[nodiscard]] static Configuration parse(int argc, char** argv);
    [[nodiscard]] static std::string help(std::string_view executable);
};

} // namespace ipcm::server
