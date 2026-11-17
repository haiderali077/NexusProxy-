#include "nexus/authentication.hpp"

#include "nexus/limits.hpp"
#include "nexus/protocol.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <poll.h>
#include <span>
#include <vector>

namespace nexus::security {
namespace {

void wipe(std::vector<std::uint8_t>& bytes) noexcept {
    if (!bytes.empty()) {
        OPENSSL_cleanse(bytes.data(), bytes.size());
        bytes.clear();
    }
}

int millisecondsUntil(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        return 0;
    }
    return static_cast<int>(std::min<std::int64_t>(
        remaining.count(), std::numeric_limits<int>::max()));
}

bool waitFor(int socketFd, TlsStatus status,
             std::chrono::steady_clock::time_point deadline,
             std::string& error) {
    while (true) {
        const int timeout = millisecondsUntil(deadline);
        if (timeout == 0) {
            error = "authentication timed out";
            return false;
        }
        const short requested =
            status == TlsStatus::WantWrite ? POLLOUT : POLLIN;
        pollfd pending{socketFd, requested, 0};
        const int ready = poll(&pending, 1, timeout);
        if (ready == -1 && errno == EINTR) {
            continue;
        }
        if (ready == -1) {
            error = std::string{"poll() failed during authentication: "} +
                    std::strerror(errno);
            return false;
        }
        if (ready == 0) {
            error = "authentication timed out";
            return false;
        }
        if (pending.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            error = "connection closed during authentication";
            return false;
        }
        return true;
    }
}

bool sendFrame(int socketFd, TlsConnection& connection,
               const protocol::Frame& frame,
               std::chrono::steady_clock::time_point deadline,
               std::string& error) {
    std::vector<std::uint8_t> encoded;
    if (!protocol::encodeFrame(frame, encoded)) {
        error = "could not encode authentication frame";
        return false;
    }

    std::size_t offset = 0;
    while (offset < encoded.size()) {
        const auto result = connection.write(
            std::span<const std::uint8_t>(encoded).subspan(offset), error);
        if (result.status == TlsStatus::Complete) {
            if (result.bytes == 0) {
                error = "authentication write made no progress";
                wipe(encoded);
                return false;
            }
            offset += result.bytes;
            continue;
        }
        if (result.status != TlsStatus::WantRead &&
            result.status != TlsStatus::WantWrite) {
            wipe(encoded);
            return false;
        }
        if (!waitFor(socketFd, result.status, deadline, error)) {
            wipe(encoded);
            return false;
        }
    }
    wipe(encoded);
    return true;
}

AuthenticationResult receiveFrame(
    int socketFd, TlsConnection& connection, protocol::Frame& frame,
    std::chrono::steady_clock::time_point deadline, std::string& error) {
    protocol::FrameDecoder decoder;
    std::uint8_t buffer[limits::kReadBufferBytes];
    while (true) {
        const auto decoded = decoder.nextFrame(frame);
        if (decoded == protocol::DecodeStatus::FrameReady) {
            return AuthenticationResult::Succeeded;
        }
        if (decoded == protocol::DecodeStatus::ProtocolError) {
            error = std::string{decoder.error()};
            return AuthenticationResult::Error;
        }

        const auto result = connection.read(buffer, error);
        if (result.status == TlsStatus::Complete) {
            if (result.bytes == 0) {
                error = "authentication read made no progress";
                return AuthenticationResult::Error;
            }
            decoder.append(std::span<const std::uint8_t>(buffer, result.bytes));
            continue;
        }
        if (result.status == TlsStatus::Closed) {
            return AuthenticationResult::Closed;
        }
        if (result.status != TlsStatus::WantRead &&
            result.status != TlsStatus::WantWrite) {
            return AuthenticationResult::Error;
        }
        if (!waitFor(socketFd, result.status, deadline, error)) {
            return AuthenticationResult::Error;
        }
    }
}

} // namespace

AuthenticationResult authenticateClient(
    int socketFd, TlsConnection& connection, const SecureToken& token,
    std::chrono::milliseconds timeout, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    protocol::Frame request{protocol::MessageType::Authenticate, 0,
                            {token.bytes().begin(), token.bytes().end()}};
    if (!sendFrame(socketFd, connection, request, deadline, error)) {
        wipe(request.payload);
        return AuthenticationResult::Error;
    }
    wipe(request.payload);

    protocol::Frame response;
    const AuthenticationResult received =
        receiveFrame(socketFd, connection, response, deadline, error);
    if (received != AuthenticationResult::Succeeded) {
        return received;
    }
    if (response.type == protocol::MessageType::AuthenticateOk) {
        return AuthenticationResult::Succeeded;
    }
    if (response.type == protocol::MessageType::AuthenticateFailed) {
        error = "edge rejected authentication";
        return AuthenticationResult::Rejected;
    }
    error = "edge sent an unexpected frame before authentication";
    return AuthenticationResult::Error;
}

AuthenticationResult authenticateServer(
    int socketFd, TlsConnection& connection, const SecureToken& expectedToken,
    std::chrono::milliseconds timeout, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    protocol::Frame request;
    const AuthenticationResult received =
        receiveFrame(socketFd, connection, request, deadline, error);
    if (received != AuthenticationResult::Succeeded) {
        return received;
    }
    if (request.type != protocol::MessageType::Authenticate) {
        error = "client sent an unexpected frame before authentication";
        wipe(request.payload);
        return AuthenticationResult::Error;
    }

    const bool accepted = expectedToken.matches(request.payload);
    wipe(request.payload);
    const protocol::Frame response{
        accepted ? protocol::MessageType::AuthenticateOk
                 : protocol::MessageType::AuthenticateFailed,
        0, {}};
    if (!sendFrame(socketFd, connection, response, deadline, error)) {
        return AuthenticationResult::Error;
    }
    if (!accepted) {
        error = "client authentication failed";
        return AuthenticationResult::Rejected;
    }
    return AuthenticationResult::Succeeded;
}

} // namespace nexus::security
