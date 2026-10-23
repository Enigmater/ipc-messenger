#include "client_core/client.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <chrono>

namespace ipcm::client {
using namespace protocol;
namespace {
std::uint64_t now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
void remember(User& user, Message message) {
    if (user.history.size() >= 1000) user.history.pop_front();
    user.history.push_back(std::move(message));
}
}
Client::Client(const std::string& path) : started_(std::chrono::steady_clock::now()) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path))
        throw std::runtime_error("Invalid socket path");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) throw std::runtime_error(std::strerror(errno));
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string error = "Cannot connect to " + path + ": " + std::strerror(errno);
        ::close(fd_); fd_ = -1;
        throw std::runtime_error(error);
    }
    BodyWriter body; body.string("messenger-tui"); body.string("0.1.0");
    queue(MessageType::hello, std::move(body), 1);
}
Client::~Client() { if (fd_ >= 0) ::close(fd_); }
void Client::queue(MessageType type, BodyWriter body, std::uint64_t id) {
    auto bytes = encode_frame({kVersion, type, id, std::move(body).take()});
    if (queued_ + bytes.size() > 1024 * 1024) throw std::runtime_error("Outgoing queue is full");
    queued_ += bytes.size(); output_.push_back(std::move(bytes));
}
void Client::refresh() { if (ready) queue(MessageType::list_users); }
void Client::send(std::uint32_t recipient, const std::string& text) {
    if (!ready || !connected) throw std::runtime_error("Not connected");
    if (text.empty()) throw std::runtime_error("Message is empty");
    if (text.size() + name.size() + 28 > kDefaultMaximumBodySize)
        throw std::runtime_error("Message is too large");
    const auto id = next_++;
    BodyWriter body; body.u32(recipient); body.u64(id); body.string(text);
    queue(MessageType::send_message, std::move(body), id);
    pending_[id] = recipient;
    remember(users[recipient], {id, now(), name, text, "sending", true, 0});
}
void Client::lost(const std::string& reason) {
    connected = ready = false; status = "Disconnected: " + reason;
    for (auto& [id, user] : pending_)
        for (auto& message : users[user].history)
            if (message.outgoing && message.id == id) message.state = "unknown: connection lost";
    pending_.clear(); output_.clear(); queued_ = offset_ = 0;
}
void Client::update() {
    if (!connected) return;
    try {
        if (!ready && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(5))
            throw std::runtime_error("Handshake timed out");
        std::size_t budget = 65536;
        while (!output_.empty() && budget) {
            auto& bytes = output_.front();
            const auto count = ::send(fd_, bytes.data() + offset_,
                std::min(bytes.size() - offset_, budget), MSG_NOSIGNAL);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                throw std::runtime_error(std::strerror(errno));
            }
            if (!count) throw std::runtime_error("Socket write failed");
            const auto size = static_cast<std::size_t>(count);
            offset_ += size; budget -= size;
            if (offset_ == bytes.size()) {
                queued_ -= bytes.size(); output_.pop_front(); offset_ = 0;
            }
        }
        std::array<std::byte, 16384> buffer{};
        for (int i = 0; i < 16; ++i) {
            const auto count = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                throw std::runtime_error(std::strerror(errno));
            }
            if (!count) { lost("Disconnected by server"); return; }
            parser_.append(std::span(buffer).first(static_cast<std::size_t>(count)));
            while (auto frame = parser_.next()) handle(*frame);
        }
    } catch (const std::exception& error) { lost(error.what()); }
}
void Client::handle(const Frame& frame) {
    if (frame.version != kVersion) throw ProtocolError("Unsupported server protocol");
    if (!ready && frame.type != MessageType::welcome && frame.type != MessageType::error)
        throw ProtocolError("Expected WELCOME");
    BodyReader body(frame.body);
    switch (frame.type) {
    case MessageType::welcome:
        if (ready) throw ProtocolError("Duplicate WELCOME");
        uid = body.u32(); (void)body.u32(); name = body.string(); (void)body.string();
        ready = true; status = "Connected as " + name; refresh(); break;
    case MessageType::user_list: {
        auto count = body.u32();
        if (count > frame.body.size() / 9) throw ProtocolError("Invalid user count");
        while (count--) {
            const auto id = body.u32(); const bool online = body.u8() != 0;
            auto username = body.string();
            users[id].name = std::move(username); users[id].online = online;
        }
        break;
    }
    case MessageType::user_status: {
        const auto id = body.u32(); const bool online = body.u8() != 0;
        if (!users.contains(id)) refresh();
        else users[id].online = online;
        break;
    }
    case MessageType::incoming_message: {
        const auto id = body.u64(); const auto sender = body.u32();
        auto username = body.string(); const auto timestamp = body.u64(); auto text = body.string();
        auto& user = users[sender]; user.name = username; user.online = true;
        // WELCOME identifies the user, while server IDs identify messages across sessions.
        // The acknowledgement can arrive before or after our own INCOMING_MESSAGE.
        auto own = std::find_if(user.history.begin(), user.history.end(), [&](const Message& message) {
            return message.outgoing && message.server_id == id;
        });
        if (sender == uid && own != user.history.end()) {
            own->timestamp = timestamp;
        } else {
            ++user.unread;
            remember(user, {id, timestamp, std::move(username), std::move(text), "received", false, id});
        }
        break;
    }
    case MessageType::message_accepted: {
        const auto id = body.u64(); const auto server_id = body.u64(); const auto state = body.u8();
        if (state != 1 && state != 2) throw ProtocolError("Invalid delivery state");
        if (pending_.contains(id)) {
            const auto recipient = pending_.at(id);
            auto& user = users[recipient];
            auto own = std::find_if(user.history.begin(), user.history.end(), [&](const Message& message) {
                return message.outgoing && message.id == id;
            });
            if (own != user.history.end()) {
                own->server_id = server_id;
                own->state = state == 2 ? "queued to recipient" : "accepted";
                if (recipient == uid) {
                    for (const auto& echo : user.history)
                        if (!echo.outgoing && echo.server_id == server_id) own->timestamp = echo.timestamp;
                    auto removed = std::erase_if(user.history, [&](const Message& message) {
                        return !message.outgoing && message.server_id == server_id;
                    });
                    user.unread -= std::min(user.unread, static_cast<unsigned>(removed));
                }
            }
            pending_.erase(id);
        }
        break;
    }
    case MessageType::error: {
        (void)body.u16(); status = body.string();
        if (!ready) throw ProtocolError(status);
        if (pending_.contains(frame.request_id)) {
            for (auto& message : users[pending_.at(frame.request_id)].history)
                if (message.outgoing && message.id == frame.request_id) message.state = "error: " + status;
            pending_.erase(frame.request_id);
        }
        break;
    }
    case MessageType::pong: break;
    default: throw ProtocolError("Unexpected server frame");
    }
    body.require_finished();
}
}
