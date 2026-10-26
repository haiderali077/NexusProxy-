#include "relay.hpp"

#include <cerrno>
#include <sys/socket.h>

namespace nexus::net {

bool OutputQueue::empty() const {
    return offset_ == bytes_.size();
}

std::size_t OutputQueue::pending() const {
    return bytes_.size() - offset_;
}

bool OutputQueue::canAppend(std::size_t count, std::size_t limit) const {
    return count <= limit && pending() <= limit - count;
}

bool OutputQueue::append(std::span<const std::uint8_t> data,
                         std::size_t limit) {
    if (!canAppend(data.size(), limit)) {
        return false;
    }
    if (empty()) {
        clear();
    } else if (bytes_.size() + data.size() > limit && offset_ != 0) {
        bytes_.erase(bytes_.begin(), bytes_.begin() + offset_);
        offset_ = 0;
    }
    bytes_.insert(bytes_.end(), data.begin(), data.end());
    return true;
}

void OutputQueue::consume(std::size_t count) {
    offset_ += count;
    if (empty()) {
        clear();
    } else if (offset_ >= 4096 && offset_ * 2 >= bytes_.size()) {
        bytes_.erase(bytes_.begin(), bytes_.begin() + offset_);
        offset_ = 0;
    }
}

void OutputQueue::clear() {
    bytes_.clear();
    offset_ = 0;
}

const std::uint8_t* OutputQueue::data() const {
    return bytes_.data() + offset_;
}

bool queueFrame(OutputQueue& output, const protocol::Frame& frame,
                std::size_t limit) {
    std::vector<std::uint8_t> encoded;
    return protocol::encodeFrame(frame, encoded) && output.append(encoded, limit);
}

FlushResult flushOutput(int socketFd, OutputQueue& output) {
    while (!output.empty()) {
        const ssize_t sent = send(socketFd, output.data(), output.pending(), 0);
        if (sent > 0) {
            output.consume(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent == 0) {
            return FlushResult::Closed;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return FlushResult::Ready;
        }
        return FlushResult::Error;
    }
    return FlushResult::Ready;
}

} // namespace nexus::net
