#include "nexus/flow_control.hpp"

#include <algorithm>

namespace nexus::flow {

SendWindow::SendWindow(std::size_t capacity)
    : capacity_(capacity), available_(capacity) {}

std::size_t SendWindow::available() const {
    return available_;
}

std::size_t SendWindow::allowedRead(std::size_t requested) const {
    return std::min(requested, available_);
}

bool SendWindow::consume(std::size_t count) {
    if (count == 0 || count > available_) {
        return false;
    }
    available_ -= count;
    return true;
}

bool SendWindow::replenish(std::size_t count) {
    if (count == 0 || count > capacity_ - available_) {
        return false;
    }
    available_ += count;
    return true;
}

std::vector<std::uint8_t> encodeWindowIncrement(std::uint32_t increment) {
    if (increment == 0) {
        return {};
    }
    return {
        static_cast<std::uint8_t>((increment >> 24) & 0xFF),
        static_cast<std::uint8_t>((increment >> 16) & 0xFF),
        static_cast<std::uint8_t>((increment >> 8) & 0xFF),
        static_cast<std::uint8_t>(increment & 0xFF),
    };
}

bool decodeWindowIncrement(std::span<const std::uint8_t> payload,
                           std::uint32_t& increment) {
    if (payload.size() != 4) {
        return false;
    }
    increment = (static_cast<std::uint32_t>(payload[0]) << 24) |
                (static_cast<std::uint32_t>(payload[1]) << 16) |
                (static_cast<std::uint32_t>(payload[2]) << 8) |
                static_cast<std::uint32_t>(payload[3]);
    return increment != 0;
}

} // namespace nexus::flow
