#include "nexus/socket_utils.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nexus::net {
namespace {

void closePreservingError(int socketFd) {
    const int savedError = errno;
    close(socketFd);
    errno = savedError;
}

int remainingMilliseconds(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        return 0;
    }
    return static_cast<int>(std::min<std::int64_t>(
        remaining.count(), std::numeric_limits<int>::max()));
}

} // namespace

bool setNonBlocking(int socketFd) {
    const int flags = fcntl(socketFd, F_GETFL, 0);
    return flags != -1 &&
           fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) != -1;
}

int createTcpListener(int port) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        return -1;
    }

    int option = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &option,
                   sizeof(option)) == -1) {
        closePreservingError(listener);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == -1 ||
        listen(listener, SOMAXCONN) == -1 || !setNonBlocking(listener)) {
        closePreservingError(listener);
        return -1;
    }

    return listener;
}

int connectTcp(const char* address, int port) {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd == -1) {
        return -1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &serverAddress.sin_addr) != 1) {
        errno = EINVAL;
        closePreservingError(socketFd);
        return -1;
    }

    if (connect(socketFd, reinterpret_cast<sockaddr*>(&serverAddress),
                sizeof(serverAddress)) == -1) {
        closePreservingError(socketFd);
        return -1;
    }

    return socketFd;
}

int connectTcp(const char* address, int port,
               std::chrono::milliseconds timeout) {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd == -1) {
        return -1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &serverAddress.sin_addr) != 1) {
        errno = EINVAL;
        closePreservingError(socketFd);
        return -1;
    }
    if (!setNonBlocking(socketFd)) {
        closePreservingError(socketFd);
        return -1;
    }

    if (connect(socketFd, reinterpret_cast<sockaddr*>(&serverAddress),
                sizeof(serverAddress)) == 0) {
        return socketFd;
    }
    if (errno != EINPROGRESS) {
        closePreservingError(socketFd);
        return -1;
    }

    // A non-blocking connect finishes through POLLOUT. The deadline keeps an
    // unreachable address from freezing the whole client.
    pollfd pending{socketFd, POLLOUT, 0};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const int ready = poll(&pending, 1, remainingMilliseconds(deadline));
        if (ready == -1 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            closePreservingError(socketFd);
            return -1;
        }
        if (ready == -1) {
            closePreservingError(socketFd);
            return -1;
        }
        break;
    }

    int connectionError = 0;
    socklen_t errorSize = sizeof(connectionError);
    if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &connectionError,
                   &errorSize) == -1) {
        closePreservingError(socketFd);
        return -1;
    }
    if (connectionError != 0) {
        errno = connectionError;
        closePreservingError(socketFd);
        return -1;
    }

    return socketFd;
}

} // namespace nexus::net
