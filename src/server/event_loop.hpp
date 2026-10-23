#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <span>
#include <vector>

namespace ipcm::server {

struct Event {
    int fd{-1};
    std::uint32_t flags{};
};

class EventLoop {
public:
    explicit EventLoop(std::size_t maximum_events = 128);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void add(int fd, std::uint32_t flags);
    void modify(int fd, std::uint32_t flags);
    void remove(int fd) noexcept;
    [[nodiscard]] std::span<const Event> wait();

private:
    int fd_{-1};
    std::vector<epoll_event> native_events_;
    std::vector<Event> events_;
};

} // namespace ipcm::server
