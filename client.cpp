// NexusProxy developer client
//
// One persistent connection reaches the public edge. Each OPEN frame creates a
// separate connection to the local service, and stream IDs route DATA frames.

#include "protocol.hpp"
#include "relay.hpp"
#include "socket_utils.hpp"

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
#include <vector>

namespace {

using nexus::net::FlushResult;
using nexus::net::OutputQueue;
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
    int socket{-1};
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
    close(stream->second.socket);
    streams.erase(stream);
    std::cout << "Closed local stream " << streamId << std::endl;
}

void closeAllLocalStreams(
    std::unordered_map<std::uint32_t, LocalStream>& streams) {
    for (auto& [streamId, stream] : streams) {
        close(stream.socket);
        std::cout << "Closed local stream " << streamId << std::endl;
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

            const int localSocket = connectTcp(localAddress, localPort);
            if (localSocket == -1) {
                std::cerr << "Failed to connect stream " << frame.streamId
                          << " to " << localAddress << ':' << localPort << ": "
                          << std::strerror(errno) << std::endl;
                queueClose(tunnelOutput, frame.streamId);
                continue;
            }
            if (!setNonBlocking(localSocket)) {
                std::cerr << "Failed to make local stream non-blocking"
                          << std::endl;
                close(localSocket);
                queueClose(tunnelOutput, frame.streamId);
                continue;
            }

            streams.emplace(frame.streamId,
                            LocalStream{localSocket, {}, false});
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

    if (argc > 3) {
        std::cerr << "Usage: ./client [local-address] [local-port]" << std::endl;
        return 1;
    }
    if (argc >= 2) {
        localAddress = argv[1];
    }
    if (argc >= 3) {
        try {
            localPort = std::stoi(argv[2]);
        } catch (const std::exception&) {
            std::cerr << "Invalid port number: " << argv[2] << std::endl;
            return 1;
        }
        if (localPort < 1 || localPort > 65535) {
            std::cerr << "Invalid port number: " << argv[2] << std::endl;
            return 1;
        }
    }

    int tunnelSocket = connectTcp(kEdgeAddress, kTunnelPort);
    if (tunnelSocket == -1) {
        std::cerr << "Failed to connect to edge " << kEdgeAddress << ':'
                  << kTunnelPort << ": " << std::strerror(errno) << std::endl;
        return 1;
    }
    if (!setNonBlocking(tunnelSocket)) {
        std::cerr << "Failed to make the tunnel non-blocking" << std::endl;
        close(tunnelSocket);
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
        pollFds.push_back({tunnelSocket, tunnelEvents, 0});

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
            pollFds.push_back({stream.socket, events, 0});
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
            if (flushOutput(tunnelSocket, tunnelOutput) != FlushResult::Ready) {
                std::cerr << "Failed to flush tunnel output" << std::endl;
                relaySucceeded = false;
                break;
            }
        }

        if (tunnelReady & (POLLIN | POLLHUP)) {
            std::uint8_t buffer[kReadBufferSize];
            while (true) {
                const ssize_t received =
                    recv(tunnelSocket, buffer, sizeof(buffer), 0);
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
                    flushOutput(stream->second.socket, stream->second.output);
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
                        recv(current->second.socket, buffer, sizeof(buffer), 0);
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
    close(tunnelSocket);
    return relaySucceeded ? 0 : 1;
}
