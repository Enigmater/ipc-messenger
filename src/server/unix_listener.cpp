#include "server/unix_listener.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ipcm::server {
namespace {

std::runtime_error system_error(std::string_view operation) {
    return std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

} // namespace

UnixListener::UnixListener(std::string path, unsigned int mode) : path_(std::move(path)) {
    sockaddr_un address{};
    if (path_.empty() || path_.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("Unix socket path is empty or too long");
    }

    prepare_path();
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ == -1) {
        throw system_error("socket failed");
    }

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1U);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path_.size() + 1U);

    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), address_size) == -1) {
        const auto error = system_error("bind failed");
        ::close(fd_);
        fd_ = -1;
        throw error;
    }
    owns_path_ = true;

    if (::chmod(path_.c_str(), static_cast<mode_t>(mode)) == -1) {
        const auto error = system_error("chmod failed");
        ::close(fd_);
        fd_ = -1;
        ::unlink(path_.c_str());
        owns_path_ = false;
        throw error;
    }

    if (::listen(fd_, SOMAXCONN) == -1) {
        const auto error = system_error("listen failed");
        ::close(fd_);
        fd_ = -1;
        ::unlink(path_.c_str());
        owns_path_ = false;
        throw error;
    }
}

UnixListener::~UnixListener() {
    if (fd_ != -1) {
        ::close(fd_);
    }
    if (owns_path_) {
        ::unlink(path_.c_str());
    }
}

int UnixListener::fd() const noexcept {
    return fd_;
}

const std::string& UnixListener::path() const noexcept {
    return path_;
}

int UnixListener::accept() const {
    return ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

void UnixListener::prepare_path() {
    struct stat information {};
    if (::lstat(path_.c_str(), &information) == -1) {
        if (errno == ENOENT) {
            return;
        }
        throw system_error("lstat failed");
    }

    if (!S_ISSOCK(information.st_mode)) {
        throw std::runtime_error("refusing to remove non-socket object at " + path_);
    }
    if (information.st_uid != ::geteuid()) {
        throw std::runtime_error("refusing to remove socket owned by another user at " + path_);
    }

    const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe == -1) {
        throw system_error("socket probe failed");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1U);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path_.size() + 1U);
    const int connect_result = ::connect(
        probe, reinterpret_cast<const sockaddr*>(&address), address_size);
    const int connect_error = errno;
    ::close(probe);

    if (connect_result == 0 || connect_error == EINPROGRESS || connect_error == EAGAIN) {
        throw std::runtime_error("another server is already listening at " + path_);
    }
    if (connect_error != ECONNREFUSED && connect_error != ENOENT) {
        throw std::runtime_error(
            "cannot verify existing socket at " + path_ + ": " + std::strerror(connect_error));
    }
    if (::unlink(path_.c_str()) == -1 && errno != ENOENT) {
        throw system_error("unlink stale socket failed");
    }
}

} // namespace ipcm::server
