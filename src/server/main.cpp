#include "server/configuration.hpp"
#include "server/server.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

std::atomic_bool stop_requested{false};

extern "C" void handle_signal(int) {
    // Обработчик лишь запрашивает остановку; сокеты и сессии освобождаются
    // обычными деструкторами после выхода из цикла сервера.
    stop_requested.store(true, std::memory_order_relaxed);
}

bool help_requested(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (help_requested(argc, argv)) {
        std::cout << ipcm::server::Configuration::help(argv[0]);
        return 0;
    }

    try {
        const auto configuration = ipcm::server::Configuration::parse(argc, argv);

        struct sigaction action {};
        action.sa_handler = handle_signal;
        sigemptyset(&action.sa_mask);
        // Без SA_RESTART: сигнал должен прервать epoll_wait(), чтобы цикл
        // заметил stop_requested даже при отсутствии активности клиентов.
        action.sa_flags = 0;
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);

        ipcm::server::Server server(configuration, stop_requested);
        server.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ipcm-server: " << error.what() << '\n';
        std::cerr << ipcm::server::Configuration::help(argv[0]);
        return 1;
    }
}
