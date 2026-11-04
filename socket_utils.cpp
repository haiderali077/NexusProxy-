#include "socket_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nexus::net {
namespace {

void closePreservingError(int socketFd) {
    const int savedError = errno;
    close(socketFd);
    errno = savedError;
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

} // namespace nexus::net
