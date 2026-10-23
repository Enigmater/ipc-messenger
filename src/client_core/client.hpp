#pragma once
#include "common/protocol.hpp"
#include <deque>
#include <chrono>
#include <map>

namespace ipcm::client {
struct Message {
    // У исходящего сообщения id локальный, у входящего — серверный.
    // Поэтому при поиске по id также проверяем outgoing.
    std::uint64_t id{}, timestamp{};
    std::string sender, text, state;
    bool outgoing{};
    // Общий ID для сопоставления серверного эха с исходящим сообщением.
    // До MESSAGE_ACCEPTED у исходящего сообщения равен 0.
    std::uint64_t server_id{};
};
struct User {
    std::string name;
    bool online{};
    unsigned unread{};
    std::deque<Message> history;
};
// Вызывается из одного потока: update() обслуживает неблокирующий сокет,
// send()/refresh() только ставят команды в очередь. Состояние читает тот же поток UI.
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
    // offset_ — отправленная часть первого кадра. queued_ учитывает полные
    // размеры всех кадров в очереди, включая частично отправленный первый.
    std::size_t offset_{}, queued_{};
    // Локальный ID сообщения одновременно служит request_id команды отправки.
    // 1 зарезервирован для HELLO.
    std::uint64_t next_{2};
    // Локальный ID -> UID получателя для отправок, ещё не получивших ответ.
    std::map<std::uint64_t, std::uint32_t> pending_;
    // Монотонное время для таймаута рукопожатия: перевод системных часов не влияет.
    std::chrono::steady_clock::time_point started_;
};
}
