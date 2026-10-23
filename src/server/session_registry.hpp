#pragma once

#include "server/session.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ipcm::server {

struct KnownUser {
    uid_t uid{};
    std::string username;
    bool online{};
};

class SessionRegistry {
public:
    void add(std::unique_ptr<ClientSession> session);
    [[nodiscard]] std::unique_ptr<ClientSession> remove(int fd);

    [[nodiscard]] ClientSession* find(int fd) noexcept;
    [[nodiscard]] std::vector<ClientSession*> sessions_for(uid_t uid) const;
    [[nodiscard]] std::vector<KnownUser> users() const;
    [[nodiscard]] bool knows(uid_t uid) const noexcept;
    [[nodiscard]] bool online(uid_t uid) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // Returns true if this is the user's first active session.
    [[nodiscard]] bool activate(ClientSession& session);

private:
    std::unordered_map<int, std::unique_ptr<ClientSession>> by_fd_;
    std::unordered_map<uid_t, std::vector<ClientSession*>> active_by_uid_;
    std::unordered_map<uid_t, std::string> known_users_;
};

} // namespace ipcm::server
