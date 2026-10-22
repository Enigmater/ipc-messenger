#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ipcm::protocol
{

    // Заголовок пакета. 4 байта = IPCM
    inline constexpr std::uint32_t kMagic = 0x4950434dU; // "IPCM"
    
    // Версия протокола
    inline constexpr std::uint16_t kVersion = 1;
    
    /*  Структура заголовка
        ┌──────────┬────────┬───────────┬────────────┬────────────┐
        │  "IPCM"  │ Версия │ Тип       │ Размер тела│ Request ID │
        │ 4 байта  │ 2 байта│ 2 байта   │ 4 байта    │ 8 байт     │
        └──────────┴────────┴───────────┴────────────┴────────────┘
    */
    inline constexpr std::size_t kHeaderSize = 20;
    inline constexpr std::uint32_t kDefaultMaximumBodySize = 64U * 1024U;

    enum class MessageType : std::uint16_t
    {
        hello = 1,
        welcome = 2,
        list_users = 3,
        user_list = 4,
        send_message = 5,
        incoming_message = 6,
        message_accepted = 7,
        user_status = 8,
        ping = 9,
        pong = 10,
        error = 11,
    };

    enum class ErrorCode : std::uint16_t
    {
        invalid_frame = 1,
        protocol_mismatch = 2,
        unexpected_message = 3,
        invalid_message = 4,
        user_not_found = 5,
        user_offline = 6,
        message_too_large = 7,
        server_busy = 8,
    };

    enum class DeliveryState : std::uint8_t
    {
        accepted = 1,
        delivered = 2,
    };

    struct Frame
    {
        std::uint16_t version{kVersion};
        MessageType type{};
        std::uint64_t request_id{};
        std::vector<std::byte> body;
    };

    class ProtocolError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    class BodyWriter
    {
    public:
        void u8(std::uint8_t value);
        void u16(std::uint16_t value);
        void u32(std::uint32_t value);
        void u64(std::uint64_t value);
        void string(std::string_view value);

        [[nodiscard]] const std::vector<std::byte> &data() const noexcept;
        [[nodiscard]] std::vector<std::byte> take() && noexcept;

    private:
        std::vector<std::byte> data_;
    };

    class BodyReader
    {
    public:
        explicit BodyReader(std::span<const std::byte> data) noexcept;

        [[nodiscard]] std::uint8_t u8();
        [[nodiscard]] std::uint16_t u16();
        [[nodiscard]] std::uint32_t u32();
        [[nodiscard]] std::uint64_t u64();
        [[nodiscard]] std::string string();
        void require_finished() const;

    private:
        [[nodiscard]] std::span<const std::byte> take(std::size_t count);

        std::span<const std::byte> data_;
        std::size_t position_{0};
    };

    class FrameParser
    {
    public:
        explicit FrameParser(std::uint32_t maximum_body_size = kDefaultMaximumBodySize);

        void append(std::span<const std::byte> bytes);
        [[nodiscard]] std::optional<Frame> next();
        [[nodiscard]] std::size_t buffered_bytes() const noexcept;

    private:
        void compact();

        std::uint32_t maximum_body_size_;
        std::vector<std::byte> buffer_;
        std::size_t position_{0};
    };

    [[nodiscard]] std::vector<std::byte> encode_frame(const Frame &frame);
    [[nodiscard]] bool is_known_message_type(MessageType type) noexcept;

} // namespace ipcm::protocol
