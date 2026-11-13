#include "nexus/tls_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <poll.h>

namespace nexus::net {
namespace {

int millisecondsUntil(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        return 0;
    }
    return static_cast<int>(std::min<std::int64_t>(
        remaining.count(), std::numeric_limits<int>::max()));
}

short eventFor(security::TlsStatus status) {
    return status == security::TlsStatus::WantWrite ? POLLOUT : POLLIN;
}

bool waitForTls(int socketFd, security::TlsStatus status,
                std::chrono::steady_clock::time_point deadline,
                std::string& error) {
    while (true) {
        const int timeout = millisecondsUntil(deadline);
        if (timeout == 0) {
            error = "TLS operation timed out";
            return false;
        }

        pollfd pending{socketFd, eventFor(status), 0};
        const int ready = poll(&pending, 1, timeout);
        if (ready == -1 && errno == EINTR) {
            continue;
        }
        if (ready == -1) {
            error = std::string{"poll() failed during TLS operation: "} +
                    std::strerror(errno);
            return false;
        }
        if (ready == 0) {
            error = "TLS operation timed out";
            return false;
        }
        if (pending.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            error = "socket closed during TLS operation";
            return false;
        }
        return true;
    }
}

} // namespace

bool performTlsHandshake(int socketFd, security::TlsConnection& connection,
                         std::chrono::milliseconds timeout,
                         std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const security::TlsStatus status = connection.handshake(error);
        if (status == security::TlsStatus::Complete) {
            return true;
        }
        if (status != security::TlsStatus::WantRead &&
            status != security::TlsStatus::WantWrite) {
            if (error.empty()) {
                error = "TLS handshake ended before completion";
            }
            return false;
        }
        if (!waitForTls(socketFd, status, deadline, error)) {
            return false;
        }
    }
}

security::TlsStatus flushTlsOutput(security::TlsConnection& connection,
                                   OutputQueue& output, std::string& error) {
    while (!output.empty()) {
        const auto result = connection.write(
            std::span<const std::uint8_t>(output.data(), output.pending()),
            error);
        if (result.status != security::TlsStatus::Complete) {
            return result.status;
        }
        if (result.bytes == 0) {
            error = "TLS write completed without consuming queued bytes";
            return security::TlsStatus::Error;
        }
        output.consume(result.bytes);
    }
    return security::TlsStatus::Complete;
}

bool drainTlsOutput(int socketFd, security::TlsConnection& connection,
                    OutputQueue& output, std::chrono::milliseconds timeout,
                    std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!output.empty()) {
        const security::TlsStatus status =
            flushTlsOutput(connection, output, error);
        if (status == security::TlsStatus::Complete) {
            return true;
        }
        if (status != security::TlsStatus::WantRead &&
            status != security::TlsStatus::WantWrite) {
            return false;
        }
        if (!waitForTls(socketFd, status, deadline, error)) {
            return false;
        }
    }
    return true;
}

} // namespace nexus::net
