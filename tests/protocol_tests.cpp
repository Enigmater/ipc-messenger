#include "common/protocol.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

namespace protocol = ipcm::protocol;

void body_round_trip() {
    protocol::BodyWriter writer;
    writer.u8(7);
    writer.u16(0x1234);
    writer.u32(0x12345678);
    writer.u64(0x123456789abcdef0ULL);
    writer.string("hello");

    protocol::BodyReader reader(writer.data());
    assert(reader.u8() == 7);
    assert(reader.u16() == 0x1234);
    assert(reader.u32() == 0x12345678);
    assert(reader.u64() == 0x123456789abcdef0ULL);
    assert(reader.string() == "hello");
    reader.require_finished();
}

void fragmented_frame_round_trip() {
    protocol::BodyWriter body;
    body.string("ipcm-tui");
    body.string("0.1.0");
    const protocol::Frame expected{
        .version = protocol::kVersion,
        .type = protocol::MessageType::hello,
        .request_id = 42,
        .body = std::move(body).take(),
    };
    const auto bytes = protocol::encode_frame(expected);

    protocol::FrameParser parser;
    for (const auto byte : bytes) {
        parser.append(std::span(&byte, 1));
    }

    const auto actual = parser.next();
    assert(actual.has_value());
    assert(actual->version == expected.version);
    assert(actual->type == expected.type);
    assert(actual->request_id == expected.request_id);
    assert(actual->body == expected.body);
    assert(!parser.next().has_value());
}

void multiple_frames_round_trip() {
    const protocol::Frame ping{
        .version = protocol::kVersion,
        .type = protocol::MessageType::ping,
        .request_id = 1,
        .body = {},
    };
    const protocol::Frame list{
        .version = protocol::kVersion,
        .type = protocol::MessageType::list_users,
        .request_id = 2,
        .body = {},
    };
    auto bytes = protocol::encode_frame(ping);
    const auto second = protocol::encode_frame(list);
    bytes.insert(bytes.end(), second.begin(), second.end());

    protocol::FrameParser parser;
    parser.append(bytes);
    const auto first_result = parser.next();
    const auto second_result = parser.next();
    assert(first_result && first_result->type == protocol::MessageType::ping);
    assert(second_result && second_result->type == protocol::MessageType::list_users);
    assert(!parser.next());
}

void invalid_magic_is_rejected() {
    auto bytes = protocol::encode_frame(protocol::Frame{
        .version = protocol::kVersion,
        .type = protocol::MessageType::ping,
        .request_id = 1,
        .body = {},
    });
    bytes[0] = std::byte{0};

    protocol::FrameParser parser;
    parser.append(bytes);
    bool rejected = false;
    try {
        static_cast<void>(parser.next());
    } catch (const protocol::ProtocolError&) {
        rejected = true;
    }
    assert(rejected);
}

void oversized_body_is_rejected_from_header() {
    protocol::BodyWriter body;
    body.string(std::string(32, 'x'));
    const auto bytes = protocol::encode_frame(protocol::Frame{
        .version = protocol::kVersion,
        .type = protocol::MessageType::hello,
        .request_id = 1,
        .body = std::move(body).take(),
    });

    protocol::FrameParser parser(8);
    parser.append(bytes);
    bool rejected = false;
    try {
        static_cast<void>(parser.next());
    } catch (const protocol::ProtocolError&) {
        rejected = true;
    }
    assert(rejected);
}

int main() {
    body_round_trip();
    fragmented_frame_round_trip();
    multiple_frames_round_trip();
    invalid_magic_is_rejected();
    oversized_body_is_rejected_from_header();
    std::cout << "protocol tests passed\n";
}
