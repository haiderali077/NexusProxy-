#pragma once

#include "protocol.hpp"

#include <cstddef>

namespace nexus::limits {

// Keep capacity decisions in one place so the client and edge enforce the
// same memory and connection boundaries.
inline constexpr std::size_t kMaxConcurrentTunnels = 1;
inline constexpr std::size_t kMaxStreamsPerTunnel = 64;
inline constexpr std::size_t kReadBufferBytes = 16 * 1024;
inline constexpr std::size_t kMaxStreamQueueBytes = 256 * 1024;
inline constexpr std::size_t kMaxTunnelQueueBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kTunnelControlReserveBytes = 64 * 1024;
inline constexpr std::size_t kMaxFramePayloadBytes =
    protocol::kMaxPayloadSize;

static_assert(kReadBufferBytes <= kMaxFramePayloadBytes);

} // namespace nexus::limits
