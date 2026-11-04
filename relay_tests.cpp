#include "protocol.hpp"
#include "relay.hpp"
#include "socket_utils.hpp"
#include "unique_socket.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>

namespace {

using nexus::net::FlushResult;
using nexus::net::OutputQueue;
using nexus::net::UniqueSocket;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void testQueueEnforcesLimit() {
    OutputQueue queue;
    const std::array<std::uint8_t, 8> first{0, 1, 2, 3, 4, 5, 6, 7};
    const std::array<std::uint8_t, 1> extra{8};

    expect(queue.append(first, first.size()),
           "queue rejected bytes that exactly fit its limit");
    expect(queue.pending() == first.size(), "queue reported wrong pending size");
    expect(!queue.append(extra, first.size()),
           "queue accepted bytes beyond its limit");
    expect(queue.pending() == first.size(),
           "failed append changed the queued byte count");
}

void testConsumedSpaceCanBeReused() {
    OutputQueue queue;
    const std::array<std::uint8_t, 8> first{0, 1, 2, 3, 4, 5, 6, 7};
    const std::array<std::uint8_t, 3> second{8, 9, 10};

    expect(queue.append(first, first.size()), "initial append failed");
    queue.consume(second.size());
    expect(queue.append(second, first.size()),
           "queue did not reuse consumed capacity");
    expect(queue.pending() == first.size(), "reused queue has wrong size");
}

void testEncodedFrameRespectsLimit() {
    OutputQueue queue;
    const nexus::protocol::Frame frame{
        nexus::protocol::MessageType::Open, 1, {}};

    expect(!nexus::net::queueFrame(
               queue, frame, nexus::protocol::kFrameHeaderSize - 1),
           "encoded frame exceeded queue limit");
    expect(queue.empty(), "rejected frame left bytes behind");
    expect(nexus::net::queueFrame(
               queue, frame, nexus::protocol::kFrameHeaderSize),
           "exact-size frame was rejected");
}

void testFlushDeliversQueuedBytes() {
    int descriptors[2]{};
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
           "socketpair creation failed");
    UniqueSocket sender{descriptors[0]};
    UniqueSocket receiver{descriptors[1]};
    expect(nexus::net::setNonBlocking(sender.get()),
           "failed to make sender non-blocking");

    OutputQueue queue;
    const std::array<std::uint8_t, 5> expected{0, 1, 0, 2, 255};
    expect(queue.append(expected, expected.size()), "queue append failed");
    expect(nexus::net::flushOutput(sender.get(), queue) == FlushResult::Ready,
           "flush reported a socket failure");
    expect(queue.empty(), "flush left bytes queued");

    std::array<std::uint8_t, 5> actual{};
    const ssize_t received = recv(receiver.get(), actual.data(), actual.size(), 0);
    expect(received == static_cast<ssize_t>(actual.size()),
           "receiver got the wrong byte count");
    expect(actual == expected, "flush changed binary payload bytes");
}

} // namespace

int main() {
    try {
        testQueueEnforcesLimit();
        testConsumedSpaceCanBeReused();
        testEncodedFrameRespectsLimit();
        testFlushDeliversQueuedBytes();
    } catch (const std::exception& error) {
        std::cerr << "Relay test failed: " << error.what() << std::endl;
        return 1;
    }

    std::cout << "All relay tests passed" << std::endl;
    return 0;
}
