#include "protocol.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nexus::protocol::DecodeStatus;
using nexus::protocol::Frame;
using nexus::protocol::FrameDecoder;
using nexus::protocol::MessageType;
using nexus::protocol::encodeFrame;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<std::uint8_t> encode(const Frame& frame) {
    std::vector<std::uint8_t> bytes;
    expect(encodeFrame(frame, bytes), "expected frame to encode");
    return bytes;
}

void expectSameFrame(const Frame& actual, const Frame& expected) {
    expect(actual.type == expected.type, "message type changed");
    expect(actual.streamId == expected.streamId, "stream ID changed");
    expect(actual.payload == expected.payload, "payload changed");
}

void writeUint32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                 std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

void testEmptyPayload() {
    const Frame expected{MessageType::Register, 0, {}};
    const auto bytes = encode(expected);
    expect(bytes.size() == nexus::protocol::kFrameHeaderSize,
           "empty frame should contain only its header");

    FrameDecoder decoder;
    decoder.append(bytes);
    Frame actual;
    expect(decoder.nextFrame(actual) == DecodeStatus::FrameReady,
           "empty frame was not decoded");
    expectSameFrame(actual, expected);
}

void testBinaryPayload() {
    const Frame expected{
        MessageType::Data, 42, {0x00, 0x01, 0x7F, 0x80, 0xFF}};

    FrameDecoder decoder;
    decoder.append(encode(expected));
    Frame actual;
    expect(decoder.nextFrame(actual) == DecodeStatus::FrameReady,
           "binary frame was not decoded");
    expectSameFrame(actual, expected);
}

void testSplitFrame() {
    const Frame expected{MessageType::Data, 7, {'h', 'e', 'l', 'l', 'o'}};
    const auto bytes = encode(expected);
    FrameDecoder decoder;
    Frame actual;

    decoder.append(std::span<const std::uint8_t>(bytes.data(), 5));
    expect(decoder.nextFrame(actual) == DecodeStatus::NeedMoreData,
           "partial header should need more data");

    decoder.append(std::span<const std::uint8_t>(bytes.data() + 5, 8));
    expect(decoder.nextFrame(actual) == DecodeStatus::NeedMoreData,
           "partial payload should need more data");

    decoder.append(std::span<const std::uint8_t>(bytes.data() + 13,
                                                 bytes.size() - 13));
    expect(decoder.nextFrame(actual) == DecodeStatus::FrameReady,
           "completed split frame was not decoded");
    expectSameFrame(actual, expected);
}

void testSeveralFramesInOneRead() {
    const Frame first{MessageType::Open, 11, {}};
    const Frame second{MessageType::Data, 11, {'o', 'k'}};
    auto combined = encode(first);
    const auto secondBytes = encode(second);
    combined.insert(combined.end(), secondBytes.begin(), secondBytes.end());

    FrameDecoder decoder;
    decoder.append(combined);
    Frame actual;
    expect(decoder.nextFrame(actual) == DecodeStatus::FrameReady,
           "first combined frame was not decoded");
    expectSameFrame(actual, first);
    expect(decoder.nextFrame(actual) == DecodeStatus::FrameReady,
           "second combined frame was not decoded");
    expectSameFrame(actual, second);
    expect(decoder.nextFrame(actual) == DecodeStatus::NeedMoreData,
           "decoder should be empty after both frames");
}

void testInvalidVersion() {
    auto bytes = encode(Frame{MessageType::Open, 1, {}});
    bytes[0] = 2;

    FrameDecoder decoder;
    decoder.append(bytes);
    Frame output;
    expect(decoder.nextFrame(output) == DecodeStatus::ProtocolError,
           "unsupported version was accepted");
    expect(!decoder.error().empty(), "version error should explain the failure");
}

void testUnknownMessageType() {
    auto bytes = encode(Frame{MessageType::Open, 1, {}});
    bytes[1] = 99;

    FrameDecoder decoder;
    decoder.append(bytes);
    Frame output;
    expect(decoder.nextFrame(output) == DecodeStatus::ProtocolError,
           "unknown message type was accepted");
}

void testInvalidPayloadLength() {
    auto oversized = encode(Frame{MessageType::Data, 1, {0x01}});
    writeUint32(oversized, 8, nexus::protocol::kMaxPayloadSize + 1);

    FrameDecoder oversizedDecoder;
    oversizedDecoder.append(oversized);
    Frame output;
    expect(oversizedDecoder.nextFrame(output) == DecodeStatus::ProtocolError,
           "oversized payload length was accepted");

    auto wrongOpenLength = encode(Frame{MessageType::Open, 1, {}});
    writeUint32(wrongOpenLength, 8, 1);

    FrameDecoder openDecoder;
    openDecoder.append(wrongOpenLength);
    expect(openDecoder.nextFrame(output) == DecodeStatus::ProtocolError,
           "OPEN frame with a payload length was accepted");
}

void testHeaderEncoding() {
    const auto bytes = encode(Frame{MessageType::Data, 0x01020304, {0xAA}});
    expect(bytes[0] == nexus::protocol::kProtocolVersion,
           "wrong version byte");
    expect(bytes[1] == static_cast<std::uint8_t>(MessageType::Data),
           "wrong message type byte");
    expect(bytes[2] == 0 && bytes[3] == 0, "flags should be zero");
    expect(bytes[4] == 0x01 && bytes[5] == 0x02 &&
               bytes[6] == 0x03 && bytes[7] == 0x04,
           "stream ID is not in network byte order");
    expect(bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 1,
           "payload length is not in network byte order");
}

void testEncoderValidation() {
    std::vector<std::uint8_t> output{0xAA};
    expect(!encodeFrame(Frame{MessageType::Data, 0, {0x01}}, output),
           "DATA with stream 0 was encoded");
    expect(output.empty(), "failed encoding should clear its output");

    expect(!encodeFrame(Frame{MessageType::Open, 1, {0x01}}, output),
           "OPEN with a payload was encoded");
    expect(!encodeFrame(Frame{static_cast<MessageType>(99), 0, {}}, output),
           "unknown message type was encoded");

    const Frame tooLarge{MessageType::Data, 1,
                         std::vector<std::uint8_t>(
                             nexus::protocol::kMaxPayloadSize + 1)};
    expect(!encodeFrame(tooLarge, output), "oversized payload was encoded");
}

void testInvalidFlags() {
    auto bytes = encode(Frame{MessageType::Open, 1, {}});
    bytes[3] = 1;

    FrameDecoder decoder;
    decoder.append(bytes);
    Frame output;
    expect(decoder.nextFrame(output) == DecodeStatus::ProtocolError,
           "nonzero reserved flags were accepted");
}

void testAuthenticationFrames() {
    std::vector<std::uint8_t> token(nexus::protocol::kMinAuthTokenSize, 0xA5);
    token[0] = 0;
    const Frame expected{MessageType::Authenticate, 0, token};

    FrameDecoder decoder;
    decoder.append(encode(expected));
    Frame actual;
    expect(decoder.nextFrame(actual) == DecodeStatus::FrameReady,
           "AUTH frame was not decoded");
    expectSameFrame(actual, expected);

    std::vector<std::uint8_t> output;
    expect(!encodeFrame(Frame{MessageType::Authenticate, 0, {'s'}}, output),
           "short authentication token was encoded");
    expect(!encodeFrame(Frame{MessageType::AuthenticateOk, 0, {0x01}}, output),
           "AUTH_OK with a payload was encoded");
    expect(encodeFrame(Frame{MessageType::AuthenticateFailed, 0, {}}, output),
           "empty AUTH_FAILED frame was rejected");
}

} // namespace

int main() {
    try {
        testEmptyPayload();
        testBinaryPayload();
        testSplitFrame();
        testSeveralFramesInOneRead();
        testInvalidVersion();
        testUnknownMessageType();
        testInvalidPayloadLength();
        testHeaderEncoding();
        testEncoderValidation();
        testInvalidFlags();
        testAuthenticationFrames();
    } catch (const std::exception& error) {
        std::cerr << "Protocol test failed: " << error.what() << std::endl;
        return 1;
    }

    std::cout << "All protocol tests passed" << std::endl;
    return 0;
}
