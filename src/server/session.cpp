#include "server/session.hpp"

#include <unistd.h>

#include <utility>

namespace ipcm::server {

ClientSession::ClientSession(
    int fd,
    PeerCredentials peer,
    std::uint32_t maximum_message_size,
    std::size_t maximum_buffer_size)
    : fd_(fd),
      peer_(std::move(peer)),
      parser_(maximum_message_size),
      maximum_buffer_size_(maximum_buffer_size) {}

ClientSession::~ClientSession() {
    if (fd_ != -1) {
        ::close(fd_);
    }
}

int ClientSession::fd() const noexcept {
    return fd_;
}

const PeerCredentials& ClientSession::peer() const noexcept {
    return peer_;
}

ConnectionState ClientSession::state() const noexcept {
    return state_;
}

bool ClientSession::is_activated() const noexcept {
    return activated_;
}

void ClientSession::activate() noexcept {
    activated_ = true;
    state_ = ConnectionState::active;
}

void ClientSession::close_after_flush() noexcept {
    // Даём серверу отправить уже поставленный ответ или ERROR перед закрытием сокета.
    state_ = ConnectionState::closing;
}

bool ClientSession::should_close_after_flush() const noexcept {
    return state_ == ConnectionState::closing && output_.empty();
}

void ClientSession::append_input(std::span<const std::byte> bytes) {
    // Проверяем свободное место вычитанием, не складывая потенциально большие размеры.
    if (bytes.size() > maximum_buffer_size_ - parser_.buffered_bytes()) {
        throw protocol::ProtocolError("client input buffer limit exceeded");
    }
    parser_.append(bytes);
}

std::optional<protocol::Frame> ClientSession::next_frame() {
    return parser_.next();
}

bool ClientSession::queue(protocol::Frame frame) {
    // Медленный получатель не должен накапливать неограниченную очередь.
    // false сообщает вызывающему коду о переполнении, не меняя очередь.
    auto encoded = protocol::encode_frame(frame);
    if (encoded.size() > maximum_buffer_size_ - output_size_) {
        return false;
    }
    output_size_ += encoded.size();
    output_.push_back(OutgoingFrame{.bytes = std::move(encoded), .position = 0});
    return true;
}

bool ClientSession::has_output() const noexcept {
    return !output_.empty();
}

OutgoingFrame& ClientSession::front_output() {
    return output_.front();
}

void ClientSession::pop_output() {
    // Память кадра освобождается только после полной отправки, тогда же снимается лимит.
    output_size_ -= output_.front().bytes.size();
    output_.pop_front();
}

} // namespace ipcm::server
