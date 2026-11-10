#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::protocol {

// These values come from protocol.md and must match on the client and edge.
inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 12;
inline constexpr std::uint32_t kMaxPayloadSize = 65'536;
inline constexpr std::size_t kMinAuthTokenSize = 32;
inline constexpr std::size_t kMaxAuthTokenSize = 256;

enum class MessageType : std::uint8_t {
    Register = 1,
    RegisterOk = 2,
    Open = 3,
    Data = 4,
    Close = 5,
    Ping = 6,
    Pong = 7,
    Authenticate = 8,
    AuthenticateOk = 9,
    AuthenticateFailed = 10,
};

// Frame stores normal C++ values. encodeFrame() converts them to wire bytes.
struct Frame {
    MessageType type{MessageType::Data};
    std::uint32_t streamId{};
    std::vector<std::uint8_t> payload;
};

enum class DecodeStatus {
    NeedMoreData,
    FrameReady,
    ProtocolError,
};

// Encodes one complete frame. Invalid fields leave output empty and return false.
bool encodeFrame(const Frame& frame, std::vector<std::uint8_t>& output);

class FrameDecoder {
public:
    // Keeps new bytes after any incomplete bytes from an earlier socket read.
    void append(std::span<const std::uint8_t> bytes);

    // Returns one frame at a time, even when several arrived in one socket read.
    DecodeStatus nextFrame(Frame& output);

    // This explains the last ProtocolError without exposing the input buffer.
    std::string_view error() const;

private:
    std::vector<std::uint8_t> buffer_;
    std::string error_;
};

} // namespace nexus::protocol
