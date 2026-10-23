#pragma once

#include "common/protocol.hpp"
#include "server/peer_credentials.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace ipcm::server {

enum class ConnectionState {
    waiting_for_hello,
    active,
    closing, // Закрытие откладывается до опустошения исходящей очереди.
};

struct OutgoingFrame {
    std::vector<std::byte> bytes;
    std::size_t position{}; // Уже отправленная часть кадра при частичном send().
};

// Владеет одним сокетом и его буферами. Один UID может иметь несколько сессий.
class ClientSession {
public:
    ClientSession(
        int fd,
        PeerCredentials peer,
        std::uint32_t maximum_message_size,
        std::size_t maximum_buffer_size);
    ~ClientSession();

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const PeerCredentials& peer() const noexcept;
    [[nodiscard]] ConnectionState state() const noexcept;
    [[nodiscard]] bool is_activated() const noexcept;
    void activate() noexcept;
    void close_after_flush() noexcept;
    [[nodiscard]] bool should_close_after_flush() const noexcept;

    void append_input(std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<protocol::Frame> next_frame();

    [[nodiscard]] bool queue(protocol::Frame frame);
    [[nodiscard]] bool has_output() const noexcept;
    [[nodiscard]] OutgoingFrame& front_output();
    void pop_output();

private:
    int fd_;
    PeerCredentials peer_;
    ConnectionState state_{ConnectionState::waiting_for_hello};
    // Помним успешный HELLO даже после перехода active -> closing:
    // реестру это нужно для удаления сессии из индекса активных пользователей.
    bool activated_{false};
    protocol::FrameParser parser_;
    std::size_t maximum_buffer_size_;
    // Полные размеры кадров в очереди, включая частично отправленный первый кадр.
    std::size_t output_size_{0};
    std::deque<OutgoingFrame> output_;
};

} // namespace ipcm::server
