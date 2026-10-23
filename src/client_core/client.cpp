#include "client_core/client.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <chrono>

namespace ipcm::client
{
    using namespace protocol;
    namespace
    {
        // Возвращает время (timestamp) в миллисекундах
        std::uint64_t now()
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::system_clock::now().time_since_epoch())
                                                  .count());
        }

        // История ограничена, чтобы длительный сеанс не накапливал сообщения без конца.
        void remember(User &user, Message message)
        {
            if (user.history.size() >= 1000)
                user.history.pop_front();
            user.history.push_back(std::move(message));
        }
    }

    Client::Client(const std::string &path) : started_(std::chrono::steady_clock::now())
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;

        // Проверям путь до сокета
        if (path.empty() || path.size() >= sizeof(address.sun_path))
            throw std::runtime_error("Invalid socket path");

        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

        // Создание UNIX-сокета (fd - file descriptor)
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0)
            throw std::runtime_error(std::strerror(errno));

        // Подключение к сокету указаного в path
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
        {
            const std::string error = "Cannot connect to " + path + ": " + std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(error);
        }

        // HELLO только ставится в очередь; фактическая отправка произойдёт в update().
        BodyWriter body;
        body.string("messenger-tui");
        body.string("0.1.0");
        queue(MessageType::hello, std::move(body), 1);
    }

    Client::~Client()
    {
        // Закрытие сокета
        if (fd_ >= 0)
            ::close(fd_);
    }

    void Client::queue(MessageType type, BodyWriter body, std::uint64_t id)
    {
        // Ограничиваем очередь: если сервер не читает сокет, память не растёт без конца.
        auto bytes = encode_frame({kVersion, type, id, std::move(body).take()});
        if (queued_ + bytes.size() > 1024 * 1024)
            throw std::runtime_error("Outgoing queue is full");
        queued_ += bytes.size();
        output_.push_back(std::move(bytes));
    }

    void Client::refresh()
    {
        if (ready)
            queue(MessageType::list_users);
    }

    void Client::send(std::uint32_t recipient, const std::string &text)
    {
        if (!ready || !connected)
            throw std::runtime_error("Not connected");
        if (text.empty())
            throw std::runtime_error("Message is empty");
        // Ограничение проверяется по будущему INCOMING_MESSAGE: кроме текста
        // в нём будут имя отправителя и 28 байт фиксированных полей и длин строк.
        if (text.size() + name.size() + 28 > kDefaultMaximumBodySize)
            throw std::runtime_error("Message is too large");
        const auto id = next_++;
        BodyWriter body;
        body.u32(recipient);
        body.u64(id);
        body.string(text);
        queue(MessageType::send_message, std::move(body), id);
        // Показываем сообщение сразу, а результат отправки уточним по ответу сервера.
        pending_[id] = recipient;
        remember(users[recipient], {id, now(), name, text, "sending", true, 0});
    }
    void Client::lost(const std::string &reason)
    {
        connected = ready = false;
        status = "Disconnected: " + reason;
        // Без ответа нельзя отличить недоставленное сообщение от принятого сервером
        // перед обрывом. Не помечаем его ошибкой доставки и не повторяем автоматически.
        for (auto &[id, user] : pending_)
            for (auto &message : users[user].history)
                if (message.outgoing && message.id == id)
                    message.state = "unknown: connection lost";
        pending_.clear();
        output_.clear();
        queued_ = offset_ = 0;
    }
    void Client::update()
    {
        if (!connected)
            return;
        try
        {
            // Таймаут действует до WELCOME, а не на всю продолжительность соединения.
            if (!ready && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(5))
                throw std::runtime_error("Handshake timed out");
            // Ограничиваем работу за один вызов, чтобы UI успевал обрабатывать ввод.
            std::size_t budget = 65536;
            while (!output_.empty() && budget)
            {
                auto &bytes = output_.front();
                // send() может передать только часть кадра. MSG_NOSIGNAL позволяет
                // обработать обрыв как ошибку, не завершая процесс сигналом SIGPIPE.
                const auto count = ::send(fd_, bytes.data() + offset_,
                                          std::min(bytes.size() - offset_, budget), MSG_NOSIGNAL);
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    // Сокет пока не готов к записи — продолжим с offset_ в следующем update().
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    throw std::runtime_error(std::strerror(errno));
                }
                if (!count)
                    throw std::runtime_error("Socket write failed");
                const auto size = static_cast<std::size_t>(count);
                offset_ += size;
                budget -= size;
                if (offset_ == bytes.size())
                {
                    queued_ -= bytes.size();
                    output_.pop_front();
                    offset_ = 0;
                }
            }
            std::array<std::byte, 16384> buffer{};
            // Чтение тоже ограничено, иначе непрерывный входящий поток вытеснит UI.
            for (int i = 0; i < 16; ++i)
            {
                const auto count = ::recv(fd_, buffer.data(), buffer.size(), 0);
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    throw std::runtime_error(std::strerror(errno));
                }
                if (!count)
                {
                    lost("Disconnected by server");
                    return;
                }
                // SOCK_STREAM не сохраняет границы кадров: recv() может вернуть
                // часть кадра или несколько сразу. Parser хранит незавершённый хвост.
                parser_.append(std::span(buffer).first(static_cast<std::size_t>(count)));
                while (auto frame = parser_.next())
                    handle(*frame);
            }
        }
        catch (const std::exception &error)
        {
            lost(error.what());
        }
    }
    void Client::handle(const Frame &frame)
    {
        if (frame.version != kVersion)
            throw ProtocolError("Unsupported server protocol");
        if (!ready && frame.type != MessageType::welcome && frame.type != MessageType::error)
            throw ProtocolError("Expected WELCOME");
        BodyReader body(frame.body);
        switch (frame.type)
        {
        case MessageType::welcome:
            if (ready)
                throw ProtocolError("Duplicate WELCOME");
            uid = body.u32();
            (void)body.u32(); // PID, подтверждённый сервером, сейчас не нужен UI.
            name = body.string();
            (void)body.string(); // Версия приложения сервера; версия протокола — в заголовке.
            ready = true;
            status = "Connected as " + name;
            refresh();
            break;
        case MessageType::user_list:
        {
            auto count = body.u32();
            // Одна запись занимает минимум 9 байт: UID, online и длина имени.
            if (count > frame.body.size() / 9)
                throw ProtocolError("Invalid user count");
            while (count--)
            {
                const auto id = body.u32();
                const bool online = body.u8() != 0;
                auto username = body.string();
                // Обновляем реквизиты на месте, сохраняя историю и непрочитанные сообщения.
                users[id].name = std::move(username);
                users[id].online = online;
            }
            break;
        }
        case MessageType::user_status:
        {
            const auto id = body.u32();
            const bool online = body.u8() != 0;
            // USER_STATUS содержит только UID и online. Имя нового пользователя
            // можно получить лишь отдельным запросом LIST_USERS.
            if (!users.contains(id))
                refresh();
            else
                users[id].online = online;
            break;
        }
        case MessageType::incoming_message:
        {
            const auto id = body.u64();
            const auto sender = body.u32();
            auto username = body.string();
            const auto timestamp = body.u64();
            auto text = body.string();
            auto &user = users[sender];
            user.name = username;
            user.online = true;
            // При отправке себе сервер присылает и эхо, и подтверждение отправки.
            // Если подтверждение уже пришло, обновляем исходящую запись вместо дубля.
            // Сравниваем серверные ID: одинаковый текст или UID не означает одно сообщение.
            auto own = std::find_if(user.history.begin(), user.history.end(), [&](const Message &message)
                                    { return message.outgoing && message.server_id == id; });
            if (sender == uid && own != user.history.end())
            {
                own->timestamp = timestamp;
            }
            else
            {
                ++user.unread;
                remember(user, {id, timestamp, std::move(username), std::move(text), "received", false, id});
            }
            break;
        }
        case MessageType::message_accepted:
        {
            const auto id = body.u64();
            const auto server_id = body.u64();
            const auto state = body.u8();
            if (state != 1 && state != 2)
                throw ProtocolError("Invalid delivery state");
            if (pending_.contains(id))
            {
                const auto recipient = pending_.at(id);
                auto &user = users[recipient];
                // История могла уже вытеснить ожидающее сообщение из-за лимита.
                auto own = std::find_if(user.history.begin(), user.history.end(), [&](const Message &message)
                                        { return message.outgoing && message.id == id; });
                if (own != user.history.end())
                {
                    own->server_id = server_id;
                    // Состояние 2 подтверждает очередь активной сессии, а не прочтение.
                    own->state = state == 2 ? "queued to recipient" : "accepted";
                    if (recipient == uid)
                    {
                        // Обратный порядок: эхо пришло раньше подтверждения и пока
                        // хранится как входящее. Переносим его время и удаляем только
                        // запись с тем же server_id, сохраняя сообщения других сеансов.
                        for (const auto &echo : user.history)
                            if (!echo.outgoing && echo.server_id == server_id)
                                own->timestamp = echo.timestamp;
                        auto removed = std::erase_if(user.history, [&](const Message &message)
                                                     { return !message.outgoing && message.server_id == server_id; });
                        // UI мог уже обнулить счётчик, поэтому вычитаем с ограничением.
                        user.unread -= std::min(user.unread, static_cast<unsigned>(removed));
                    }
                }
                pending_.erase(id);
            }
            break;
        }
        case MessageType::error:
        {
            (void)body.u16();
            status = body.string();
            if (!ready)
                throw ProtocolError(status);
            // ERROR связывается с отправкой через request_id заголовка,
            // поскольку в теле ошибки нет локального ID сообщения.
            if (pending_.contains(frame.request_id))
            {
                for (auto &message : users[pending_.at(frame.request_id)].history)
                    if (message.outgoing && message.id == frame.request_id)
                        message.state = "error: " + status;
                pending_.erase(frame.request_id);
            }
            break;
        }
        case MessageType::pong:
            break;
        default:
            throw ProtocolError("Unexpected server frame");
        }
        // Лишние байты означают несоответствие формату, даже если нужные поля прочитаны.
        body.require_finished();
    }
}
