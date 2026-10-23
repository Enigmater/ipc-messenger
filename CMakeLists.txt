cmake_minimum_required(VERSION 3.20)

project(IPCMessenger VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The current ipcm-server implementation requires Linux")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ipcm_common
    src/common/protocol.cpp
)
target_include_directories(ipcm_common PUBLIC src)
target_compile_options(ipcm_common PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_library(ipcm_client_core src/client_core/client.cpp)
target_link_libraries(ipcm_client_core PUBLIC ipcm_common)
target_compile_options(ipcm_client_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)
add_executable(messenger-tui src/tui/main.cpp)
target_include_directories(messenger-tui PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(messenger-tui PRIVATE ipcm_client_core ${CURSES_LIBRARIES})
target_compile_options(messenger-tui PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(ipcm-server
    src/server/configuration.cpp
    src/server/event_loop.cpp
    src/server/logger.cpp
    src/server/main.cpp
    src/server/peer_credentials.cpp
    src/server/server.cpp
    src/server/session.cpp
    src/server/session_registry.cpp
    src/server/unix_listener.cpp
)
target_link_libraries(ipcm-server PRIVATE ipcm_common)
target_compile_options(ipcm-server PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
