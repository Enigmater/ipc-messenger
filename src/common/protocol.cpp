#include "common/protocol.hpp"

#include <algorithm>
#include <limits>

namespace ipcm::protocol {
namespace {

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
}

std::uint16_t read_u16(std::span<const std::byte> input) {
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(input[0]) << 8U) |
        std::to_integer<std::uint16_t>(input[1]));
}

std::uint32_t read_u32(std::span<const std::byte> input) {
    std::uint32_t result = 0;
    for (const auto value : input.first<4>()) {
        result = (result << 8U) | std::to_integer<std::uint32_t>(value);
    }
    return result;
}

std::uint64_t read_u64(std::span<const std::byte> input) {
    std::uint64_t result = 0;
    for (const auto value : input.first<8>()) {
        result = (result << 8U) | std::to_integer<std::uint64_t>(value);
    }
    return result;
}

} // namespace

void BodyWriter::u8(std::uint8_t value) {
    data_.push_back(static_cast<std::byte>(value));
}

void BodyWriter::u16(std::uint16_t value) {
    append_u16(data_, value);
}

void BodyWriter::u32(std::uint32_t value) {
    append_u32(data_, value);
}

void BodyWriter::u64(std::uint64_t value) {
    append_u64(data_, value);
}

void BodyWriter::string(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("string is too large");
    }
    u32(static_cast<std::uint32_t>(value.size()));
    data_.insert(
        data_.end(),
        reinterpret_cast<const std::byte*>(value.data()),
        reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

const std::vector<std::byte>& BodyWriter::data() const noexcept {
    return data_;
}

std::vector<std::byte> BodyWriter::take() && noexcept {
    return std::move(data_);
}

BodyReader::BodyReader(std::span<const std::byte> data) noexcept : data_(data) {}

std::span<const std::byte> BodyReader::take(std::size_t count) {
    if (count > data_.size() - position_) {
        throw ProtocolError("truncated message body");
    }
    const auto result = data_.subspan(position_, count);
    position_ += count;
    return result;
}

std::uint8_t BodyReader::u8() {
    return std::to_integer<std::uint8_t>(take(1)[0]);
}

std::uint16_t BodyReader::u16() {
    return read_u16(take(2));
}

std::uint32_t BodyReader::u32() {
    return read_u32(take(4));
}

std::uint64_t BodyReader::u64() {
    return read_u64(take(8));
}

std::string BodyReader::string() {
    const auto size = static_cast<std::size_t>(u32());
    const auto bytes = take(size);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void BodyReader::require_finished() const {
    if (position_ != data_.size()) {
        throw ProtocolError("unexpected trailing bytes in message body");
    }
}

FrameParser::FrameParser(std::uint32_t maximum_body_size)
    : maximum_body_size_(maximum_body_size) {}

void FrameParser::append(std::span<const std::byte> bytes) {
    if (position_ > 0 && (position_ >= buffer_.size() / 2U || position_ > maximum_body_size_)) {
        compact();
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameParser::next() {
    const auto available = buffer_.size() - position_;
    if (available < kHeaderSize) {
        return std::nullopt;
    }

    const auto header = std::span<const std::byte>(buffer_).subspan(position_, kHeaderSize);
    if (read_u32(header.subspan(0, 4)) != kMagic) {
        throw ProtocolError("invalid frame magic");
    }

    Frame frame;
    frame.version = read_u16(header.subspan(4, 2));
    frame.type = static_cast<MessageType>(read_u16(header.subspan(6, 2)));
    const auto body_size = read_u32(header.subspan(8, 4));
    frame.request_id = read_u64(header.subspan(12, 8));

    if (!is_known_message_type(frame.type)) {
        throw ProtocolError("unknown message type");
    }
    if (body_size > maximum_body_size_) {
        throw ProtocolError("frame body exceeds configured limit");
    }

    const auto full_size = kHeaderSize + static_cast<std::size_t>(body_size);
    if (available < full_size) {
        return std::nullopt;
    }

    const auto body_begin = buffer_.begin() + static_cast<std::ptrdiff_t>(position_ + kHeaderSize);
    frame.body.assign(body_begin, body_begin + static_cast<std::ptrdiff_t>(body_size));
    position_ += full_size;

    if (position_ == buffer_.size()) {
        buffer_.clear();
        position_ = 0;
    }
    return frame;
}

std::size_t FrameParser::buffered_bytes() const noexcept {
    return buffer_.size() - position_;
}

void FrameParser::compact() {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(position_));
    position_ = 0;
}

std::vector<std::byte> encode_frame(const Frame& frame) {
    if (frame.body.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("frame body is too large");
    }

    std::vector<std::byte> output;
    output.reserve(kHeaderSize + frame.body.size());
    append_u32(output, kMagic);
    append_u16(output, frame.version);
    append_u16(output, static_cast<std::uint16_t>(frame.type));
    append_u32(output, static_cast<std::uint32_t>(frame.body.size()));
    append_u64(output, frame.request_id);
    output.insert(output.end(), frame.body.begin(), frame.body.end());
    return output;
}

bool is_known_message_type(MessageType type) noexcept {
    switch (type) {
    case MessageType::hello:
    case MessageType::welcome:
    case MessageType::list_users:
    case MessageType::user_list:
    case MessageType::send_message:
    case MessageType::incoming_message:
    case MessageType::message_accepted:
    case MessageType::user_status:
    case MessageType::ping:
    case MessageType::pong:
    case MessageType::error:
        return true;
    }
    return false;
}

} // namespace ipcm::protocol
