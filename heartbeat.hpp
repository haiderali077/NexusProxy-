#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nexus::reliability {

inline constexpr std::size_t kHeartbeatPayloadSize = 8;

std::vector<std::uint8_t> encodeHeartbeatToken(std::uint64_t token);
bool decodeHeartbeatToken(std::span<const std::uint8_t> payload,
                          std::uint64_t &token);

} // namespace nexus::reliability
