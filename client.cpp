// NexusProxy developer client
//
// One persistent connection reaches the public edge. Each OPEN frame creates a
// separate connection to the local service, and stream IDs route DATA frames.

#include "protocol.hpp"
#include "config.hpp"
#include "relay.hpp"
#include "socket_utils.hpp"
#include "unique_socket.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using nexus::net::FlushResult;
using nexus::net::OutputQueue;
using nexus::net::UniqueSocket;
using nexus::net::connectTcp;
using nexus::net::flushOutput;
using nexus::net::queueFrame;
using nexus::net::setNonBlocking;

constexpr const char* kEdgeAddress = "127.0.0.1";
constexpr int kTunnelPort = 7000;
constexpr std::size_t kReadBufferSize = 16 * 1024;
constexpr std::size_t kMaxStreamQueueBytes = 256 * 1024;
constexpr std::size_t kMaxTunnelQueueBytes = 4 * 1024 * 1024;
constexpr std::size_t kTunnelControlReserve = 64 * 1024;

struct LocalStream {
    UniqueSocket socket;
    OutputQueue output;
    bool closeAfterFlush{};
};

bool tunnelCanAcceptData(const OutputQueue& output) {
    constexpr std::size_t required = nexus::protocol::kFrameHeaderSize +
                                     kReadBufferSize + kTunnelControlReserve;
    return output.canAppend(required, kMaxTunnelQueueBytes);
}

bool queueClose(OutputQueue& tunnelOutput, std::uint32_t streamId) {
    return queueFrame(tunnelOutput,
                      {nexus::protocol::MessageType::Close, streamId, {}},
                      kMaxTunnelQueueBytes);
}

void removeLocalStream(
    std::unordered_map<std::uint32_t, LocalStream>& streams,
    std::uint32_t streamId) {
    const auto stream = streams.find(streamId);
    if (stream == streams.end()) {
        return;
    }
    streams.erase(stream);
    std::cout << "Closed local stream " << streamId << std::endl;
}

void closeAllLocalStreams(
    std::unordered_map<std::uint32_t, LocalStream>& streams) {
    for (const auto& entry : streams) {
        std::cout << "Closed local stream " << entry.first << std::endl;
    }
    streams.clear();
}

bool handleTunnelFrames(
    nexus::protocol::FrameDecoder& decoder,
    std::unordered_map<std::uint32_t, LocalStream>& streams,
    OutputQueue& tunnelOutput, const char* localAddress, int localPort) {
    while (true) {
        nexus::protocol::Frame frame;
        const auto status = decoder.nextFrame(frame);
        if (status == nexus::protocol::DecodeStatus::NeedMoreData) {
            return true;
        }
        if (status == nexus::protocol::DecodeStatus::ProtocolError) {
            std::cerr << "Tunnel protocol error: " << decoder.error() << std::endl;
            return false;
        }

        if (frame.type == nexus::protocol::MessageType::Open) {
            if (streams.contains(frame.streamId)) {
                std::cerr << "Received duplicate OPEN for stream "
                          << frame.streamId << std::endl;
                return false;
            }

            UniqueSocket localSocket{connectTcp(localAddress, localPort)};
            if (!localSocket) {
                std::cerr << "Failed to connect stream " << frame.streamId
                          << " to " << localAddress << ':' << localPort << ": "
                          << std::strerror(errno) << std::endl;
                queueClose(tunnelOutput, frame.streamId);
                continue;
            }
            if (!setNonBlocking(localSocket.get())) {
                std::cerr << "Failed to make local stream non-blocking"
                          << std::endl;
                queueClose(tunnelOutput, frame.streamId);
                continue;
            }

            streams.emplace(
                frame.streamId,
                LocalStream{std::move(localSocket), {}, false});
            std::cout << "Opened local stream " << frame.streamId << std::endl;
            continue;
        }

        const auto stream = streams.find(frame.streamId);
        if (stream == streams.end()) {
            if (frame.type == nexus::protocol::MessageType::Data) {
                queueClose(tunnelOutput, frame.streamId);
            }
            continue;
        }

        if (frame.type == nexus::protocol::MessageType::Data) {
            if (!stream->second.output.append(frame.payload,
                                               kMaxStreamQueueBytes)) {
                std::cerr << "Local stream " << frame.streamId
                          << " exceeded its output limit" << std::endl;
                queueClose(tunnelOutput, frame.streamId);
                removeLocalStream(streams, frame.streamId);
            }
            continue;
        }
        if (frame.type == nexus::protocol::MessageType::Close) {
            if (stream->second.output.empty()) {
                removeLocalStream(streams, frame.streamId);
            } else {
                stream->second.closeAfterFlush = true;
            }
            continue;
        }

        std::cerr << "Unexpected frame type from edge" << std::endl;
        return false;
    }
}

struct WatchedStream {
    std::uint32_t streamId{};
    std::size_t pollIndex{};
};

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    const char* localAddress = "127.0.0.1";
    int localPort = 9000;
    const char* edgeAddress = kEdgeAddress;
    int tunnelPort = kTunnelPort;

    if (argc > 5) {
        std::cerr << "Usage: ./nexus-client [local-address] [local-port] "
                     "[edge-address] [tunnel-port]"
                  << std::endl;
        return 1;
    }
    if (argc >= 2) {
        localAddress = argv[1];
    }
    if (argc >= 3) {
        if (!nexus::config::parsePort(argv[2], localPort)) {
            std::cerr << "Invalid port number: " << argv[2] << std::endl;
            return 1;
        }
    }
    if (argc >= 4) {
        edgeAddress = argv[3];
    }
    if (argc >= 5) {
        if (!nexus::config::parsePort(argv[4], tunnelPort)) {
            std::cerr << "Invalid tunnel port: " << argv[4] << std::endl;
            return 1;
        }
    }

    UniqueSocket tunnelSocket{connectTcp(edgeAddress, tunnelPort)};
    if (!tunnelSocket) {
        std::cerr << "Failed to connect to edge " << edgeAddress << ':'
                  << tunnelPort << ": " << std::strerror(errno) << std::endl;
        return 1;
    }
    if (!setNonBlocking(tunnelSocket.get())) {
        std::cerr << "Failed to make the tunnel non-blocking" << std::endl;
        return 1;
    }

    std::cout << "Connected to edge; exposing " << localAddress << ':'
              << localPort << std::endl;

    std::unordered_map<std::uint32_t, LocalStream> localStreams;
    nexus::protocol::FrameDecoder tunnelDecoder;
    OutputQueue tunnelOutput;
    bool relaySucceeded = true;
    bool running = true;

    while (running) {
        std::vector<pollfd> pollFds;
        short tunnelEvents = POLLIN;
        if (!tunnelOutput.empty()) {
            tunnelEvents |= POLLOUT;
        }
        pollFds.push_back({tunnelSocket.get(), tunnelEvents, 0});

        std::vector<WatchedStream> watchedStreams;
        watchedStreams.reserve(localStreams.size());
        for (const auto& [streamId, stream] : localStreams) {
            short events =
                !stream.closeAfterFlush && tunnelCanAcceptData(tunnelOutput)
                    ? POLLIN
                    : 0;
            if (!stream.output.empty()) {
                events |= POLLOUT;
            }
            watchedStreams.push_back({streamId, pollFds.size()});
            pollFds.push_back({stream.socket.get(), events, 0});
        }

        int ready;
        do {
            ready = poll(pollFds.data(), pollFds.size(), -1);
        } while (ready == -1 && errno == EINTR);

        if (ready == -1) {
            std::cerr << "poll() failed: " << std::strerror(errno) << std::endl;
            relaySucceeded = false;
            break;
        }

        const short tunnelReady = pollFds[0].revents;
        if (tunnelReady & POLLOUT) {
            if (flushOutput(tunnelSocket.get(), tunnelOutput) !=
                FlushResult::Ready) {
                std::cerr << "Failed to flush tunnel output" << std::endl;
                relaySucceeded = false;
                break;
            }
        }

        if (tunnelReady & (POLLIN | POLLHUP)) {
            std::uint8_t buffer[kReadBufferSize];
            while (true) {
                const ssize_t received =
                    recv(tunnelSocket.get(), buffer, sizeof(buffer), 0);
                if (received > 0) {
                    tunnelDecoder.append(std::span<const std::uint8_t>(
                        buffer, static_cast<std::size_t>(received)));
                    if (!handleTunnelFrames(tunnelDecoder, localStreams,
                                            tunnelOutput, localAddress,
                                            localPort)) {
                        relaySucceeded = false;
                        running = false;
                        break;
                    }
                    continue;
                }
                if (received == 0) {
                    std::cout << "Edge closed the tunnel" << std::endl;
                    running = false;
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                std::cerr << "Tunnel recv() failed: " << std::strerror(errno)
                          << std::endl;
                relaySucceeded = false;
                running = false;
                break;
            }
        }

        if (tunnelReady & (POLLERR | POLLNVAL)) {
            std::cerr << "Tunnel reported a socket error" << std::endl;
            relaySucceeded = false;
            break;
        }

        for (const WatchedStream watched : watchedStreams) {
            const auto stream = localStreams.find(watched.streamId);
            if (stream == localStreams.end()) {
                continue;
            }

            const short events = pollFds[watched.pollIndex].revents;
            if (events & POLLOUT) {
                const FlushResult result =
                    flushOutput(stream->second.socket.get(),
                                stream->second.output);
                if (result != FlushResult::Ready) {
                    queueClose(tunnelOutput, watched.streamId);
                    removeLocalStream(localStreams, watched.streamId);
                    continue;
                }
                if (stream->second.closeAfterFlush &&
                    stream->second.output.empty()) {
                    removeLocalStream(localStreams, watched.streamId);
                    continue;
                }
            }

            const auto current = localStreams.find(watched.streamId);
            if (current == localStreams.end()) {
                continue;
            }

            if (events & (POLLIN | POLLHUP)) {
                std::uint8_t buffer[kReadBufferSize];
                bool streamFinished = false;
                while (!streamFinished) {
                    if (!tunnelCanAcceptData(tunnelOutput)) {
                        break;
                    }
                    const ssize_t received =
                        recv(current->second.socket.get(), buffer,
                             sizeof(buffer), 0);
                    if (received > 0) {
                        const nexus::protocol::Frame dataFrame{
                            nexus::protocol::MessageType::Data,
                            watched.streamId,
                            {buffer, buffer + received}};
                        if (!queueFrame(tunnelOutput, dataFrame,
                                        kMaxTunnelQueueBytes)) {
                            std::cerr << "Failed to encode DATA for stream "
                                      << watched.streamId << std::endl;
                            relaySucceeded = false;
                            running = false;
                            streamFinished = true;
                        }
                        continue;
                    }
                    if (received == 0) {
                        queueClose(tunnelOutput, watched.streamId);
                        removeLocalStream(localStreams, watched.streamId);
                        streamFinished = true;
                        continue;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        streamFinished = true;
                        continue;
                    }
                    std::cerr << "Local recv() failed for stream "
                              << watched.streamId << ": "
                              << std::strerror(errno) << std::endl;
                    queueClose(tunnelOutput, watched.streamId);
                    removeLocalStream(localStreams, watched.streamId);
                    streamFinished = true;
                }
            }

            if (localStreams.contains(watched.streamId) &&
                (events & (POLLERR | POLLNVAL))) {
                queueClose(tunnelOutput, watched.streamId);
                removeLocalStream(localStreams, watched.streamId);
            }
        }
    }

    closeAllLocalStreams(localStreams);
    return relaySucceeded ? 0 : 1;
}
