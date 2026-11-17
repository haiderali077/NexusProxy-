#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nexus::flow {

// Tracks how many bytes this endpoint may send for one stream. Credit returns
// only after the peer has written earlier bytes to its destination socket.
class SendWindow {
public:
    explicit SendWindow(std::size_t capacity);

    std::size_t available() const;
    std::size_t allowedRead(std::size_t requested) const;

    bool consume(std::size_t count);
    bool replenish(std::size_t count);

private:
    std::size_t capacity_{};
    std::size_t available_{};
};

// WINDOW_UPDATE stores its positive byte increment as one big-endian uint32.
std::vector<std::uint8_t> encodeWindowIncrement(std::uint32_t increment);
bool decodeWindowIncrement(std::span<const std::uint8_t> payload,
                           std::uint32_t& increment);

} // namespace nexus::flow
