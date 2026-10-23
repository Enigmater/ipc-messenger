#include "server/server.hpp"

#include "server/peer_credentials.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ipcm::server {
namespace {

constexpr std::uint32_t kBaseClientEvents = EPOLLIN | EPOLLRDHUP;
constexpr std::size_t kReadBufferSize = 16U * 1024U;

std::uint32_t checked_uid(uid_t uid) {
    if (static_cast<std::uintmax_t>(uid) > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("UID does not fit protocol field");
    }
    return static_cast<std::uint32_t>(uid);
}

std::uint32_t checked_pid(pid_t pid) {
    if (pid < 0 || static_cast<std::uintmax_t>(pid) > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("PID does not fit protocol field");
    }
    return static_cast<std::uint32_t>(pid);
}

} // namespace

Server::Server(Configuration configuration, std::atomic_bool& stop_requested)
    : configuration_(std::move(configuration)),
      stop_requested_(stop_requested),
      logger_(configuration_.log_level),
      event_loop_(),
      listener_(configuration_.socket_path, configuration_.socket_mode) {
    event_loop_.add(listener_.fd(), EPOLLIN);
}

void Server::run() {
    logger_.log(LogLevel::info, "server started socket=", listener_.path());
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        for (const auto& event : event_loop_.wait()) {
            if (stop_requested_.load(std::memory_order_relaxed)) {
                break;
            }
            handle_event(event);
        }
    }
    logger_.log(LogLevel::info, "server stopping sessions=", sessions_.size());
}

void Server::accept_clients() {
    while (true) {
        const int client_fd = listener_.accept();
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            logger_.log(LogLevel::error, "accept failed: ", std::strerror(errno));
            return;
        }

        if (sessions_.size() >= configuration_.maximum_sessions) {
            logger_.log(LogLevel::warning, "connection rejected: session limit reached");
            ::close(client_fd);
            continue;
        }

        try {
            auto peer = read_peer_credentials(client_fd);
            logger_.log(
                LogLevel::info,
                "client connected fd=", client_fd,
                " uid=", peer.uid,
                " pid=", peer.pid);
            auto session = std::make_unique<ClientSession>(
                client_fd,
                std::move(peer),
                configuration_.maximum_message_size,
                configuration_.maximum_client_buffer);
            event_loop_.add(client_fd, kBaseClientEvents);
            sessions_.add(std::move(session));
        } catch (const std::exception& error) {
            logger_.log(LogLevel::warning, "connection setup failed: ", error.what());
            ::close(client_fd);
        }
    }
}

void Server::handle_event(const Event& event) {
    if (event.fd == listener_.fd()) {
        if ((event.flags & EPOLLIN) != 0U) {
            accept_clients();
        }
        return;
    }

    auto* session = sessions_.find(event.fd);
    if (session == nullptr) {
        return;
    }

    if ((event.flags & (EPOLLERR | EPOLLHUP)) != 0U) {
        disconnect(event.fd, "socket error or hangup");
        return;
    }
    if ((event.flags & EPOLLIN) != 0U && !read_from(*session)) {
        return;
    }

    session = sessions_.find(event.fd);
    if (session == nullptr) {
        return;
    }
    if ((event.flags & EPOLLOUT) != 0U && !write_to(*session)) {
        return;
    }

    session = sessions_.find(event.fd);
    if (session == nullptr) {
        return;
    }
    if ((event.flags & EPOLLRDHUP) != 0U && !session->has_output()) {
        disconnect(event.fd, "peer closed connection");
    }
}

bool Server::read_from(ClientSession& session) {
    std::array<std::byte, kReadBufferSize> buffer{};
    while (true) {
        const auto count = ::recv(session.fd(), buffer.data(), buffer.size(), 0);
        if (count == 0) {
            disconnect(session.fd(), "peer closed connection");
            return false;
        }
        if (count == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            disconnect(session.fd(), std::string("recv failed: ") + std::strerror(errno));
            return false;
        }

        try {
            session.append_input(std::span(buffer).first(static_cast<std::size_t>(count)));
            while (auto frame = session.next_frame()) {
                handle_frame(session, *frame);
                if (session.state() == ConnectionState::closing) {
                    break;
                }
            }
        } catch (const protocol::ProtocolError& error) {
            logger_.log(LogLevel::warning, "protocol error fd=", session.fd(), ": ", error.what());
            queue_error(
                session, 0, protocol::ErrorCode::invalid_frame, error.what(), true);
            break;
        }
    }
    update_interest(session);
    return true;
}

bool Server::write_to(ClientSession& session) {
    while (session.has_output()) {
        auto& output = session.front_output();
        const auto remaining = std::span(output.bytes).subspan(output.position);
        const auto count = ::send(
            session.fd(), remaining.data(), remaining.size(), MSG_NOSIGNAL);
        if (count == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            disconnect(session.fd(), std::string("send failed: ") + std::strerror(errno));
            return false;
        }
        output.position += static_cast<std::size_t>(count);
        if (output.position == output.bytes.size()) {
            session.pop_output();
        }
    }

    if (session.should_close_after_flush()) {
        disconnect(session.fd(), "protocol close");
        return false;
    }
    update_interest(session);
    return true;
}

void Server::handle_frame(ClientSession& session, const protocol::Frame& frame) {
    if (session.state() == ConnectionState::waiting_for_hello &&
        frame.type != protocol::MessageType::hello) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::unexpected_message,
            "HELLO must be the first message",
            true);
        return;
    }
    if (session.state() != ConnectionState::waiting_for_hello &&
        frame.version != protocol::kVersion) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::protocol_mismatch,
            "unsupported protocol version",
            true);
        return;
    }

    switch (frame.type) {
    case protocol::MessageType::hello:
        handle_hello(session, frame);
        break;
    case protocol::MessageType::list_users:
        handle_list_users(session, frame);
        break;
    case protocol::MessageType::send_message:
        handle_send_message(session, frame);
        break;
    case protocol::MessageType::ping:
        handle_ping(session, frame);
        break;
    default:
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::unexpected_message,
            "message type is not accepted from clients",
            false);
        break;
    }
}

void Server::handle_hello(ClientSession& session, const protocol::Frame& frame) {
    if (session.state() != ConnectionState::waiting_for_hello) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::unexpected_message,
            "HELLO was already received",
            false);
        return;
    }
    if (frame.version != protocol::kVersion) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::protocol_mismatch,
            "unsupported protocol version",
            true);
        return;
    }

    protocol::BodyReader body(frame.body);
    const auto client_name = body.string();
    const auto client_version = body.string();
    body.require_finished();
    if (client_name.empty() || client_name.size() > 64 || client_version.size() > 64) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::invalid_message,
            "invalid client name or version",
            true);
        return;
    }

    const bool first_session = sessions_.activate(session);
    queue(session, welcome_frame(session, frame.request_id));
    logger_.log(
        LogLevel::info,
        "client activated fd=", session.fd(),
        " uid=", session.peer().uid,
        " app=", client_name,
        " version=", client_version);
    if (first_session) {
        broadcast_user_status(session.peer().uid, true);
    }
}

void Server::handle_list_users(ClientSession& session, const protocol::Frame& frame) {
    protocol::BodyReader body(frame.body);
    body.require_finished();
    queue(session, user_list_frame(frame.request_id));
}

void Server::handle_send_message(ClientSession& session, const protocol::Frame& frame) {
    protocol::BodyReader body(frame.body);
    const auto recipient_uid_value = body.u32();
    const auto client_message_id = body.u64();
    const auto text = body.string();
    body.require_finished();

    const auto recipient_uid = static_cast<uid_t>(recipient_uid_value);
    const auto incoming_body_size = 28U + session.peer().username.size() + text.size();
    if (text.empty() || incoming_body_size > configuration_.maximum_message_size) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::invalid_message,
            "message text is empty or too large",
            false);
        return;
    }
    if (!sessions_.knows(recipient_uid)) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::user_not_found,
            "recipient is unknown",
            false);
        return;
    }
    auto recipients = sessions_.sessions_for(recipient_uid);
    if (recipients.empty()) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::user_offline,
            "recipient is offline",
            false);
        return;
    }

    const auto server_message_id = next_message_id_++;
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    protocol::BodyWriter incoming_body;
    incoming_body.u64(server_message_id);
    incoming_body.u32(checked_uid(session.peer().uid));
    incoming_body.string(session.peer().username);
    incoming_body.u64(static_cast<std::uint64_t>(timestamp));
    incoming_body.string(text);

    bool delivered = false;
    for (auto* recipient : recipients) {
        protocol::Frame incoming{
            .version = protocol::kVersion,
            .type = protocol::MessageType::incoming_message,
            .request_id = 0,
            .body = incoming_body.data(),
        };
        if (recipient->queue(std::move(incoming))) {
            delivered = true;
            update_interest(*recipient);
        } else {
            logger_.log(
                LogLevel::warning,
                "recipient output limit uid=", recipient_uid,
                " fd=", recipient->fd());
        }
    }

    if (!delivered) {
        queue_error(
            session,
            frame.request_id,
            protocol::ErrorCode::server_busy,
            "all recipient sessions are congested",
            false);
        return;
    }

    protocol::BodyWriter accepted_body;
    accepted_body.u64(client_message_id);
    accepted_body.u64(server_message_id);
    accepted_body.u8(static_cast<std::uint8_t>(protocol::DeliveryState::delivered));
    queue(session, protocol::Frame{
        .version = protocol::kVersion,
        .type = protocol::MessageType::message_accepted,
        .request_id = frame.request_id,
        .body = std::move(accepted_body).take(),
    });
    logger_.log(
        LogLevel::info,
        "message accepted id=", server_message_id,
        " from=", session.peer().uid,
        " to=", recipient_uid,
        " bytes=", text.size());
}

void Server::handle_ping(ClientSession& session, const protocol::Frame& frame) {
    protocol::BodyReader body(frame.body);
    body.require_finished();
    queue(session, protocol::Frame{
        .version = protocol::kVersion,
        .type = protocol::MessageType::pong,
        .request_id = frame.request_id,
        .body = {},
    });
}

void Server::queue(ClientSession& session, protocol::Frame frame) {
    if (!session.queue(std::move(frame))) {
        logger_.log(LogLevel::warning, "client output limit fd=", session.fd());
        session.close_after_flush();
        update_interest(session);
        return;
    }
    update_interest(session);
}

void Server::queue_error(
    ClientSession& session,
    std::uint64_t request_id,
    protocol::ErrorCode code,
    std::string_view message,
    bool close_after) {
    protocol::BodyWriter body;
    body.u16(static_cast<std::uint16_t>(code));
    body.string(message);
    queue(session, protocol::Frame{
        .version = protocol::kVersion,
        .type = protocol::MessageType::error,
        .request_id = request_id,
        .body = std::move(body).take(),
    });
    if (auto* current = sessions_.find(session.fd()); current != nullptr && close_after) {
        current->close_after_flush();
        update_interest(*current);
    }
}

void Server::update_interest(const ClientSession& session) {
    std::uint32_t events = kBaseClientEvents;
    if (session.has_output()) {
        events |= EPOLLOUT;
    }
    event_loop_.modify(session.fd(), events);
}

void Server::disconnect(int fd, std::string_view reason) {
    auto* session = sessions_.find(fd);
    if (session == nullptr) {
        return;
    }
    const auto uid = session->peer().uid;
    const bool was_active = session->is_activated();
    event_loop_.remove(fd);
    auto removed = sessions_.remove(fd);
    const bool became_offline = was_active && !sessions_.online(uid);
    logger_.log(
        LogLevel::info,
        "client disconnected fd=", fd,
        " uid=", uid,
        " reason=", reason);
    removed.reset();
    if (became_offline) {
        broadcast_user_status(uid, false);
    }
}

void Server::broadcast_user_status(uid_t uid, bool online) {
    protocol::BodyWriter body;
    body.u32(checked_uid(uid));
    body.u8(online ? 1U : 0U);
    const auto encoded_body = body.data();

    for (const auto& user : sessions_.users()) {
        for (auto* session : sessions_.sessions_for(user.uid)) {
            protocol::Frame frame{
                .version = protocol::kVersion,
                .type = protocol::MessageType::user_status,
                .request_id = 0,
                .body = encoded_body,
            };
            if (session->queue(std::move(frame))) {
                update_interest(*session);
            }
        }
    }
}

protocol::Frame Server::welcome_frame(
    const ClientSession& session, std::uint64_t request_id) const {
    protocol::BodyWriter body;
    body.u32(checked_uid(session.peer().uid));
    body.u32(checked_pid(session.peer().pid));
    body.string(session.peer().username);
    body.string("0.1.0");
    return protocol::Frame{
        .version = protocol::kVersion,
        .type = protocol::MessageType::welcome,
        .request_id = request_id,
        .body = std::move(body).take(),
    };
}

protocol::Frame Server::user_list_frame(std::uint64_t request_id) const {
    const auto users = sessions_.users();
    protocol::BodyWriter body;
    body.u32(static_cast<std::uint32_t>(users.size()));
    for (const auto& user : users) {
        body.u32(checked_uid(user.uid));
        body.u8(user.online ? 1U : 0U);
        body.string(user.username);
    }
    return protocol::Frame{
        .version = protocol::kVersion,
        .type = protocol::MessageType::user_list,
        .request_id = request_id,
        .body = std::move(body).take(),
    };
}

} // namespace ipcm::server
