#include "server/session_registry.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace ipcm::server {

void SessionRegistry::add(std::unique_ptr<ClientSession> session) {
    const int fd = session->fd();
    if (!by_fd_.emplace(fd, std::move(session)).second) {
        throw std::logic_error("duplicate client file descriptor");
    }
}

std::unique_ptr<ClientSession> SessionRegistry::remove(int fd) {
    const auto iterator = by_fd_.find(fd);
    if (iterator == by_fd_.end()) {
        return nullptr;
    }

    ClientSession* session = iterator->second.get();
    if (session->is_activated()) {
        auto active = active_by_uid_.find(session->peer().uid);
        if (active != active_by_uid_.end()) {
            std::erase(active->second, session);
            if (active->second.empty()) {
                active_by_uid_.erase(active);
            }
        }
    }

    auto result = std::move(iterator->second);
    by_fd_.erase(iterator);
    return result;
}

ClientSession* SessionRegistry::find(int fd) noexcept {
    const auto iterator = by_fd_.find(fd);
    return iterator == by_fd_.end() ? nullptr : iterator->second.get();
}

std::vector<ClientSession*> SessionRegistry::sessions_for(uid_t uid) const {
    const auto iterator = active_by_uid_.find(uid);
    return iterator == active_by_uid_.end() ? std::vector<ClientSession*>{} : iterator->second;
}

std::vector<KnownUser> SessionRegistry::users() const {
    std::vector<KnownUser> result;
    result.reserve(known_users_.size());
    for (const auto& [uid, username] : known_users_) {
        result.push_back(KnownUser{
            .uid = uid,
            .username = username,
            .online = online(uid),
        });
    }
    std::ranges::sort(result, {}, &KnownUser::username);
    return result;
}

bool SessionRegistry::knows(uid_t uid) const noexcept {
    return known_users_.contains(uid);
}

bool SessionRegistry::online(uid_t uid) const noexcept {
    const auto iterator = active_by_uid_.find(uid);
    return iterator != active_by_uid_.end() && !iterator->second.empty();
}

std::size_t SessionRegistry::size() const noexcept {
    return by_fd_.size();
}

bool SessionRegistry::activate(ClientSession& session) {
    const auto uid = session.peer().uid;
    const bool first_session = !online(uid);
    known_users_.insert_or_assign(uid, session.peer().username);
    active_by_uid_[uid].push_back(&session);
    session.activate();
    return first_session;
}

} // namespace ipcm::server
