// NexusProxy edge server
//
// The edge keeps one developer tunnel open and multiplexes every public TCP
// connection through it. Protocol stream IDs keep each conversation separate.

#include "nexus/config.hpp"
#include "nexus/flow_control.hpp"
#include "nexus/authentication.hpp"
#include "nexus/auth_rate_limiter.hpp"
#include "nexus/heartbeat.hpp"
#include "nexus/limits.hpp"
#include "nexus/logging.hpp"
#include "nexus/protocol.hpp"
#include "nexus/relay.hpp"
#include "nexus/secret_config.hpp"
#include "nexus/secure_token.hpp"
#include "nexus/shutdown.hpp"
#include "nexus/socket_utils.hpp"
#include "nexus/tls_connection.hpp"
#include "nexus/tls_context.hpp"
#include "nexus/tls_transport.hpp"
#include "nexus/unique_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
using nexus::net::drainTlsOutput;
using nexus::net::flushTlsOutput;
using nexus::net::FlushResult;
using nexus::net::OutputQueue;
using nexus::net::queueFrame;
using nexus::net::setNonBlocking;
using nexus::net::UniqueSocket;

constexpr int kTunnelPort = 7000;
constexpr int kPublicPort = 8000;
constexpr auto kTunnelIdleTimeout = 20s;
constexpr auto kTlsHandshakeTimeout = 5s;
constexpr auto kAuthenticationTimeout = 5s;
static_assert(nexus::limits::kMaxConcurrentTunnels == 1,
              "the current edge owns one active tunnel");

struct PublicStream {
  UniqueSocket socket;
  OutputQueue output;
  // CLOSE may arrive after DATA that is still queued. Delay close() until the
  // final bytes reach the public user.
  bool closeAfterFlush{};
  nexus::flow::SendWindow sendWindow{
      nexus::limits::kMaxStreamQueueBytes};
};

// Leave room for one DATA frame plus control frames such as CLOSE. When this is
// false, poll() temporarily stops requesting reads from public sockets.
bool tunnelCanAcceptData(const OutputQueue &output) {
  constexpr std::size_t required = nexus::protocol::kFrameHeaderSize +
                                   nexus::limits::kReadBufferBytes +
                                   nexus::limits::kTunnelControlReserveBytes;
  return output.canAppend(required, nexus::limits::kMaxTunnelQueueBytes);
}

void removePublicStream(
    std::unordered_map<std::uint32_t, PublicStream> &streams,
    std::uint32_t streamId, std::uint64_t tunnelId) {
  const auto stream = streams.find(streamId);
  if (stream == streams.end()) {
    return;
  }
  streams.erase(stream);
  nexus::logging::writeEvent(std::cout, "edge", tunnelId, streamId,
                             "closed public stream");
}

void closeAllPublicStreams(
    std::unordered_map<std::uint32_t, PublicStream> &streams,
    std::uint64_t tunnelId) {
  for (const auto &entry : streams) {
    nexus::logging::writeEvent(std::cout, "edge", tunnelId, entry.first,
                               "closed public stream");
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
                    nexus::limits::kMaxTunnelQueueBytes);
}

void finishPublicStream(
    std::unordered_map<std::uint32_t, PublicStream> &streams,
    OutputQueue &tunnelOutput, std::uint32_t streamId, bool notifyClient,
    std::uint64_t tunnelId) {
  if (notifyClient && !queueClose(tunnelOutput, streamId)) {
    std::cerr << "Failed to encode CLOSE for stream " << streamId << std::endl;
  }
  removePublicStream(streams, streamId, tunnelId);
}

bool handleTunnelFrames(
    nexus::protocol::FrameDecoder &decoder,
    std::unordered_map<std::uint32_t, PublicStream> &streams,
    OutputQueue &tunnelOutput, std::uint64_t tunnelId) {
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
      if (!queueFrame(tunnelOutput, pong,
                      nexus::limits::kMaxTunnelQueueBytes)) {
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

    if (frame.type == nexus::protocol::MessageType::WindowUpdate) {
      std::uint32_t increment = 0;
      if (!nexus::flow::decodeWindowIncrement(frame.payload, increment) ||
          !stream->second.sendWindow.replenish(increment)) {
        std::cerr << "Invalid WINDOW_UPDATE for public stream "
                  << frame.streamId << std::endl;
        return false;
      }
      continue;
    }

    if (frame.type == nexus::protocol::MessageType::Data) {
      if (!stream->second.output.append(
              frame.payload, nexus::limits::kMaxStreamQueueBytes)) {
        std::cerr << "Public stream " << frame.streamId
                  << " exceeded its output limit" << std::endl;
        queueClose(tunnelOutput, frame.streamId);
        removePublicStream(streams, frame.streamId, tunnelId);
      }
      continue;
    }
    if (frame.type == nexus::protocol::MessageType::Close) {
      if (stream->second.output.empty()) {
        removePublicStream(streams, frame.streamId, tunnelId);
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

  const char *certificateFile = std::getenv("NEXUS_TLS_CERT_FILE");
  const char *privateKeyFile = std::getenv("NEXUS_TLS_KEY_FILE");
  if (certificateFile == nullptr || *certificateFile == '\0' ||
      privateKeyFile == nullptr || *privateKeyFile == '\0') {
    std::cerr << "NEXUS_TLS_CERT_FILE and NEXUS_TLS_KEY_FILE are required"
              << std::endl;
    return 1;
  }
  std::string tlsError;
  auto tlsContext = nexus::security::TlsContext::createServer(
      certificateFile, privateKeyFile, tlsError);
  if (!tlsContext) {
    std::cerr << "Failed to configure edge TLS: " << tlsError << std::endl;
    return 1;
  }
  std::string secretError;
  auto loadedToken = nexus::security::loadSecret(
      "NEXUS_AUTH_TOKEN", "NEXUS_AUTH_TOKEN_FILE",
      nexus::protocol::kMinAuthTokenSize,
      nexus::protocol::kMaxAuthTokenSize, secretError);
  if (!loadedToken) {
    std::cerr << "Failed to load authentication token: " << secretError
              << std::endl;
    return 1;
  }
  nexus::security::SecureToken authenticationToken{*loadedToken};
  nexus::security::AuthenticationRateLimiter authenticationLimiter{3, 10s,
                                                                    2s};

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
  std::optional<nexus::security::TlsConnection> tunnelTls;
  std::uint32_t nextStreamId = 1;
  std::unordered_map<std::uint32_t, PublicStream> publicStreams;
  nexus::protocol::FrameDecoder tunnelDecoder;
  OutputQueue tunnelOutput;
  std::optional<std::chrono::steady_clock::time_point> tunnelIdleDeadline;
  std::uint64_t nextTunnelId = 1;
  std::uint64_t activeTunnelId = 0;
  bool running = true;
  bool tlsReadWantsWrite = false;
  bool tlsWriteWantsRead = false;

  auto disconnectTunnel = [&] {
    if (tunnelSocket) {
      nexus::logging::writeEvent(std::cout, "edge", activeTunnelId,
                                 std::nullopt, "tunnel disconnected");
    }
    if (tunnelTls) {
      tunnelTls->shutdown();
      tunnelTls.reset();
    }
    tunnelSocket.reset();
    tunnelOutput.clear();
    tunnelDecoder = nexus::protocol::FrameDecoder{};
    tunnelIdleDeadline.reset();
    closeAllPublicStreams(publicStreams, activeTunnelId);
    activeTunnelId = 0;
    tlsReadWantsWrite = false;
    tlsWriteWantsRead = false;
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
      if (!tunnelOutput.empty() || tlsReadWantsWrite) {
        events |= POLLOUT;
      }
      pollFds.push_back({tunnelSocket.get(), events, 0});
    }

    std::vector<WatchedStream> watchedStreams;
    watchedStreams.reserve(publicStreams.size());
    for (const auto &[streamId, stream] : publicStreams) {
      const bool canRead = !stream.closeAfterFlush &&
                           stream.sendWindow.available() > 0 &&
                           tunnelCanAcceptData(tunnelOutput);
      short events = canRead ? POLLIN : 0;
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
          nexus::logging::writeEvent(
              std::cerr, "edge", activeTunnelId, std::nullopt,
              "rejected another tunnel at the active limit");
        } else if (!authenticationLimiter.allowAttempt(
                       std::chrono::steady_clock::now())) {
          std::cerr << "Rejected tunnel while authentication rate limit is active"
                    << std::endl;
        } else if (!setNonBlocking(accepted.get())) {
          std::cerr << "Failed to make tunnel non-blocking" << std::endl;
        } else {
          tlsError.clear();
          auto acceptedTls = nexus::security::TlsConnection::createServer(
              *tlsContext, accepted.get(), tlsError);
          if (!acceptedTls || !nexus::net::performTlsHandshake(
                                  accepted.get(), *acceptedTls,
                                  kTlsHandshakeTimeout, tlsError)) {
            std::cerr << "Rejected tunnel with failed TLS handshake: "
                      << tlsError << std::endl;
            continue;
          }
          tlsError.clear();
          const auto authentication = nexus::security::authenticateServer(
              accepted.get(), *acceptedTls, authenticationToken,
              kAuthenticationTimeout, tlsError);
          if (authentication !=
              nexus::security::AuthenticationResult::Succeeded) {
            authenticationLimiter.recordFailure(
                std::chrono::steady_clock::now());
            std::cerr << "Rejected unauthenticated tunnel" << std::endl;
            continue;
          }
          authenticationLimiter.recordSuccess();
          // Only an encrypted, authenticated socket becomes the active tunnel.
          // Until this assignment, the public listener remains disabled.
          tunnelSocket = std::move(accepted);
          tunnelTls = std::move(*acceptedTls);
          activeTunnelId = nextTunnelId++;
          // Any valid tunnel traffic refreshes this deadline. Healthy clients
          // remain connected because their PING frames arrive well before it.
          tunnelIdleDeadline =
              std::chrono::steady_clock::now() + kTunnelIdleTimeout;
          nexus::logging::writeEvent(std::cout, "edge", activeTunnelId,
                                     std::nullopt, "developer tunnel connected");
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
        if (publicStreams.size() >= nexus::limits::kMaxStreamsPerTunnel) {
          nexus::logging::writeEvent(
              std::cerr, "edge", activeTunnelId, std::nullopt,
              "rejected public connection at the stream limit");
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
                        nexus::limits::kMaxTunnelQueueBytes)) {
          removePublicStream(publicStreams, streamId, activeTunnelId);
          continue;
        }
        nexus::logging::writeEvent(std::cout, "edge", activeTunnelId,
                                   streamId, "opened public stream");
      }
    }

    if (tunnelSocket &&
        tunnelPollIndex != std::numeric_limits<std::size_t>::max()) {
      const short events = pollFds[tunnelPollIndex].revents;

      if (!tunnelOutput.empty() &&
          ((events & POLLOUT) ||
           (tlsWriteWantsRead && (events & POLLIN)))) {
        tlsError.clear();
        const auto status = flushTlsOutput(*tunnelTls, tunnelOutput, tlsError);
        tlsWriteWantsRead =
            status == nexus::security::TlsStatus::WantRead;
        if (status != nexus::security::TlsStatus::Complete &&
            status != nexus::security::TlsStatus::WantRead &&
            status != nexus::security::TlsStatus::WantWrite) {
          std::cerr << "Encrypted tunnel write failed: " << tlsError
                    << std::endl;
          disconnectTunnel();
        }
      }

      if (tunnelSocket &&
          ((events & (POLLIN | POLLHUP)) ||
           (tlsReadWantsWrite && (events & POLLOUT)))) {
        std::uint8_t buffer[nexus::limits::kReadBufferBytes];
        while (true) {
          tlsError.clear();
          const auto result = tunnelTls->read(buffer, tlsError);
          if (result.status == nexus::security::TlsStatus::Complete &&
              result.bytes > 0) {
            tlsReadWantsWrite = false;
            tunnelIdleDeadline =
                std::chrono::steady_clock::now() + kTunnelIdleTimeout;
            tunnelDecoder.append(std::span<const std::uint8_t>(
                buffer, result.bytes));
            if (!handleTunnelFrames(tunnelDecoder, publicStreams, tunnelOutput,
                                    activeTunnelId)) {
              disconnectTunnel();
              break;
            }
            continue;
          }
          if (result.status == nexus::security::TlsStatus::WantRead) {
            tlsReadWantsWrite = false;
            break;
          }
          if (result.status == nexus::security::TlsStatus::WantWrite) {
            tlsReadWantsWrite = true;
            break;
          }
          if (result.status == nexus::security::TlsStatus::Closed) {
            disconnectTunnel();
            break;
          }
          std::cerr << "Encrypted tunnel read failed: " << tlsError
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
                             static_cast<bool>(tunnelSocket), activeTunnelId);
          continue;
        }
        if (stream->second.closeAfterFlush && stream->second.output.empty()) {
          removePublicStream(publicStreams, watched.streamId, activeTunnelId);
          continue;
        }
      }

      const auto current = publicStreams.find(watched.streamId);
      if (current == publicStreams.end()) {
        continue;
      }

      if (events & (POLLIN | POLLHUP)) {
        std::uint8_t buffer[nexus::limits::kReadBufferBytes];
        bool streamFinished = false;
        while (!streamFinished) {
          if (!tunnelCanAcceptData(tunnelOutput)) {
            break;
          }
          const std::size_t allowed =
              current->second.sendWindow.allowedRead(sizeof(buffer));
          if (allowed == 0) {
            break;
          }
          const ssize_t received =
              recv(current->second.socket.get(), buffer, allowed, 0);
          if (received > 0) {
            const nexus::protocol::Frame dataFrame{
                nexus::protocol::MessageType::Data,
                watched.streamId,
                {buffer, buffer + received}};
            if (!queueFrame(tunnelOutput, dataFrame,
                            nexus::limits::kMaxTunnelQueueBytes)) {
              std::cerr << "Failed to encode DATA for stream "
                        << watched.streamId << std::endl;
              running = false;
              streamFinished = true;
            } else if (!current->second.sendWindow.consume(
                           static_cast<std::size_t>(received))) {
              std::cerr << "Public stream " << watched.streamId
                        << " consumed more flow-control credit than available"
                        << std::endl;
              running = false;
              streamFinished = true;
            }
            continue;
          }
          if (received == 0) {
            finishPublicStream(publicStreams, tunnelOutput, watched.streamId,
                               static_cast<bool>(tunnelSocket), activeTunnelId);
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
                             static_cast<bool>(tunnelSocket), activeTunnelId);
          streamFinished = true;
        }
      }

      if (publicStreams.contains(watched.streamId) &&
          (events & (POLLERR | POLLNVAL))) {
        finishPublicStream(publicStreams, tunnelOutput, watched.streamId,
                           static_cast<bool>(tunnelSocket), activeTunnelId);
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
    tlsError.clear();
    drainTlsOutput(tunnelSocket.get(), *tunnelTls, tunnelOutput, 1s, tlsError);
    tunnelTls->shutdown();
  }
  closeAllPublicStreams(publicStreams, activeTunnelId);
  if (nexus::process::shutdownRequested()) {
    std::cout << "Edge shutdown complete" << std::endl;
  }
  return running ? 0 : 1;
}
