#pragma once

#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nexus::net {

class OutputQueue {
public:
    bool empty() const;
    std::size_t pending() const;
    bool canAppend(std::size_t count, std::size_t limit) const;
    bool append(std::span<const std::uint8_t> data, std::size_t limit);
    void consume(std::size_t count);
    void clear();

    const std::uint8_t* data() const;

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t offset_{};
};

enum class FlushResult {
    Ready,
    Closed,
    Error,
};

bool queueFrame(OutputQueue& output, const protocol::Frame& frame,
                std::size_t limit);
FlushResult flushOutput(int socketFd, OutputQueue& output);

} // namespace nexus::net
