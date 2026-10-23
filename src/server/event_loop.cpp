#include "server/event_loop.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ipcm::server {
namespace {

void control(int epoll_fd, int operation, int fd, std::uint32_t flags) {
    epoll_event event{};
    event.events = flags;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd, operation, fd, &event) == -1) {
        throw std::runtime_error("epoll_ctl failed: " + std::string(std::strerror(errno)));
    }
}

} // namespace

EventLoop::EventLoop(std::size_t maximum_events) : native_events_(maximum_events) {
    if (maximum_events == 0) {
        throw std::invalid_argument("maximum_events must be positive");
    }
    fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd_ == -1) {
        throw std::runtime_error("epoll_create1 failed: " + std::string(std::strerror(errno)));
    }
}

EventLoop::~EventLoop() {
    if (fd_ != -1) {
        ::close(fd_);
    }
}

void EventLoop::add(int fd, std::uint32_t flags) {
    control(fd_, EPOLL_CTL_ADD, fd, flags);
}

void EventLoop::modify(int fd, std::uint32_t flags) {
    control(fd_, EPOLL_CTL_MOD, fd, flags);
}

void EventLoop::remove(int fd) noexcept {
    ::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);
}

std::span<const Event> EventLoop::wait() {
    const int count = ::epoll_wait(
        fd_, native_events_.data(), static_cast<int>(native_events_.size()), -1);

    if (count == -1 && errno == EINTR) {
        events_.clear();
        return events_;
    }

    if (count == -1) {
        throw std::runtime_error("epoll_wait failed: " + std::string(std::strerror(errno)));
    }

    events_.clear();
    events_.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        events_.push_back(Event{
            .fd = native_events_[static_cast<std::size_t>(index)].data.fd,
            .flags = native_events_[static_cast<std::size_t>(index)].events,
        });
    }
    return events_;
}

} // namespace ipcm::server
