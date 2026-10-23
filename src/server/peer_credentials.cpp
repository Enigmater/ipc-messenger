#include "server/peer_credentials.hpp"

#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ipcm::server {

PeerCredentials read_peer_credentials(int socket_fd) {
    ucred native{};
    socklen_t size = sizeof(native);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &native, &size) == -1) {
        throw std::runtime_error("SO_PEERCRED failed: " + std::string(std::strerror(errno)));
    }

    long buffer_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (buffer_size < 1024) {
        buffer_size = 16384;
    }
    std::vector<char> buffer(static_cast<std::size_t>(buffer_size));
    passwd entry{};
    passwd* result = nullptr;
    const int error = ::getpwuid_r(native.uid, &entry, buffer.data(), buffer.size(), &result);

    std::string username;
    if (error == 0 && result != nullptr) {
        username = result->pw_name;
    } else {
        username = std::to_string(native.uid);
    }

    return PeerCredentials{
        .pid = native.pid,
        .uid = native.uid,
        .gid = native.gid,
        .username = std::move(username),
    };
}

} // namespace ipcm::server
