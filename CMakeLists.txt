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

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol-tests tests/protocol_tests.cpp)
    target_link_libraries(protocol-tests PRIVATE ipcm_common)
    target_compile_options(protocol-tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME protocol-tests COMMAND protocol-tests)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME server-integration-test
            COMMAND ${CMAKE_COMMAND} -E env
                IPCM_SERVER=$<TARGET_FILE:ipcm-server>
                ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/server_integration_test.py
        )
    endif()
endif()
