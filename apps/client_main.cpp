// NexusProxy developer client
//
// One persistent connection reaches the public edge. Each OPEN frame creates a
// separate connection to the local service, and stream IDs route DATA frames.

#include "nexus/config.hpp"
#include "nexus/flow_control.hpp"
#include "nexus/authentication.hpp"
#include "nexus/heartbeat.hpp"
#include "nexus/limits.hpp"
#include "nexus/logging.hpp"
#include "nexus/protocol.hpp"
#include "nexus/relay.hpp"
#include "nexus/retry.hpp"
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
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using nexus::net::connectTcp;
using nexus::net::drainTlsOutput;
using nexus::net::flushTlsOutput;
using nexus::net::flushOutput;
using nexus::net::FlushResult;
using nexus::net::OutputQueue;
using nexus::net::queueFrame;
using nexus::net::setNonBlocking;
using nexus::net::UniqueSocket;
using nexus::reliability::ReconnectBackoff;

using namespace std::chrono_literals;

constexpr const char *kEdgeAddress = "127.0.0.1";
constexpr int kTunnelPort = 7000;
constexpr auto kHeartbeatInterval = 5s;
constexpr auto kPongTimeout = 15s;
constexpr auto kConnectTimeout = 5s;
constexpr auto kTlsHandshakeTimeout = 5s;
constexpr auto kAuthenticationTimeout = 5s;

struct LocalStream {
  UniqueSocket socket;
  OutputQueue output;
  bool closeAfterFlush{};
  nexus::flow::SendWindow sendWindow{
      nexus::limits::kMaxStreamQueueBytes};
};

bool tunnelCanAcceptData(const OutputQueue &output) {
  constexpr std::size_t required = nexus::protocol::kFrameHeaderSize +
                                   nexus::limits::kReadBufferBytes +
                                   nexus::limits::kTunnelControlReserveBytes;
  return output.canAppend(required, nexus::limits::kMaxTunnelQueueBytes);
}

bool queueClose(OutputQueue &tunnelOutput, std::uint32_t streamId) {
  return queueFrame(tunnelOutput,
                    {nexus::protocol::MessageType::Close, streamId, {}},
                    nexus::limits::kMaxTunnelQueueBytes);
}

void removeLocalStream(std::unordered_map<std::uint32_t, LocalStream> &streams,
                       std::uint32_t streamId, std::uint64_t tunnelId) {
  const auto stream = streams.find(streamId);
  if (stream == streams.end()) {
    return;
  }
  streams.erase(stream);
  nexus::logging::writeEvent(std::cout, "client", tunnelId, streamId,
                             "closed local stream");
}

void closeAllLocalStreams(
    std::unordered_map<std::uint32_t, LocalStream> &streams,
    std::uint64_t tunnelId) {
  for (const auto &entry : streams) {
    nexus::logging::writeEvent(std::cout, "client", tunnelId, entry.first,
                               "closed local stream");
  }
  streams.clear();
}

bool handleTunnelFrames(nexus::protocol::FrameDecoder &decoder,
                        std::unordered_map<std::uint32_t, LocalStream> &streams,
                        OutputQueue &tunnelOutput, const char *localAddress,
                        int localPort,
                        std::optional<std::uint64_t> &awaitedPong,
                        std::chrono::steady_clock::time_point &nextHeartbeat,
                        std::uint64_t tunnelId) {
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

    if (frame.type == nexus::protocol::MessageType::Pong) {
      std::uint64_t token = 0;
      if (!nexus::reliability::decodeHeartbeatToken(frame.payload, token) ||
          !awaitedPong || token != *awaitedPong) {
        std::cerr << "Received a PONG that does not match the active PING"
                  << std::endl;
        return false;
      }
      // A matching reply proves this tunnel can still carry data both ways.
      awaitedPong.reset();
      nextHeartbeat = std::chrono::steady_clock::now() + kHeartbeatInterval;
      continue;
    }

    if (frame.type == nexus::protocol::MessageType::Open) {
      if (streams.contains(frame.streamId)) {
        std::cerr << "Received duplicate OPEN for stream " << frame.streamId
                  << std::endl;
        return false;
      }
      if (streams.size() >= nexus::limits::kMaxStreamsPerTunnel) {
        nexus::logging::writeEvent(std::cerr, "client", tunnelId,
                                   frame.streamId,
                                   "rejected stream at the active limit");
        queueClose(tunnelOutput, frame.streamId);
        continue;
      }

      UniqueSocket localSocket{
          connectTcp(localAddress, localPort, kConnectTimeout)};
      if (!localSocket) {
        std::cerr << "Failed to connect stream " << frame.streamId << " to "
                  << localAddress << ':' << localPort << ": "
                  << std::strerror(errno) << std::endl;
        queueClose(tunnelOutput, frame.streamId);
        continue;
      }
      if (!setNonBlocking(localSocket.get())) {
        std::cerr << "Failed to make local stream non-blocking" << std::endl;
        queueClose(tunnelOutput, frame.streamId);
        continue;
      }

      streams.emplace(frame.streamId,
                      LocalStream{std::move(localSocket), {}, false});
      nexus::logging::writeEvent(std::cout, "client", tunnelId,
                                 frame.streamId, "opened local stream");
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
        std::cerr << "Invalid WINDOW_UPDATE for local stream "
                  << frame.streamId << std::endl;
        return false;
      }
      continue;
    }

    if (frame.type == nexus::protocol::MessageType::Data) {
      if (!stream->second.output.append(
              frame.payload, nexus::limits::kMaxStreamQueueBytes)) {
        std::cerr << "Local stream " << frame.streamId
                  << " exceeded its output limit" << std::endl;
        queueClose(tunnelOutput, frame.streamId);
        removeLocalStream(streams, frame.streamId, tunnelId);
      }
      continue;
    }
    if (frame.type == nexus::protocol::MessageType::Close) {
      if (stream->second.output.empty()) {
        removeLocalStream(streams, frame.streamId, tunnelId);
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

void waitBeforeReconnect(ReconnectBackoff &backoff, std::string_view reason) {
  const std::chrono::milliseconds delay = backoff.nextDelay();
  std::cout << reason << "; reconnecting in " << delay.count() << " ms"
            << std::endl;
  const auto deadline = std::chrono::steady_clock::now() + delay;
  while (!nexus::process::shutdownRequested() &&
         std::chrono::steady_clock::now() < deadline) {
    // Short sleeps let SIGINT and SIGTERM interrupt even the longest backoff.
    std::this_thread::sleep_for(100ms);
  }
}

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

  const char *localAddress = "127.0.0.1";
  int localPort = 9000;
  const char *edgeAddress = kEdgeAddress;
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

  const char *trustedCaFile = std::getenv("NEXUS_TLS_CA_FILE");
  const char *configuredServerName = std::getenv("NEXUS_TLS_SERVER_NAME");
  if (trustedCaFile == nullptr || *trustedCaFile == '\0') {
    std::cerr << "NEXUS_TLS_CA_FILE must name the trusted CA certificate"
              << std::endl;
    return 1;
  }
  const std::string expectedServerName =
      configuredServerName == nullptr || *configuredServerName == '\0'
          ? edgeAddress
          : configuredServerName;
  std::string tlsError;
  auto tlsContext = nexus::security::TlsContext::createClient(trustedCaFile,
                                                               tlsError);
  if (!tlsContext) {
    std::cerr << "Failed to configure client TLS: " << tlsError << std::endl;
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

  ReconnectBackoff reconnectBackoff{250ms, 10s};
  std::uint64_t nextTunnelId = 1;

  // Every pass owns one complete tunnel session. Returning here after a
  // disconnect would make a temporary network problem stop the client.
  while (!nexus::process::shutdownRequested()) {
    UniqueSocket tunnelSocket{
        connectTcp(edgeAddress, tunnelPort, kConnectTimeout)};
    if (!tunnelSocket) {
      std::cerr << "Failed to connect to edge " << edgeAddress << ':'
                << tunnelPort << ": " << std::strerror(errno) << std::endl;
      waitBeforeReconnect(reconnectBackoff, "Edge is unavailable");
      continue;
    }
    if (!setNonBlocking(tunnelSocket.get())) {
      std::cerr << "Failed to make the tunnel non-blocking" << std::endl;
      waitBeforeReconnect(reconnectBackoff, "Tunnel setup did not complete");
      continue;
    }

    tlsError.clear();
    auto tunnelTls = nexus::security::TlsConnection::createClient(
        *tlsContext, tunnelSocket.get(), expectedServerName, tlsError);
    if (!tunnelTls || !nexus::net::performTlsHandshake(
                          tunnelSocket.get(), *tunnelTls,
                          kTlsHandshakeTimeout, tlsError) ||
        !tunnelTls->peerCertificateVerified()) {
      std::cerr << "TLS connection to the edge failed: " << tlsError
                << std::endl;
      waitBeforeReconnect(reconnectBackoff, "Secure tunnel setup failed");
      continue;
    }
    tlsError.clear();
    const auto authentication = nexus::security::authenticateClient(
        tunnelSocket.get(), *tunnelTls, authenticationToken,
        kAuthenticationTimeout, tlsError);
    if (authentication !=
        nexus::security::AuthenticationResult::Succeeded) {
      std::cerr << "Tunnel authentication failed: " << tlsError << std::endl;
      waitBeforeReconnect(reconnectBackoff, "Secure tunnel setup failed");
      continue;
    }

    // Reset only after the edge identity and client token are both accepted.
    reconnectBackoff.reset();
    const std::uint64_t tunnelId = nextTunnelId++;
    nexus::logging::writeEvent(std::cout, "client", tunnelId, std::nullopt,
                               "connected to edge");
    std::cout << "Exposing " << localAddress << ':' << localPort << std::endl;

    // Decoder bytes, output bytes, and local sockets belong to this tunnel
    // only. Recreating them prevents stale state entering the next session.
    std::unordered_map<std::uint32_t, LocalStream> localStreams;
    nexus::protocol::FrameDecoder tunnelDecoder;
    OutputQueue tunnelOutput;
    bool relaySucceeded = true;
    bool sessionRunning = true;
    std::uint64_t heartbeatToken = 1;
    std::optional<std::uint64_t> awaitedPong;
    auto nextHeartbeat = std::chrono::steady_clock::now();
    auto pongDeadline = nextHeartbeat;
    bool tlsReadWantsWrite = false;
    bool tlsWriteWantsRead = false;

    while (sessionRunning && !nexus::process::shutdownRequested()) {
      const auto now = std::chrono::steady_clock::now();
      if (awaitedPong && now >= pongDeadline) {
        std::cerr << "The edge did not answer PING before its deadline"
                  << std::endl;
        relaySucceeded = false;
        break;
      }
      if (!awaitedPong && now >= nextHeartbeat) {
        const std::uint64_t token = heartbeatToken++;
        const nexus::protocol::Frame ping{
            nexus::protocol::MessageType::Ping, 0,
            nexus::reliability::encodeHeartbeatToken(token)};
        if (!queueFrame(tunnelOutput, ping,
                        nexus::limits::kMaxTunnelQueueBytes)) {
          std::cerr << "Could not queue PING; tunnel output is full"
                    << std::endl;
          relaySucceeded = false;
          break;
        }
        awaitedPong = token;
        pongDeadline = now + kPongTimeout;
      }

      std::vector<pollfd> pollFds;
      short tunnelEvents = POLLIN;
      if (!tunnelOutput.empty() || tlsReadWantsWrite) {
        tunnelEvents |= POLLOUT;
      }
      pollFds.push_back({tunnelSocket.get(), tunnelEvents, 0});

      std::vector<WatchedStream> watchedStreams;
      watchedStreams.reserve(localStreams.size());
      for (const auto &[streamId, stream] : localStreams) {
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
        const auto wakeDeadline = awaitedPong ? pongDeadline : nextHeartbeat;
        ready = poll(pollFds.data(), pollFds.size(),
                     pollTimeoutUntil(wakeDeadline));
      } while (ready == -1 && errno == EINTR &&
               !nexus::process::shutdownRequested());

      if (nexus::process::shutdownRequested()) {
        break;
      }

      if (ready == -1) {
        std::cerr << "poll() failed: " << std::strerror(errno) << std::endl;
        relaySucceeded = false;
        break;
      }

      const short tunnelReady = pollFds[0].revents;
      if (!tunnelOutput.empty() &&
          ((tunnelReady & POLLOUT) ||
           (tlsWriteWantsRead && (tunnelReady & POLLIN)))) {
        tlsError.clear();
        const auto status = flushTlsOutput(*tunnelTls, tunnelOutput, tlsError);
        tlsWriteWantsRead =
            status == nexus::security::TlsStatus::WantRead;
        if (status != nexus::security::TlsStatus::Complete &&
            status != nexus::security::TlsStatus::WantRead &&
            status != nexus::security::TlsStatus::WantWrite) {
          std::cerr << "Failed to flush encrypted tunnel output: " << tlsError
                    << std::endl;
          relaySucceeded = false;
          break;
        }
      }

      if ((tunnelReady & (POLLIN | POLLHUP)) ||
          (tlsReadWantsWrite && (tunnelReady & POLLOUT))) {
        std::uint8_t buffer[nexus::limits::kReadBufferBytes];
        while (true) {
          tlsError.clear();
          const auto result = tunnelTls->read(buffer, tlsError);
          if (result.status == nexus::security::TlsStatus::Complete &&
              result.bytes > 0) {
            tlsReadWantsWrite = false;
            tunnelDecoder.append(std::span<const std::uint8_t>(
                buffer, result.bytes));
            if (!handleTunnelFrames(tunnelDecoder, localStreams, tunnelOutput,
                                    localAddress, localPort, awaitedPong,
                                    nextHeartbeat, tunnelId)) {
              relaySucceeded = false;
              sessionRunning = false;
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
            std::cout << "Edge closed the tunnel" << std::endl;
            sessionRunning = false;
            break;
          }
          std::cerr << "Encrypted tunnel read failed: " << tlsError
                    << std::endl;
          relaySucceeded = false;
          sessionRunning = false;
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
              flushOutput(stream->second.socket.get(), stream->second.output);
          if (result != FlushResult::Ready) {
            queueClose(tunnelOutput, watched.streamId);
            removeLocalStream(localStreams, watched.streamId, tunnelId);
            continue;
          }
          if (stream->second.closeAfterFlush && stream->second.output.empty()) {
            removeLocalStream(localStreams, watched.streamId, tunnelId);
            continue;
          }
        }

        const auto current = localStreams.find(watched.streamId);
        if (current == localStreams.end()) {
          continue;
        }

        if (events & (POLLIN | POLLHUP)) {
          std::uint8_t buffer[nexus::limits::kReadBufferBytes];
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
              if (!queueFrame(tunnelOutput, dataFrame,
                              nexus::limits::kMaxTunnelQueueBytes)) {
                std::cerr << "Failed to encode DATA for stream "
                          << watched.streamId << std::endl;
                relaySucceeded = false;
                sessionRunning = false;
                streamFinished = true;
              }
              continue;
            }
            if (received == 0) {
              queueClose(tunnelOutput, watched.streamId);
              removeLocalStream(localStreams, watched.streamId, tunnelId);
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
            std::cerr << "Local recv() failed for stream " << watched.streamId
                      << ": " << std::strerror(errno) << std::endl;
            queueClose(tunnelOutput, watched.streamId);
            removeLocalStream(localStreams, watched.streamId, tunnelId);
            streamFinished = true;
          }
        }

        if (localStreams.contains(watched.streamId) &&
            (events & (POLLERR | POLLNVAL))) {
          queueClose(tunnelOutput, watched.streamId);
          removeLocalStream(localStreams, watched.streamId, tunnelId);
        }
      }
    }

    if (nexus::process::shutdownRequested()) {
      // Tell the edge which streams are ending, but never let shutdown wait
      // forever for a peer that may already be gone.
      for (const auto &[streamId, stream] : localStreams) {
        static_cast<void>(stream);
        queueClose(tunnelOutput, streamId);
      }
      tlsError.clear();
      drainTlsOutput(tunnelSocket.get(), *tunnelTls, tunnelOutput, 1s,
                     tlsError);
      tunnelTls->shutdown();
      closeAllLocalStreams(localStreams, tunnelId);
      break;
    }

    closeAllLocalStreams(localStreams, tunnelId);
    waitBeforeReconnect(reconnectBackoff, relaySucceeded
                                              ? "Tunnel disconnected"
                                              : "Tunnel session failed");
  }

  std::cout << "Client shutdown complete" << std::endl;
  return 0;
}
