#include "unique_socket.hpp"

#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {

using nexus::net::UniqueSocket;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void expectClosed(int socketFd, std::string_view message) {
    errno = 0;
    expect(fcntl(socketFd, F_GETFD) == -1 && errno == EBADF, message);
}

void testDestructorClosesDescriptor() {
    int descriptors[2]{};
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
           "socketpair creation failed");

    const int ownedDescriptor = descriptors[0];
    {
        UniqueSocket socket{ownedDescriptor};
        expect(socket.valid(), "new owner should be valid");
        expect(socket.get() == ownedDescriptor, "owner stored wrong descriptor");
    }

    expectClosed(ownedDescriptor, "destructor did not close descriptor");
    close(descriptors[1]);
}

void testMoveTransfersOwnership() {
    int descriptors[2]{};
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
           "socketpair creation failed");

    const int ownedDescriptor = descriptors[0];
    UniqueSocket source{ownedDescriptor};
    UniqueSocket destination{std::move(source)};

    expect(!source.valid(), "move constructor did not empty source");
    expect(destination.get() == ownedDescriptor,
           "move constructor lost descriptor");

    UniqueSocket assigned;
    assigned = std::move(destination);
    expect(!destination.valid(), "move assignment did not empty source");
    expect(assigned.get() == ownedDescriptor,
           "move assignment lost descriptor");

    close(descriptors[1]);
}

void testReleaseReturnsOwnership() {
    int descriptors[2]{};
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
           "socketpair creation failed");

    UniqueSocket socket{descriptors[0]};
    const int released = socket.release();
    expect(!socket.valid(), "release did not empty owner");
    expect(fcntl(released, F_GETFD) != -1,
           "release unexpectedly closed descriptor");

    close(released);
    close(descriptors[1]);
}

void testResetClosesOldDescriptor() {
    int firstPair[2]{};
    int secondPair[2]{};
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, firstPair) == 0,
           "first socketpair creation failed");
    expect(socketpair(AF_UNIX, SOCK_STREAM, 0, secondPair) == 0,
           "second socketpair creation failed");

    const int oldDescriptor = firstPair[0];
    UniqueSocket socket{oldDescriptor};
    socket.reset(secondPair[0]);

    expectClosed(oldDescriptor, "reset did not close old descriptor");
    expect(socket.get() == secondPair[0], "reset did not adopt new descriptor");

    close(firstPair[1]);
    close(secondPair[1]);
}

} // namespace

int main() {
    try {
        testDestructorClosesDescriptor();
        testMoveTransfersOwnership();
        testReleaseReturnsOwnership();
        testResetClosesOldDescriptor();
    } catch (const std::exception& error) {
        std::cerr << "Socket ownership test failed: " << error.what() << std::endl;
        return 1;
    }

    std::cout << "All socket ownership tests passed" << std::endl;
    return 0;
}
