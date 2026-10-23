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
    // SO_PEERCRED связывает соединение с Linux-пользователем: клиент не может
    // выдать себя за другой UID, просто передав чужое имя в HELLO.
    ucred native{};
    socklen_t size = sizeof(native);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &native, &size) == -1) {
        throw std::runtime_error("SO_PEERCRED failed: " + std::string(std::strerror(errno)));
    }

    // getpwuid_r использует буфер вызывающего кода. Если sysconf не дал
    // подходящий размер, берём запасной; повторного запроса при ERANGE здесь нет.
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
        // Отсутствие имени в NSS/passwd не меняет личность: UID остаётся достоверным.
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
