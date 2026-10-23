#pragma once

#include <string>

namespace ipcm::server {

class UnixListener {
public:
    UnixListener(std::string path, unsigned int mode);
    ~UnixListener();

    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] int accept() const;

private:
    void prepare_path();

    std::string path_;
    int fd_{-1};
    bool owns_path_{false};
};

} // namespace ipcm::server
