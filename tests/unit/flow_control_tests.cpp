#include "nexus/flow_control.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void testSendWindowAccounting() {
    nexus::flow::SendWindow window{100};
    expect(window.available() == 100, "window did not start at capacity");
    expect(window.allowedRead(40) == 40, "window limited a valid read");
    expect(window.allowedRead(120) == 100, "window did not clamp a read");

    expect(window.consume(60), "window rejected available credit");
    expect(window.available() == 40, "consume did not reduce credit");
    expect(!window.consume(41), "window allowed credit to become negative");
    expect(!window.consume(0), "window accepted a zero-byte send");

    expect(window.replenish(25), "window rejected returned credit");
    expect(window.available() == 65, "replenish did not return credit");
    expect(!window.replenish(36), "window grew beyond its capacity");
    expect(!window.replenish(0), "window accepted a zero-byte update");
}

void testWindowIncrementEncoding() {
    const auto payload = nexus::flow::encodeWindowIncrement(0x01020304);
    expect(payload == std::vector<std::uint8_t>({1, 2, 3, 4}),
           "window increment was not encoded in network byte order");
    expect(nexus::flow::encodeWindowIncrement(0).empty(),
           "zero window increment was encoded");

    std::uint32_t decoded = 0;
    expect(nexus::flow::decodeWindowIncrement(payload, decoded),
           "valid window increment was not decoded");
    expect(decoded == 0x01020304, "decoded window increment changed");
    expect(!nexus::flow::decodeWindowIncrement({payload.data(), 3}, decoded),
           "short window increment was accepted");
    const std::vector<std::uint8_t> zero(4, 0);
    expect(!nexus::flow::decodeWindowIncrement(zero, decoded),
           "zero window increment was accepted");
}

} // namespace

int main() {
    try {
        testSendWindowAccounting();
        testWindowIncrementEncoding();
    } catch (const std::exception& error) {
        std::cerr << "Flow-control test failed: " << error.what() << std::endl;
        return 1;
    }

    std::cout << "All flow-control tests passed" << std::endl;
    return 0;
}
