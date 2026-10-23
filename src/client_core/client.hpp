#pragma once
#include "common/protocol.hpp"
#include <deque>
#include <chrono>
#include <map>

namespace ipcm::client {
struct Message {
    std::uint64_t id{}, timestamp{};
    std::string sender, text, state;
    bool outgoing{};
    std::uint64_t server_id{};
};
struct User {
    std::string name;
    bool online{};
    unsigned unread{};
    std::deque<Message> history;
};
// All methods run on the caller's event loop; no blocking socket operations.
class Client {
public:
    explicit Client(const std::string& path);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void update();
    void refresh();
    void send(std::uint32_t recipient, const std::string& text);
    std::map<std::uint32_t, User> users;
    std::uint32_t uid{};
    std::string name, status{"Connecting..."};
    bool ready{}, connected{true};
private:
    void queue(protocol::MessageType type, protocol::BodyWriter body = {}, std::uint64_t id = 0);
    void handle(const protocol::Frame& frame);
    void lost(const std::string& reason);
    int fd_{-1};
    protocol::FrameParser parser_;
    std::deque<std::vector<std::byte>> output_;
    std::size_t offset_{}, queued_{};
    std::uint64_t next_{2};
    std::map<std::uint64_t, std::uint32_t> pending_;
    std::chrono::steady_clock::time_point started_;
};
}
