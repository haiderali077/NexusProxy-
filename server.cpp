// NexusProxy edge server
//
// The edge keeps one developer tunnel open and multiplexes every public TCP
// connection through it. Protocol stream IDs keep each conversation separate.

#include "protocol.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kTunnelPort = 7000;
constexpr int kPublicPort = 8000;
constexpr std::size_t kReadBufferSize = 16 * 1024;
constexpr std::size_t kMaxStreamQueueBytes = 256 * 1024;
constexpr std::size_t kMaxTunnelQueueBytes = 4 * 1024 * 1024;
constexpr std::size_t kTunnelControlReserve = 64 * 1024;

// A non-blocking send may stop halfway through a buffer. offset remembers the
// first unsent byte so the next POLLOUT event can continue from that position.
struct OutputQueue {
    std::vector<std::uint8_t> bytes;
    std::size_t offset{};

    bool empty() const {
        return offset == bytes.size();
    }

    std::size_t pending() const {
        return bytes.size() - offset;
    }

    bool canAppend(std::size_t count, std::size_t limit) const {
        return count <= limit && pending() <= limit - count;
    }

    bool append(std::span<const std::uint8_t> data, std::size_t limit) {
        if (!canAppend(data.size(), limit)) {
            return false;
        }
        if (empty()) {
            bytes.clear();
            offset = 0;
        } else if (bytes.size() + data.size() > limit && offset != 0) {
            bytes.erase(bytes.begin(), bytes.begin() + offset);
            offset = 0;
        }
        bytes.insert(bytes.end(), data.begin(), data.end());
        return true;
    }

    void consume(std::size_t count) {
        offset += count;
        if (empty()) {
            bytes.clear();
            offset = 0;
        } else if (offset >= 4096 && offset * 2 >= bytes.size()) {
            bytes.erase(bytes.begin(), bytes.begin() + offset);
            offset = 0;
        }
    }

    void clear() {
        bytes.clear();
        offset = 0;
    }
};

struct PublicStream {
    int socket{-1};
    OutputQueue output;
    // CLOSE may arrive after DATA that is still queued. Delay close() until the
    // final bytes reach the public user.
    bool closeAfterFlush{};
};

enum class FlushResult {
    Ready,
    Closed,
    Error,
};

bool setNonBlocking(int socketFd) {
    const int flags = fcntl(socketFd, F_GETFL, 0);
    if (flags == -1 || fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return false;
    }
    return true;
}

int createListener(int port) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        std::cerr << "Failed to create listener for port " << port << std::endl;
        return -1;
    }

    int option = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &option,
                   sizeof(option)) == -1) {
        std::cerr << "Failed to set SO_REUSEADDR on port " << port << std::endl;
        close(listener);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == -1) {
        std::cerr << "Failed to bind port " << port << ": "
                  << std::strerror(errno) << std::endl;
        close(listener);
        return -1;
    }
    if (listen(listener, SOMAXCONN) == -1 || !setNonBlocking(listener)) {
        std::cerr << "Failed to prepare listener on port " << port << std::endl;
        close(listener);
        return -1;
    }

    std::cout << "Listening on port " << port << std::endl;
    return listener;
}

bool queueFrame(OutputQueue& output, const nexus::protocol::Frame& frame,
                std::size_t limit) {
    std::vector<std::uint8_t> encoded;
    if (!nexus::protocol::encodeFrame(frame, encoded)) {
        return false;
    }
    return output.append(encoded, limit);
}

// Leave room for one DATA frame plus control frames such as CLOSE. When this is
// false, poll() temporarily stops requesting reads from public sockets.
bool tunnelCanAcceptData(const OutputQueue& output) {
    constexpr std::size_t required = nexus::protocol::kFrameHeaderSize +
                                     kReadBufferSize + kTunnelControlReserve;
    return output.canAppend(required, kMaxTunnelQueueBytes);
}

FlushResult flushOutput(int socketFd, OutputQueue& output) {
    while (!output.empty()) {
        const ssize_t sent = send(
            socketFd, output.bytes.data() + output.offset, output.pending(), 0);
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

void removePublicStream(
    std::unordered_map<std::uint32_t, PublicStream>& streams,
    std::uint32_t streamId) {
    const auto stream = streams.find(streamId);
    if (stream == streams.end()) {
        return;
    }
    close(stream->second.socket);
    streams.erase(stream);
    std::cout << "Closed public stream " << streamId << std::endl;
}

void closeAllPublicStreams(
    std::unordered_map<std::uint32_t, PublicStream>& streams) {
    for (auto& [streamId, stream] : streams) {
        close(stream.socket);
        std::cout << "Closed public stream " << streamId << std::endl;
    }
    streams.clear();
}

std::uint32_t allocateStreamId(
    std::uint32_t& nextStreamId,
    const std::unordered_map<std::uint32_t, PublicStream>& streams) {
    if (streams.size() >= std::numeric_limits<std::uint32_t>::max() - 1ULL) {
        return 0;
    }

    while (true) {
        const std::uint32_t candidate = nextStreamId++;
        if (nextStreamId == 0) {
            nextStreamId = 1;
        }
        if (candidate != 0 && !streams.contains(candidate)) {
            return candidate;
        }
    }
}

bool queueClose(OutputQueue& tunnelOutput, std::uint32_t streamId) {
    return queueFrame(tunnelOutput,
                      {nexus::protocol::MessageType::Close, streamId, {}},
                      kMaxTunnelQueueBytes);
}

void finishPublicStream(
    std::unordered_map<std::uint32_t, PublicStream>& streams,
    OutputQueue& tunnelOutput, std::uint32_t streamId, bool notifyClient) {
    if (notifyClient && !queueClose(tunnelOutput, streamId)) {
        std::cerr << "Failed to encode CLOSE for stream " << streamId
                  << std::endl;
    }
    removePublicStream(streams, streamId);
}

bool handleTunnelFrames(
    nexus::protocol::FrameDecoder& decoder,
    std::unordered_map<std::uint32_t, PublicStream>& streams,
    OutputQueue& tunnelOutput) {
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
                std::cerr << "Public stream " << frame.streamId
                          << " exceeded its output limit" << std::endl;
                queueClose(tunnelOutput, frame.streamId);
                removePublicStream(streams, frame.streamId);
            }
            continue;
        }
        if (frame.type == nexus::protocol::MessageType::Close) {
            if (stream->second.output.empty()) {
                removePublicStream(streams, frame.streamId);
            } else {
                stream->second.closeAfterFlush = true;
            }
            continue;
        }

        std::cerr << "Unexpected frame type from developer client" << std::endl;
        return false;
    }
}

struct WatchedStream {
    std::uint32_t streamId{};
    std::size_t pollIndex{};
};

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    const int tunnelListener = createListener(kTunnelPort);
    if (tunnelListener == -1) {
        return 1;
    }
    const int publicListener = createListener(kPublicPort);
    if (publicListener == -1) {
        close(tunnelListener);
        return 1;
    }

    int tunnelSocket = -1;
    std::uint32_t nextStreamId = 1;
    std::unordered_map<std::uint32_t, PublicStream> publicStreams;
    nexus::protocol::FrameDecoder tunnelDecoder;
    OutputQueue tunnelOutput;
    bool running = true;

    auto disconnectTunnel = [&] {
        if (tunnelSocket != -1) {
            close(tunnelSocket);
            tunnelSocket = -1;
        }
        tunnelOutput.clear();
        tunnelDecoder = nexus::protocol::FrameDecoder{};
        closeAllPublicStreams(publicStreams);
        std::cout << "Tunnel disconnected; waiting for a developer client"
                  << std::endl;
    };

    while (running) {
        std::vector<pollfd> pollFds;
        pollFds.push_back({tunnelListener, POLLIN, 0});
        const short publicListenerEvents =
            tunnelSocket != -1 && tunnelCanAcceptData(tunnelOutput) ? POLLIN : 0;
        pollFds.push_back({publicListener, publicListenerEvents, 0});

        std::size_t tunnelPollIndex = std::numeric_limits<std::size_t>::max();
        if (tunnelSocket != -1) {
            tunnelPollIndex = pollFds.size();
            short events = POLLIN;
            if (!tunnelOutput.empty()) {
                events |= POLLOUT;
            }
            pollFds.push_back({tunnelSocket, events, 0});
        }

        std::vector<WatchedStream> watchedStreams;
        watchedStreams.reserve(publicStreams.size());
        for (const auto& [streamId, stream] : publicStreams) {
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
            break;
        }

        if (pollFds[0].revents & POLLIN) {
            while (true) {
                const int accepted = accept(tunnelListener, nullptr, nullptr);
                if (accepted == -1) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "Failed to accept tunnel: "
                                  << std::strerror(errno) << std::endl;
                    }
                    break;
                }

                if (tunnelSocket != -1) {
                    std::cerr << "A tunnel is already connected; rejecting another"
                              << std::endl;
                    close(accepted);
                } else if (!setNonBlocking(accepted)) {
                    std::cerr << "Failed to make tunnel non-blocking" << std::endl;
                    close(accepted);
                } else {
                    tunnelSocket = accepted;
                    std::cout << "Developer tunnel connected" << std::endl;
                }
            }
        }

        if (pollFds[1].revents & POLLIN) {
            while (true) {
                const int accepted = accept(publicListener, nullptr, nullptr);
                if (accepted == -1) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        std::cerr << "Failed to accept public connection: "
                                  << std::strerror(errno) << std::endl;
                    }
                    break;
                }

                if (tunnelSocket == -1) {
                    std::cerr << "No tunnel connected; rejecting public connection"
                              << std::endl;
                    close(accepted);
                    continue;
                }
                if (!setNonBlocking(accepted)) {
                    std::cerr << "Failed to make public socket non-blocking"
                              << std::endl;
                    close(accepted);
                    continue;
                }

                const std::uint32_t streamId =
                    allocateStreamId(nextStreamId, publicStreams);
                if (streamId == 0) {
                    std::cerr << "No stream IDs are available" << std::endl;
                    close(accepted);
                    continue;
                }

                publicStreams.emplace(streamId, PublicStream{accepted, {}, false});
                if (!queueFrame(tunnelOutput,
                                {nexus::protocol::MessageType::Open,
                                 streamId, {}},
                                kMaxTunnelQueueBytes)) {
                    removePublicStream(publicStreams, streamId);
                    continue;
                }
                std::cout << "Opened public stream " << streamId << std::endl;
            }
        }

        if (tunnelSocket != -1 &&
            tunnelPollIndex != std::numeric_limits<std::size_t>::max()) {
            const short events = pollFds[tunnelPollIndex].revents;

            if (events & POLLOUT) {
                if (flushOutput(tunnelSocket, tunnelOutput) != FlushResult::Ready) {
                    disconnectTunnel();
                }
            }

            if (tunnelSocket != -1 && (events & (POLLIN | POLLHUP))) {
                std::uint8_t buffer[kReadBufferSize];
                while (true) {
                    const ssize_t received =
                        recv(tunnelSocket, buffer, sizeof(buffer), 0);
                    if (received > 0) {
                        tunnelDecoder.append(std::span<const std::uint8_t>(
                            buffer, static_cast<std::size_t>(received)));
                        if (!handleTunnelFrames(tunnelDecoder, publicStreams,
                                                tunnelOutput)) {
                            disconnectTunnel();
                            break;
                        }
                        continue;
                    }
                    if (received == 0) {
                        disconnectTunnel();
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
                    disconnectTunnel();
                    break;
                }
            }

            if (tunnelSocket != -1 && (events & (POLLERR | POLLNVAL))) {
                disconnectTunnel();
            }
        }

        for (const WatchedStream watched : watchedStreams) {
            const auto stream = publicStreams.find(watched.streamId);
            if (stream == publicStreams.end()) {
                continue;
            }

            const short events = pollFds[watched.pollIndex].revents;
            if (events & POLLOUT) {
                const FlushResult result =
                    flushOutput(stream->second.socket, stream->second.output);
                if (result != FlushResult::Ready) {
                    finishPublicStream(publicStreams, tunnelOutput,
                                       watched.streamId, tunnelSocket != -1);
                    continue;
                }
                if (stream->second.closeAfterFlush &&
                    stream->second.output.empty()) {
                    removePublicStream(publicStreams, watched.streamId);
                    continue;
                }
            }

            const auto current = publicStreams.find(watched.streamId);
            if (current == publicStreams.end()) {
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
                            running = false;
                            streamFinished = true;
                        }
                        continue;
                    }
                    if (received == 0) {
                        finishPublicStream(publicStreams, tunnelOutput,
                                           watched.streamId,
                                           tunnelSocket != -1);
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
                    std::cerr << "Public recv() failed for stream "
                              << watched.streamId << ": "
                              << std::strerror(errno) << std::endl;
                    finishPublicStream(publicStreams, tunnelOutput,
                                       watched.streamId, tunnelSocket != -1);
                    streamFinished = true;
                }
            }

            if (publicStreams.contains(watched.streamId) &&
                (events & (POLLERR | POLLNVAL))) {
                finishPublicStream(publicStreams, tunnelOutput,
                                   watched.streamId, tunnelSocket != -1);
            }
        }
    }

    if (tunnelSocket != -1) {
        close(tunnelSocket);
    }
    closeAllPublicStreams(publicStreams);
    close(tunnelListener);
    close(publicListener);
    return running ? 0 : 1;
}
