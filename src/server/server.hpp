#pragma once

#include "common/protocol.hpp"
#include "server/configuration.hpp"
#include "server/event_loop.hpp"
#include "server/logger.hpp"
#include "server/session_registry.hpp"
#include "server/unix_listener.hpp"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace ipcm::server {

// Один поток обслуживает все сокеты через epoll; состояние сессий не требует mutex.
// Остановка запрашивается извне через флаг, переданный из main().
class Server {
public:
    Server(Configuration configuration, std::atomic_bool& stop_requested);
    void run();

private:
    void accept_clients();
    void handle_event(const Event& event);
    // false означает, что сессия уже отключена и ссылку больше нельзя использовать.
    [[nodiscard]] bool read_from(ClientSession& session);
    [[nodiscard]] bool write_to(ClientSession& session);
    void handle_frame(ClientSession& session, const protocol::Frame& frame);
    void handle_hello(ClientSession& session, const protocol::Frame& frame);
    void handle_list_users(ClientSession& session, const protocol::Frame& frame);
    void handle_send_message(ClientSession& session, const protocol::Frame& frame);
    void handle_ping(ClientSession& session, const protocol::Frame& frame);

    void queue(ClientSession& session, protocol::Frame frame);
    void queue_error(
        ClientSession& session,
        std::uint64_t request_id,
        protocol::ErrorCode code,
        std::string_view message,
        bool close_after);
    void update_interest(const ClientSession& session);
    void disconnect(int fd, std::string_view reason);
    void broadcast_user_status(uid_t uid, bool online);

    [[nodiscard]] protocol::Frame welcome_frame(const ClientSession& session, std::uint64_t request_id) const;
    [[nodiscard]] protocol::Frame user_list_frame(std::uint64_t request_id) const;

    Configuration configuration_;
    std::atomic_bool& stop_requested_;
    Logger logger_;
    EventLoop event_loop_;
    UnixListener listener_;
    SessionRegistry sessions_;
    // Общая последовательность для всех отправителей; сбрасывается при перезапуске.
    std::uint64_t next_message_id_{1};
};

} // namespace ipcm::server
