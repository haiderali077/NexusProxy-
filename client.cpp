// NexusProxy developer client
// Connects to the edge and a local service. After the edge announces a public
// connection, it relays request and response bytes between the tunnel and the
// local service.

#include "protocol.hpp"

#include <array>        // std::array stores the fixed-size OPEN frame header.
#include <arpa/inet.h>  // inet_pton() converts an IP string to binary.
#include <cerrno>       // errno and EINTR identify interrupted system calls.
#include <csignal>      // SIGPIPE handling keeps failed sends recoverable.
#include <cstddef>      // std::size_t stores buffer lengths and byte counts.
#include <cstring>      // std::strerror() turns errno into a readable message.
#include <iostream>     // Prints progress and error messages.
#include <netinet/in.h> // sockaddr_in describes an IPv4 endpoint.
#include <string>       // std::stoi() converts a string to an integer.
#include <sys/socket.h> // socket() and connect().
#include <unistd.h>     // close() releases a file descriptor.
#include <poll.h>       // poll() watches several sockets at the same time.
#include <unordered_map> // Connects each stream ID to its local socket.
#include <vector>       // Holds encoded frames and owned payload bytes.

namespace {

[[maybe_unused]] constexpr const char* kEdgeAddress = "127.0.0.1";
[[maybe_unused]] constexpr int kTunnelPort = 7000;

// Both the edge and the local application are TCP endpoints, so this helper is
// shared by both connections. The caller owns a successful returned descriptor.
[[maybe_unused]] int connectTcp(const char* address, int port) {
    // This creates an IPv4 TCP socket. It is not connected until connect()
    // succeeds below.

    const int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == -1) {
        std::cerr << "Failed to create Tcp socket\n";
        return -1;
    }

    // sockaddr_in stores the server's IPv4 address and port. htons() converts
    // the port to network byte order, while inet_pton() converts the readable IP
    // address into the binary form expected by connect().

    struct sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &serverAddress.sin_addr) <= 0) {
        std::cerr << "Invalid address: " << address << std::endl;
        close(clientSocket);
        return -1;
    }
    // connect() needs the socket, the server address, and the size of that
    // address. Success establishes TCP but does not exchange application data.

    int result = connect(clientSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress));
    if (result == -1) {
        std::cerr << "Failed to connect to Tcp endpoint: " << address << ":" << port << std::endl;
        close(clientSocket);
        return -1;
    }
    // Do not close a successful socket here because main() still needs it.
    return clientSocket;
}

} // namespace

bool sendAll(int socketFd, const char* data, std::size_t length) {
    std::size_t bytesSent = 0;

    while (bytesSent < length) {
        ssize_t result = send(socketFd, data + bytesSent, length - bytesSent, 0);

        if (result == -1) {
            if (errno == EINTR) {
                // A signal interrupted send() before completion, so try again.
                continue;
            }
            return false;
        }
        if (result == 0) {
            // No progress means the complete buffer cannot be delivered.
            return false;
        }

        bytesSent += static_cast<std::size_t>(result);
    }

    return true;
}

bool sendFrame(int socketFd, const nexus::protocol::Frame& frame) {
    std::vector<std::uint8_t> bytes;
    if (!nexus::protocol::encodeFrame(frame, bytes)) {
        return false;
    }

    return sendAll(socketFd, reinterpret_cast<const char*>(bytes.data()),
                   bytes.size());
}

// OPEN has an empty payload, so its complete wire representation is exactly one
// fixed-size header. Reading the exact size avoids consuming later relay bytes.
bool receiveOpenFrame(int tunnelSocket, nexus::protocol::Frame& frame) {
    std::array<std::uint8_t, nexus::protocol::kFrameHeaderSize> bytes{};
    std::size_t received = 0;

    while (received < bytes.size()) {
        const ssize_t result = recv(tunnelSocket, bytes.data() + received,
                                    bytes.size() - received, 0);
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Failed to receive OPEN frame: "
                      << std::strerror(errno) << std::endl;
            return false;
        }
        if (result == 0) {
            std::cerr << "The edge closed the tunnel before sending OPEN"
                      << std::endl;
            return false;
        }
        received += static_cast<std::size_t>(result);
    }

    nexus::protocol::FrameDecoder decoder;
    decoder.append(bytes);
    if (decoder.nextFrame(frame) != nexus::protocol::DecodeStatus::FrameReady ||
        frame.type != nexus::protocol::MessageType::Open) {
        std::cerr << "Expected a valid OPEN frame from the edge" << std::endl;
        return false;
    }
    return true;
}

// The optional arguments choose the local service that will eventually receive
// tunneled traffic. For example: ./client 127.0.0.1 9000
int main(int argc, char* argv[]) {
    // A closed local service should make send() fail instead of terminating this
    // process with SIGPIPE, allowing the normal cleanup path to run.
    std::signal(SIGPIPE, SIG_IGN);

    // These defaults point to a service on this computer. They are separate from
    // kEdgeAddress and kTunnelPort, which identify the remote tunnel server.
    const char* localAddress = "127.0.0.1";
    int localPort = 9000;

    // argv[0] is the program name, so at most two user-supplied values are valid.
    if (argc > 3) {
        std::cerr << "Usage: ./client [local-address] [local-port]" << std::endl;
        return 1;
    }

    // The first optional argument replaces the default local address.
    if (argc >= 2) {
        localAddress = argv[1];
    }

    // The second optional argument arrives as text. std::stoi() converts it to an
    // integer, and the range check ensures it can be used as a TCP port.
    if (argc >= 3) {
        try {
            localPort = std::stoi(argv[2]);
            if (localPort < 1 || localPort > 65535) {
                std::cerr << "Invalid port number: " << argv[2] << std::endl;
                return 1;
            }
        } catch (const std::exception& e) {
            std::cerr << "Invalid port number: " << argv[2] << std::endl;
            return 1;
        }
    }

    // This confirms the selected target before any connection is attempted.
    std::cout << "Local service: " << localAddress << ":" << localPort << std::endl;

    // tunnelSocket is the outbound connection to the NexusProxy edge.
    int tunnelSocket = connectTcp(kEdgeAddress, kTunnelPort);
    if (tunnelSocket == -1) {
        std::cerr << "Failed to connect to server at " << kEdgeAddress << ":" << kTunnelPort << std::endl;
        return 1;
    }

    // localSocket is a separate connection to the application being exposed.
    int localSocket = connectTcp(localAddress, localPort);

    // The tunnel was opened first, so it must be closed if the local connection
    // fails. Otherwise the edge would keep a connection this process no longer uses.
    if (localSocket == -1) {
        std::cerr << "Failed to connect to local service at " << localAddress << ":" << localPort << std::endl;
        close(tunnelSocket);
        return 1;
    }

    // Both sides of the developer client are connected and ready for relay data.
    std::cout << "Successfully connected to the local service at " << localAddress << ":" << localPort << std::endl;



    nexus::protocol::Frame openFrame;
    if (!receiveOpenFrame(tunnelSocket, openFrame)) {
        close(localSocket);
        close(tunnelSocket);
        return 1;
    }

    // The key is the identity used in tunnel frames; the value is the socket
    // connected to the local application for that public conversation.
    std::unordered_map<std::uint32_t, int> localStreams;
    localStreams.emplace(openFrame.streamId, localSocket);

    std::cout << "Public stream " << openFrame.streamId << " is ready"
              << std::endl;

    // Watch both connections so requests and responses can move independently.
    struct pollfd fds[2]{};
    fds[0].fd = tunnelSocket;
    fds[0].events = POLLIN;
    fds[1].fd = localSocket;
    fds[1].events = POLLIN;

    char buffer[4096];
    nexus::protocol::FrameDecoder tunnelDecoder;
    // This stays true for a normal disconnect and becomes false on a relay error.
    bool relaySucceeded = true;

    while (true) {
        int ready;

        do {
            ready = poll(fds, 2, -1);
        } while (ready == -1 && errno == EINTR);

        if (ready == -1) {
            std::cerr << "Failed to poll sockets: " << std::strerror(errno) << std::endl;
            relaySucceeded = false;
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            // These are public request bytes coming from the edge.
            ssize_t tunnelBytesReceived;

            do {
                tunnelBytesReceived = recv(tunnelSocket, buffer, sizeof(buffer), 0);
            } while (tunnelBytesReceived == -1 && errno == EINTR);

            if (tunnelBytesReceived > 0) {
                tunnelDecoder.append(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(buffer),
                    static_cast<std::size_t>(tunnelBytesReceived)));

                while (true) {
                    nexus::protocol::Frame frame;
                    const auto status = tunnelDecoder.nextFrame(frame);
                    if (status == nexus::protocol::DecodeStatus::NeedMoreData) {
                        break;
                    }
                    if (status == nexus::protocol::DecodeStatus::ProtocolError ||
                        frame.type != nexus::protocol::MessageType::Data) {
                        std::cerr << "Received an invalid tunnel frame" << std::endl;
                        relaySucceeded = false;
                        break;
                    }

                    const auto local = localStreams.find(frame.streamId);
                    if (local == localStreams.end()) {
                        std::cerr << "Received DATA for an unknown stream"
                                  << std::endl;
                        relaySucceeded = false;
                        break;
                    }

                    if (!sendAll(local->second,
                                 reinterpret_cast<const char*>(frame.payload.data()),
                                 frame.payload.size())) {
                        std::cerr << "Failed to send DATA to local service"
                                  << std::endl;
                        relaySucceeded = false;
                        break;
                    }
                }

                if (!relaySucceeded) {
                    break;
                }
            } else {
                // Zero means the edge closed cleanly; -1 means recv() failed.
                if (tunnelBytesReceived == 0) {
                    std::cout << "Tunnel connection closed by edge" << std::endl;
                } else {
                    std::cerr << "Failed to receive tunnel data: "
                              << std::strerror(errno) << std::endl;
                    relaySucceeded = false;
                }
                break;
            }
        }

        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
            // These are local response bytes that must go back through the tunnel.
            ssize_t localBytesReceived;

            do {
                localBytesReceived = recv(localSocket, buffer, sizeof(buffer), 0);
            } while (localBytesReceived == -1 && errno == EINTR);

            if (localBytesReceived > 0) {
                const auto* payloadBegin =
                    reinterpret_cast<const std::uint8_t*>(buffer);
                const nexus::protocol::Frame dataFrame{
                    nexus::protocol::MessageType::Data, openFrame.streamId,
                    {payloadBegin, payloadBegin + localBytesReceived}};

                if (!sendFrame(tunnelSocket, dataFrame)) {
                    std::cerr << "Failed to send local data through the tunnel" << std::endl;
                    relaySucceeded = false;
                    break;
                }

                std::cout << "Sent " << localBytesReceived
                          << " local response bytes through the tunnel" << std::endl;
            } else {
                if (localBytesReceived == 0) {
                    std::cout << "Local connection closed by service" << std::endl;
                } else {
                    std::cerr << "Failed to receive local data: "
                              << std::strerror(errno) << std::endl;
                    relaySucceeded = false;
                }
                break;
            }
        }
    }

    // main() owns both descriptors returned by connectTcp() and closes each one.
    close(localSocket);
    localStreams.erase(openFrame.streamId);
    close(tunnelSocket);
    return relaySucceeded ? 0 : 1;
}
