#include "heartbeat.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void testRoundTrip() {
  constexpr std::uint64_t expected = 0x0102030405060708ULL;
  const auto payload = nexus::reliability::encodeHeartbeatToken(expected);
  expect(payload.size() == nexus::reliability::kHeartbeatPayloadSize,
         "heartbeat payload has the wrong size");
  expect(payload.front() == 0x01 && payload.back() == 0x08,
         "heartbeat token is not in network byte order");

  std::uint64_t actual = 0;
  expect(nexus::reliability::decodeHeartbeatToken(payload, actual),
         "valid heartbeat payload did not decode");
  expect(actual == expected, "heartbeat token changed during round trip");
}

void testWrongSizeIsRejected() {
  const std::uint8_t byte = 0;
  std::uint64_t token = 123;
  expect(!nexus::reliability::decodeHeartbeatToken(
             std::span<const std::uint8_t>(&byte, 1), token),
         "short heartbeat payload was accepted");
  expect(token == 123, "failed decoding changed the output token");
}

} // namespace

int main() {
  try {
    testRoundTrip();
    testWrongSizeIsRejected();
  } catch (const std::exception &error) {
    std::cerr << "Heartbeat test failed: " << error.what() << std::endl;
    return 1;
  }

  std::cout << "All heartbeat tests passed" << std::endl;
  return 0;
}
