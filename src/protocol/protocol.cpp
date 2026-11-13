#include "nexus/protocol.hpp"

#include <algorithm>

namespace nexus::protocol {
namespace {

constexpr std::size_t kVersionOffset = 0;
constexpr std::size_t kTypeOffset = 1;
constexpr std::size_t kFlagsOffset = 2;
constexpr std::size_t kStreamIdOffset = 4;
constexpr std::size_t kPayloadLengthOffset = 8;

// Writing individual bytes avoids relying on the machine's integer byte order.
void appendUint16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    output.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendUint32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    output.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint16_t readUint16(const std::vector<std::uint8_t>& input,
                         std::size_t offset) {
    return (static_cast<std::uint16_t>(input[offset]) << 8) |
           static_cast<std::uint16_t>(input[offset + 1]);
}

std::uint32_t readUint32(const std::vector<std::uint8_t>& input,
                         std::size_t offset) {
    return (static_cast<std::uint32_t>(input[offset]) << 24) |
           (static_cast<std::uint32_t>(input[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(input[offset + 2]) << 8) |
           static_cast<std::uint32_t>(input[offset + 3]);
}

bool isKnownMessageType(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(MessageType::Register) &&
           value <= static_cast<std::uint8_t>(MessageType::AuthenticateFailed);
}

std::string_view validateFields(MessageType type, std::uint32_t streamId,
                                std::uint32_t payloadLength) {
    if (payloadLength > kMaxPayloadSize) {
        return "payload exceeds the maximum size";
    }

    switch (type) {
        case MessageType::Register:
        case MessageType::RegisterOk:
            if (streamId != 0) {
                return "registration frames must use stream 0";
            }
            if (payloadLength > 63) {
                return "tunnel names cannot exceed 63 bytes";
            }
            return {};

        case MessageType::Open:
        case MessageType::Close:
            if (streamId == 0) {
                return "stream frames require a nonzero stream ID";
            }
            if (payloadLength != 0) {
                return "OPEN and CLOSE frames must have an empty payload";
            }
            return {};

        case MessageType::Data:
            if (streamId == 0) {
                return "DATA frames require a nonzero stream ID";
            }
            if (payloadLength == 0) {
                return "DATA frames cannot have an empty payload";
            }
            return {};

        case MessageType::Ping:
        case MessageType::Pong:
            if (streamId != 0) {
                return "PING and PONG frames must use stream 0";
            }
            if (payloadLength != 8) {
                return "PING and PONG payloads must contain 8 bytes";
            }
            return {};

        case MessageType::Authenticate:
            if (streamId != 0) {
                return "AUTH must use stream 0";
            }
            if (payloadLength < kMinAuthTokenSize ||
                payloadLength > kMaxAuthTokenSize) {
                return "AUTH token length is invalid";
            }
            return {};

        case MessageType::AuthenticateOk:
        case MessageType::AuthenticateFailed:
            if (streamId != 0) {
                return "authentication responses must use stream 0";
            }
            if (payloadLength != 0) {
                return "authentication responses must have an empty payload";
            }
            return {};
    }

    return "unknown message type";
}

bool isValidTunnelName(std::span<const std::uint8_t> payload) {
    return std::all_of(payload.begin(), payload.end(), [](std::uint8_t byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9') || byte == '-';
    });
}

bool requiresTunnelNameValidation(MessageType type) {
    return type == MessageType::Register || type == MessageType::RegisterOk;
}

} // namespace

bool encodeFrame(const Frame& frame, std::vector<std::uint8_t>& output) {
    output.clear();

    const auto rawType = static_cast<std::uint8_t>(frame.type);
    if (!isKnownMessageType(rawType) || frame.payload.size() > kMaxPayloadSize) {
        return false;
    }

    const auto payloadLength = static_cast<std::uint32_t>(frame.payload.size());
    if (!validateFields(frame.type, frame.streamId, payloadLength).empty()) {
        return false;
    }
    if (requiresTunnelNameValidation(frame.type) &&
        !isValidTunnelName(frame.payload)) {
        return false;
    }

    output.reserve(kFrameHeaderSize + frame.payload.size());
    output.push_back(kProtocolVersion);
    output.push_back(rawType);
    appendUint16(output, 0); // Version 1 reserves the flags field.
    appendUint32(output, frame.streamId);
    appendUint32(output, payloadLength);
    output.insert(output.end(), frame.payload.begin(), frame.payload.end());
    return true;
}

void FrameDecoder::append(std::span<const std::uint8_t> bytes) {
    // More bytes cannot safely restore framing after a protocol error.
    if (!error_.empty()) {
        return;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

DecodeStatus FrameDecoder::nextFrame(Frame& output) {
    if (!error_.empty()) {
        return DecodeStatus::ProtocolError;
    }
    if (buffer_.size() < kFrameHeaderSize) {
        return DecodeStatus::NeedMoreData;
    }

    if (buffer_[kVersionOffset] != kProtocolVersion) {
        error_ = "unsupported protocol version";
        return DecodeStatus::ProtocolError;
    }

    const std::uint8_t rawType = buffer_[kTypeOffset];
    if (!isKnownMessageType(rawType)) {
        error_ = "unknown message type";
        return DecodeStatus::ProtocolError;
    }
    if (readUint16(buffer_, kFlagsOffset) != 0) {
        error_ = "reserved flags must be zero";
        return DecodeStatus::ProtocolError;
    }

    const auto type = static_cast<MessageType>(rawType);
    const std::uint32_t streamId = readUint32(buffer_, kStreamIdOffset);
    const std::uint32_t payloadLength =
        readUint32(buffer_, kPayloadLengthOffset);

    const std::string_view fieldError =
        validateFields(type, streamId, payloadLength);
    if (!fieldError.empty()) {
        error_ = fieldError;
        return DecodeStatus::ProtocolError;
    }

    const std::size_t frameSize =
        kFrameHeaderSize + static_cast<std::size_t>(payloadLength);
    if (buffer_.size() < frameSize) {
        return DecodeStatus::NeedMoreData;
    }

    const auto payloadBegin = buffer_.begin() + kFrameHeaderSize;
    const auto payloadEnd = buffer_.begin() + frameSize;
    if (requiresTunnelNameValidation(type) &&
        !isValidTunnelName(std::span<const std::uint8_t>(
            buffer_.data() + kFrameHeaderSize, payloadLength))) {
        error_ = "tunnel name contains an invalid character";
        return DecodeStatus::ProtocolError;
    }

    output.type = type;
    output.streamId = streamId;
    output.payload.assign(payloadBegin, payloadEnd);
    buffer_.erase(buffer_.begin(), payloadEnd);
    return DecodeStatus::FrameReady;
}

std::string_view FrameDecoder::error() const {
    return error_;
}

} // namespace nexus::protocol
