#include "nexus/heartbeat.hpp"

namespace nexus::reliability {

std::vector<std::uint8_t> encodeHeartbeatToken(std::uint64_t token) {
  std::vector<std::uint8_t> payload(kHeartbeatPayloadSize);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    const std::size_t shift = (payload.size() - 1 - index) * 8;
    payload[index] = static_cast<std::uint8_t>((token >> shift) & 0xFF);
  }
  return payload;
}

bool decodeHeartbeatToken(std::span<const std::uint8_t> payload,
                          std::uint64_t &token) {
  if (payload.size() != kHeartbeatPayloadSize) {
    return false;
  }

  std::uint64_t decoded = 0;
  for (const std::uint8_t byte : payload) {
    decoded = (decoded << 8) | byte;
  }
  token = decoded;
  return true;
}

} // namespace nexus::reliability
