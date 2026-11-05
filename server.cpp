// NexusProxy edge server
//
// The edge keeps one developer tunnel open and multiplexes every public TCP
// connection through it. Protocol stream IDs keep each conversation separate.

#include "config.hpp"
#include "heartbeat.hpp"
#include "protocol.hpp"
#include "relay.hpp"
#include "shutdown.hpp"
#include "socket_utils.hpp"
#include "unique_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using nexus::net::flushOutput;
using nexus::net::drainOutput;
using nexus::net::FlushResult;
using nexus::net::OutputQueue;
using nexus::net::queueFrame;
using nexus::net::setNonBlocking;
using nexus::net::UniqueSocket;

constexpr int kTunnelPort = 7000;
constexpr int kPublicPort = 8000;
constexpr std::size_t kReadBufferSize = 16 * 1024;
constexpr std::size_t kMaxStreamQueueBytes = 256 * 1024;
constexpr std::size_t kMaxTunnelQueueBytes = 4 * 1024 * 1024;
constexpr std::size_t kTunnelControlReserve = 64 * 1024;
constexpr auto kTunnelIdleTimeout = 20s;

struct PublicStream {
  UniqueSocket socket;
  OutputQueue output;
  // CLOSE may arrive after DATA that is still queued. Delay close() until the
  // final bytes reach the public user.
  bool closeAfterFlush{};
};

// Leave room for one DATA frame plus control frames such as CLOSE. When this is
// false, poll() temporarily stops requesting reads from public sockets.
bool tunnelCanAcceptData(const OutputQueue &output) {
  constexpr std::size_t required = nexus::protocol::kFrameHeaderSize +
                                   kReadBufferSize + kTunnelControlReserve;
  return output.canAppend(required, kMaxTunnelQueueBytes);
}

void removePublicStream(
    std::unordered_map<std::uint32_t, PublicStream> &streams,
    std::uint32_t streamId) {
  const auto stream = streams.find(streamId);
  if (stream == streams.end()) {
    return;
  }
  streams.erase(stream);
  std::cout << "Closed public stream " << streamId << std::endl;
}

void closeAllPublicStreams(
    std::unordered_map<std::uint32_t, PublicStream> &streams) {
  for (const auto &entry : streams) {
    std::cout << "Closed public stream " << entry.first << std::endl;
  }
  streams.clear();
}

std::uint32_t allocateStreamId(
    std::uint32_t &nextStreamId,
    const std::unordered_map<std::uint32_t, PublicStream> &streams) {
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

bool queueClose(OutputQueue &tunnelOutput, std::uint32_t streamId) {
  return queueFrame(tunnelOutput,
                    {nexus::protocol::MessageType::Close, streamId, {}},
                    kMaxTunnelQueueBytes);
}

void finishPublicStream(
    std::unordered_map<std::uint32_t, PublicStream> &streams,
    OutputQueue &tunnelOutput, std::uint32_t streamId, bool notifyClient) {
  if (notifyClient && !queueClose(tunnelOutput, streamId)) {
    std::cerr << "Failed to encode CLOSE for stream " << streamId << std::endl;
  }
  removePublicStream(streams, streamId);
}

bool handleTunnelFrames(
    nexus::protocol::FrameDecoder &decoder,
    std::unordered_map<std::uint32_t, PublicStream> &streams,
    OutputQueue &tunnelOutput) {
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

    if (frame.type == nexus::protocol::MessageType::Ping) {
      // Echoing the token lets the client match this PONG to its PING.
      const nexus::protocol::Frame pong{nexus::protocol::MessageType::Pong, 0,
                                        frame.payload};
      if (!queueFrame(tunnelOutput, pong, kMaxTunnelQueueBytes)) {
        std::cerr << "Could not queue PONG; tunnel output is full" << std::endl;
        return false;
      }
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
      if (!stream->second.output.append(frame.payload, kMaxStreamQueueBytes)) {
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

int pollTimeoutUntil(std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= 0ms) {
    return 0;
  }
  return static_cast<int>(std::min<std::int64_t>(
      remaining.count(), std::numeric_limits<int>::max()));
}

} // namespace

int main(int argc, char *argv[]) {
  std::signal(SIGPIPE, SIG_IGN);
  if (!nexus::process::installShutdownHandlers()) {
    std::cerr << "Failed to install shutdown signal handlers" << std::endl;
    return 1;
  }

  int tunnelPort = kTunnelPort;
  int publicPort = kPublicPort;
  if (argc > 3) {
    std::cerr << "Usage: ./nexus-edge [tunnel-port] [public-port]" << std::endl;
    return 1;
  }
  if (argc >= 2 && !nexus::config::parsePort(argv[1], tunnelPort)) {
    std::cerr << "Invalid tunnel port: " << argv[1] << std::endl;
    return 1;
  }
  if (argc >= 3 && !nexus::config::parsePort(argv[2], publicPort)) {
    std::cerr << "Invalid public port: " << argv[2] << std::endl;
    return 1;
  }

  UniqueSocket tunnelListener{nexus::net::createTcpListener(tunnelPort)};
  if (!tunnelListener) {
    std::cerr << "Failed to listen on tunnel port " << tunnelPort << ": "
              << std::strerror(errno) << std::endl;
    return 1;
  }
  std::cout << "Listening on port " << tunnelPort << std::endl;

  UniqueSocket publicListener{nexus::net::createTcpListener(publicPort)};
  if (!publicListener) {
    std::cerr << "Failed to listen on public port " << publicPort << ": "
              << std::strerror(errno) << std::endl;
    return 1;
  }
  std::cout << "Listening on port " << publicPort << std::endl;

  UniqueSocket tunnelSocket;
  std::uint32_t nextStreamId = 1;
  std::unordered_map<std::uint32_t, PublicStream> publicStreams;
  nexus::protocol::FrameDecoder tunnelDecoder;
  OutputQueue tunnelOutput;
  std::optional<std::chrono::steady_clock::time_point> tunnelIdleDeadline;
  bool running = true;

  auto disconnectTunnel = [&] {
    tunnelSocket.reset();
    tunnelOutput.clear();
    tunnelDecoder = nexus::protocol::FrameDecoder{};
    tunnelIdleDeadline.reset();
    closeAllPublicStreams(publicStreams);
    std::cout << "Tunnel disconnected; waiting for a developer client"
              << std::endl;
  };

  while (running && !nexus::process::shutdownRequested()) {
    if (tunnelSocket && tunnelIdleDeadline &&
        std::chrono::steady_clock::now() >= *tunnelIdleDeadline) {
      std::cerr << "Tunnel was silent past its idle deadline" << std::endl;
      disconnectTunnel();
    }
    std::vector<pollfd> pollFds;
    pollFds.push_back({tunnelListener.get(), POLLIN, 0});
    const short publicListenerEvents =
        tunnelSocket && tunnelCanAcceptData(tunnelOutput) ? POLLIN : 0;
    pollFds.push_back({publicListener.get(), publicListenerEvents, 0});

    std::size_t tunnelPollIndex = std::numeric_limits<std::size_t>::max();
    if (tunnelSocket) {
      tunnelPollIndex = pollFds.size();
      short events = POLLIN;
      if (!tunnelOutput.empty()) {
        events |= POLLOUT;
      }
      pollFds.push_back({tunnelSocket.get(), events, 0});
    }

    std::vector<WatchedStream> watchedStreams;
    watchedStreams.reserve(publicStreams.size());
    for (const auto &[streamId, stream] : publicStreams) {
      short events =
          !stream.closeAfterFlush && tunnelCanAcceptData(tunnelOutput) ? POLLIN
                                                                       : 0;
      if (!stream.output.empty()) {
        events |= POLLOUT;
      }
      watchedStreams.push_back({streamId, pollFds.size()});
      pollFds.push_back({stream.socket.get(), events, 0});
    }

    int ready;
    do {
      const int timeout = tunnelIdleDeadline
                              ? pollTimeoutUntil(*tunnelIdleDeadline)
                              : -1;
      ready = poll(pollFds.data(), pollFds.size(), timeout);
    } while (ready == -1 && errno == EINTR &&
             !nexus::process::shutdownRequested());

    if (nexus::process::shutdownRequested()) {
      // Break before processing ready listeners so shutdown accepts no new work.
      break;
    }

    if (ready == -1) {
      std::cerr << "poll() failed: " << std::strerror(errno) << std::endl;
      break;
    }

    if (pollFds[0].revents & POLLIN) {
      while (!nexus::process::shutdownRequested()) {
        UniqueSocket accepted{accept(tunnelListener.get(), nullptr, nullptr)};
        if (!accepted) {
          if (errno == EINTR) {
            continue;
          }
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "Failed to accept tunnel: " << std::strerror(errno)
                      << std::endl;
          }
          break;
        }

        if (tunnelSocket) {
          std::cerr << "A tunnel is already connected; rejecting another"
                    << std::endl;
        } else if (!setNonBlocking(accepted.get())) {
          std::cerr << "Failed to make tunnel non-blocking" << std::endl;
        } else {
          tunnelSocket = std::move(accepted);
          // Any valid tunnel traffic refreshes this deadline. Healthy clients
          // remain connected because their PING frames arrive well before it.
          tunnelIdleDeadline =
              std::chrono::steady_clock::now() + kTunnelIdleTimeout;
          std::cout << "Developer tunnel connected" << std::endl;
        }
      }
    }

    if (pollFds[1].revents & POLLIN) {
      while (!nexus::process::shutdownRequested()) {
        UniqueSocket accepted{accept(publicListener.get(), nullptr, nullptr)};
        if (!accepted) {
          if (errno == EINTR) {
            continue;
          }
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "Failed to accept public connection: "
                      << std::strerror(errno) << std::endl;
          }
          break;
        }

        if (!tunnelSocket) {
          std::cerr << "No tunnel connected; rejecting public connection"
                    << std::endl;
          continue;
        }
        if (!setNonBlocking(accepted.get())) {
          std::cerr << "Failed to make public socket non-blocking" << std::endl;
          continue;
        }

        const std::uint32_t streamId =
            allocateStreamId(nextStreamId, publicStreams);
        if (streamId == 0) {
          std::cerr << "No stream IDs are available" << std::endl;
          continue;
        }

        publicStreams.emplace(streamId,
                              PublicStream{std::move(accepted), {}, false});
        if (!queueFrame(tunnelOutput,
                        {nexus::protocol::MessageType::Open, streamId, {}},
                        kMaxTunnelQueueBytes)) {
          removePublicStream(publicStreams, streamId);
          continue;
        }
        std::cout << "Opened public stream " << streamId << std::endl;
      }
    }

    if (tunnelSocket &&
        tunnelPollIndex != std::numeric_limits<std::size_t>::max()) {
      const short events = pollFds[tunnelPollIndex].revents;

      if (events & POLLOUT) {
        if (flushOutput(tunnelSocket.get(), tunnelOutput) !=
            FlushResult::Ready) {
          disconnectTunnel();
        }
      }

      if (tunnelSocket && (events & (POLLIN | POLLHUP))) {
        std::uint8_t buffer[kReadBufferSize];
        while (true) {
          const ssize_t received =
              recv(tunnelSocket.get(), buffer, sizeof(buffer), 0);
          if (received > 0) {
            tunnelIdleDeadline =
                std::chrono::steady_clock::now() + kTunnelIdleTimeout;
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

      if (tunnelSocket && (events & (POLLERR | POLLNVAL))) {
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
            flushOutput(stream->second.socket.get(), stream->second.output);
        if (result != FlushResult::Ready) {
          finishPublicStream(publicStreams, tunnelOutput, watched.streamId,
                             static_cast<bool>(tunnelSocket));
          continue;
        }
        if (stream->second.closeAfterFlush && stream->second.output.empty()) {
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
              recv(current->second.socket.get(), buffer, sizeof(buffer), 0);
          if (received > 0) {
            const nexus::protocol::Frame dataFrame{
                nexus::protocol::MessageType::Data,
                watched.streamId,
                {buffer, buffer + received}};
            if (!queueFrame(tunnelOutput, dataFrame, kMaxTunnelQueueBytes)) {
              std::cerr << "Failed to encode DATA for stream "
                        << watched.streamId << std::endl;
              running = false;
              streamFinished = true;
            }
            continue;
          }
          if (received == 0) {
            finishPublicStream(publicStreams, tunnelOutput, watched.streamId,
                               static_cast<bool>(tunnelSocket));
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
          std::cerr << "Public recv() failed for stream " << watched.streamId
                    << ": " << std::strerror(errno) << std::endl;
          finishPublicStream(publicStreams, tunnelOutput, watched.streamId,
                             static_cast<bool>(tunnelSocket));
          streamFinished = true;
        }
      }

      if (publicStreams.contains(watched.streamId) &&
          (events & (POLLERR | POLLNVAL))) {
        finishPublicStream(publicStreams, tunnelOutput, watched.streamId,
                           static_cast<bool>(tunnelSocket));
      }
    }
  }

  if (nexus::process::shutdownRequested() && tunnelSocket) {
    for (const auto &[streamId, stream] : publicStreams) {
      static_cast<void>(stream);
      queueClose(tunnelOutput, streamId);
    }
    // Give CLOSE frames a brief chance to leave, then rely on RAII even if the
    // peer is stalled. A shutdown path must have its own deadline too.
    drainOutput(tunnelSocket.get(), tunnelOutput, 1s);
  }
  closeAllPublicStreams(publicStreams);
  if (nexus::process::shutdownRequested()) {
    std::cout << "Edge shutdown complete" << std::endl;
  }
  return running ? 0 : 1;
}
