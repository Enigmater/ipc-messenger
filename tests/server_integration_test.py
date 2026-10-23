#!/usr/bin/env python3

import os
import signal
import socket
import struct
import subprocess
import tempfile
import time


MAGIC = 0x4950434D
VERSION = 1
HEADER = struct.Struct("!IHHIQ")

HELLO = 1
WELCOME = 2
LIST_USERS = 3
USER_LIST = 4
SEND_MESSAGE = 5
INCOMING_MESSAGE = 6
MESSAGE_ACCEPTED = 7


def encoded_string(value: str) -> bytes:
    data = value.encode("utf-8")
    return struct.pack("!I", len(data)) + data


def frame(message_type: int, request_id: int, body: bytes = b"") -> bytes:
    return HEADER.pack(MAGIC, VERSION, message_type, len(body), request_id) + body


def receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            raise RuntimeError("server closed the connection")
        result.extend(part)
    return bytes(result)


def receive_frame(connection: socket.socket):
    header = receive_exact(connection, HEADER.size)
    magic, version, message_type, body_size, request_id = HEADER.unpack(header)
    assert magic == MAGIC
    assert version == VERSION
    return message_type, request_id, receive_exact(connection, body_size)


def receive_until(connection: socket.socket, expected_type: int):
    for _ in range(10):
        received = receive_frame(connection)
        if received[0] == expected_type:
            return received
    raise AssertionError(f"message type {expected_type} was not received")


def connect_client(socket_path: str, name: str) -> socket.socket:
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(2)
    connection.connect(socket_path)
    hello = encoded_string(name) + encoded_string("integration-test")
    connection.sendall(frame(HELLO, 1, hello))
    message_type, request_id, _ = receive_until(connection, WELCOME)
    assert message_type == WELCOME
    assert request_id == 1
    return connection


def main() -> None:
    server_binary = os.environ.get("IPCM_SERVER", "./build/ipcm-server")
    with tempfile.TemporaryDirectory(prefix="ipcm-test-") as directory:
        socket_path = os.path.join(directory, "server.sock")
        server = subprocess.Popen(
            [server_binary, "--socket", socket_path, "--log-level", "warning"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        first = None
        second = None
        try:
            for _ in range(100):
                if os.path.exists(socket_path):
                    break
                if server.poll() is not None:
                    raise RuntimeError(server.stderr.read())
                time.sleep(0.01)
            else:
                raise RuntimeError("server socket was not created")

            first = connect_client(socket_path, "test-client-1")
            second = connect_client(socket_path, "test-client-2")

            first.sendall(frame(LIST_USERS, 2))
            _, request_id, body = receive_until(first, USER_LIST)
            assert request_id == 2
            assert struct.unpack_from("!I", body)[0] == 1

            text = "hello over AF_UNIX"
            send_body = (
                struct.pack("!IQ", os.getuid(), 123)
                + encoded_string(text)
            )
            first.sendall(frame(SEND_MESSAGE, 3, send_body))

            _, _, incoming_body = receive_until(second, INCOMING_MESSAGE)
            assert text.encode("utf-8") in incoming_body
            _, request_id, accepted_body = receive_until(first, MESSAGE_ACCEPTED)
            assert request_id == 3
            client_message_id = struct.unpack_from("!Q", accepted_body)[0]
            assert client_message_id == 123
        finally:
            if first is not None:
                first.close()
            if second is not None:
                second.close()
            if server.poll() is None:
                server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)
            if server.returncode != 0:
                raise RuntimeError(server.stderr.read())

    print("server integration test passed")


if __name__ == "__main__":
    main()
