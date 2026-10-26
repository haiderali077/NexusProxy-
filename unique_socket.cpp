#include "unique_socket.hpp"

#include <unistd.h>
#include <utility>

namespace nexus::net {

UniqueSocket::UniqueSocket(int socketFd) noexcept : socketFd_(socketFd) {}

UniqueSocket::~UniqueSocket() {
    reset();
}

UniqueSocket::UniqueSocket(UniqueSocket&& other) noexcept
    : socketFd_(std::exchange(other.socketFd_, -1)) {}

UniqueSocket& UniqueSocket::operator=(UniqueSocket&& other) noexcept {
    if (this != &other) {
        reset(std::exchange(other.socketFd_, -1));
    }
    return *this;
}

int UniqueSocket::get() const noexcept {
    return socketFd_;
}

bool UniqueSocket::valid() const noexcept {
    return socketFd_ != -1;
}

UniqueSocket::operator bool() const noexcept {
    return valid();
}

int UniqueSocket::release() noexcept {
    return std::exchange(socketFd_, -1);
}

void UniqueSocket::reset(int socketFd) noexcept {
    if (socketFd_ != -1) {
        close(socketFd_);
    }
    socketFd_ = socketFd;
}

} // namespace nexus::net
