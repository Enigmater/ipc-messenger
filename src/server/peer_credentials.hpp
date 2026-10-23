#pragma once

#include <sys/types.h>

#include <string>

namespace ipcm::server {

struct PeerCredentials {
    pid_t pid{};
    uid_t uid{};
    gid_t gid{};
    std::string username;
};

[[nodiscard]] PeerCredentials read_peer_credentials(int socket_fd);

} // namespace ipcm::server
