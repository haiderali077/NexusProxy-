// NexusProxy edge server
//
// This program keeps one developer tunnel and one public connection open. It
// notifies the developer client, then relays bytes in both directions between
// the public connection and the developer tunnel.

#include "protocol.hpp"

#include <cerrno>  // errno and EINTR identify an interrupted send().
#include <csignal>
#include <cstddef> // std::size_t stores buffer lengths and byte counts.
#include <cstdint> // std::uint32_t matches the stream ID stored in a frame.
#include <cstring> // std::strerror() turns errno into a readable message.
#include <iostream>
#include <sys/socket.h> // socket(), setsockopt(), bind(), listen(), and accept().
#include <netinet/in.h> // sockaddr_in, INADDR_ANY, and htons().
#include <unistd.h>     // close() releases a socket descriptor.
#include <poll.h>       // poll() watches several sockets at the same time.
#include <unordered_map> // Connects each stream ID to its public socket.
#include <vector>       // Holds an encoded protocol frame before sending it.

// This builds a passive TCP listening socket for one local port. On success the
// caller owns the returned descriptor and must eventually close it.
int createListener(int port) {
    // AF_INET means IPv4 and SOCK_STREAM means TCP. The final 0 asks the OS for
    // the default protocol. socket() creates the endpoint, but it is not bound,
    // listening, or connected yet.
    int tcpListenerSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (tcpListenerSocket == -1) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }

    // SO_REUSEADDR makes restarting the server less likely to fail because the
    // local address is still tied to an earlier connection. It does not make it
    // safe for two active servers to own the same address and port.
    int option = 1;
    if (setsockopt(tcpListenerSocket, SOL_SOCKET, SO_REUSEADDR,
                   &option, sizeof(option)) == -1) {
        std::cerr << "Failed to set socket options" << std::endl;
        close(tcpListenerSocket);
        return -1;
    }

    // `{}` starts every field at zero. INADDR_ANY means this server accepts
    // connections through its local IPv4 interfaces, not one specific local IP.
    // htons() stores the port in the byte order used by network protocols.
    struct sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(port);

    // bind() assigns this local address and port to the socket. The cast changes
    // my IPv4-specific address pointer to the general type required by the API;
    // the size tells bind() how much address data it received.
    if (bind(tcpListenerSocket, (struct sockaddr*)&serverAddress,
             sizeof(serverAddress)) == -1) {
        std::cerr << "Failed to bind port " << port << std::endl;
        close(tcpListenerSocket);
        return -1;
    }

    // listen() changes the bound socket into a passive listener. The value 5 is
    // the requested queue size for connections waiting to be accepted.
    if (listen(tcpListenerSocket, 5) == -1) {
        std::cerr << "Failed to listen on port " << port << std::endl;
        close(tcpListenerSocket);
        return -1;
    }

    std::cout << "Listening on port " << port << std::endl;
    // Returning transfers ownership to main(); this function must not close a
    // successfully created listener.
    return tcpListenerSocket;
}

// send() may write fewer bytes than requested, even when it succeeds. This
// helper will hide that detail from the notification and relay code.
bool sendAll(int socketFd, const char* data, std::size_t length) {
    std::size_t bytesSent = 0;

    while (bytesSent < length) {
        // Skip the bytes already written and give send() only the remaining
        // part of the buffer. One successful call may still write only part.
        ssize_t result = send(socketFd, data + bytesSent, length - bytesSent, 0);

        if (result == -1) {
            if (errno == EINTR) {
                // A signal interrupted send() before completion, so try again.
                continue;
            }

            return false;
        }

        // No progress means the complete buffer cannot be delivered.
        if (result == 0) {
            std::cerr << "send() made no progress\n";
            return false;
        }

        bytesSent += static_cast<std::size_t>(result);
    }

    // Reaching this point means every requested byte was accepted by the socket.
    return true;
}

// Control and data messages use the same frame format, so callers only build a
// Frame and this helper handles conversion to bytes plus complete delivery.
bool sendFrame(int socketFd, const nexus::protocol::Frame& frame) {
    std::vector<std::uint8_t> bytes;
    if (!nexus::protocol::encodeFrame(frame, bytes)) {
        return false;
    }

    return sendAll(socketFd, reinterpret_cast<const char*>(bytes.data()),
                   bytes.size());
}

int main() {
    // Make a failed send() return -1 instead of terminating the whole process.
    std::signal(SIGPIPE, SIG_IGN);

    // Port 7000 is currently intended for the developer client's tunnel.
    int socket7000 = createListener(7000);
    if (socket7000 == -1) {
        std::cerr << "Failed to create listener on port 7000" << std::endl;
        return 1;
    }

    // Port 8000 is currently the public-facing listener. This is still a second
    // listening socket; it is not the local application behind client.cpp.
    int socket8000 = createListener(8000);
    if (socket8000 == -1) {
        std::cerr << "Failed to create listener on port 8000" << std::endl;
        close(socket7000);
        return 1;
    }

    // -1 means no developer client is connected yet. This is separate from
    // socket7000: socket7000 listens for clients, while tunnelSocket will hold
    // the connection returned by accept().
    int tunnelSocket = -1;

    // The public descriptor must also survive after the accept block finishes.
    int publicSocket = -1;

    // Stream 0 belongs to tunnel-wide control messages, so public connections
    // receive IDs beginning at 1. The counter advances after every assignment.
    std::uint32_t publicStreamId = 0;
    std::uint32_t nextStreamId = 1;

    // The stream ID is the protocol identity; the descriptor is the operating
    // system resource used to read and write that public TCP connection.
    std::unordered_map<std::uint32_t, int> publicStreams;
    nexus::protocol::FrameDecoder tunnelDecoder;

    // The first two entries watch the listeners. The other entries become active
    // after accept() creates the tunnel and public connections.
    struct pollfd fds[4]{};
    fds[0].fd = socket7000; // Watch for a developer client trying to connect.
    fds[0].events = POLLIN;
    fds[1].fd = socket8000; // Watch for a public user trying to connect.
    fds[1].events = POLLIN;
    // poll() ignores negative descriptors, so this entry stays inactive until a
    // developer client connects.
    fds[2].fd = -1;
    fds[2].events = POLLIN; // Watch for data or disconnect on the tunnel.
    fds[3].fd = -1;
    fds[3].events = POLLIN; // Watch for request bytes from the public user.

    while (true) {
        // The count is 4 because poll() checks every entry in fds. The final -1
        // means wait indefinitely until one of those descriptors has an event.
        int result = poll(fds, 4, -1);

        if (result == -1) {
            std::cerr << "Poll failed" << std::endl;
            break;
        }

        // POLLIN on a listening socket means a connection is ready to accept;
        // it does not mean normal application bytes are ready on this listener.
        if (fds[0].revents & POLLIN) {
            std::cout << "Connection waiting on port 7000" << std::endl;
            // accept() receives the listening descriptor and creates a new
            // descriptor for this one connection. socket7000 remains a listener.
            int acceptedTunnel = accept(socket7000, nullptr, nullptr);

            if (acceptedTunnel == -1) {
                std::cerr << "Failed to accept connection on port 7000" << std::endl;
            } else if (tunnelSocket != -1) {
                // This version supports one developer tunnel. Rejecting a second
                // client avoids silently replacing the connection we already own.
                std::cout << "A tunnel is already connected; rejecting the new client"
                          << std::endl;
                close(acceptedTunnel);
            } else {
                // Store the accepted descriptor instead of closing it here. The
                // server now owns it and keeps it for the future relay step.
                tunnelSocket = acceptedTunnel;
                // fds[2] now watches this connected socket. fds[0] must continue
                // watching socket7000 so future connection attempts are noticed.
                fds[2].fd = tunnelSocket;
                std::cout << "Accepted and retained tunnel connection on port 7000"
                          << std::endl;
            }
        }

        // This repeats the same accept flow for the public listener. The two
        // accepted descriptors belong to separate TCP connections.
        if (fds[1].revents & POLLIN) {
            std::cout << "Connection waiting on port 8000" << std::endl;
            int acceptedPublic = accept(socket8000, nullptr, nullptr);

            if (acceptedPublic == -1) {
                std::cerr << "Failed to accept connection on port 8000" << std::endl;
            } else if (tunnelSocket == -1) {
                // There is nowhere to route public traffic without a tunnel.
                std::cerr << "No tunnel connected; rejecting public connection on port 8000"
                          << std::endl;
                close(acceptedPublic);
            } else if (publicSocket != -1) {
                // This milestone supports only one public connection at a time.
                std::cerr << "A public connection is already retained; rejecting the new client on port 8000"
                          << std::endl;
                close(acceptedPublic);
            } else {
                publicSocket = acceptedPublic;
                publicStreamId = nextStreamId++;
                if (nextStreamId == 0) {
                    nextStreamId = 1;
                }

                std::cout << "Assigned stream " << publicStreamId
                          << " to the public connection" << std::endl;
                publicStreams.emplace(publicStreamId, publicSocket);

                // OPEN carries the new stream ID, allowing the client to create
                // the matching local connection before DATA frames arrive.
                const nexus::protocol::Frame openFrame{
                    nexus::protocol::MessageType::Open, publicStreamId, {}};

                if (!sendFrame(tunnelSocket, openFrame)) {
                    std::cerr << "Failed to notify developer client" << std::endl;
                    close(publicSocket);
                    publicStreams.erase(publicStreamId);
                    publicSocket = -1;
                    publicStreamId = 0;
                    close(tunnelSocket);
                    tunnelSocket = -1;
                    fds[2].fd = -1;
                } else {
                    // fds[3] lets later poll() calls report every incoming chunk
                    // instead of blocking inside this accept branch.
                    fds[3].fd = publicSocket;
                    std::cout << "Retained public connection and notified developer client"
                              << std::endl;
                }
            }
        }

        if (publicSocket != -1 &&
            (fds[3].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            char buffer[4096];
            ssize_t bytesReceived;

            // Retry the same read when a signal interrupts recv().
            do {
                bytesReceived = recv(publicSocket, buffer, sizeof(buffer), 0);
            } while (bytesReceived == -1 && errno == EINTR);

            if (bytesReceived > 0) {
                const auto* payloadBegin =
                    reinterpret_cast<const std::uint8_t*>(buffer);
                const nexus::protocol::Frame dataFrame{
                    nexus::protocol::MessageType::Data, publicStreamId,
                    {payloadBegin, payloadBegin + bytesReceived}};

                if (!sendFrame(tunnelSocket, dataFrame)) {
                    std::cerr << "Failed to send public data through the tunnel"
                              << std::endl;
                    close(publicSocket);
                    publicStreams.erase(publicStreamId);
                    publicSocket = -1;
                    publicStreamId = 0;
                    fds[3].fd = -1;
                    close(tunnelSocket);
                    tunnelSocket = -1;
                    fds[2].fd = -1;
                } else {
                    std::cout << "Sent " << bytesReceived
                              << " public bytes through the tunnel" << std::endl;
                }
            } else {
                if (bytesReceived == 0) {
                    std::cout << "Public connection closed by client" << std::endl;
                } else {
                    std::cerr << "Failed to receive public data: "
                              << std::strerror(errno) << std::endl;
                }

                close(publicSocket);
                publicStreams.erase(publicStreamId);
                publicSocket = -1;
                publicStreamId = 0;
                fds[3].fd = -1;

                // This first relay version uses one tunnel per public connection.
                // Closing it tells the developer client that this stream ended.
                close(tunnelSocket);
                tunnelSocket = -1;
                fds[2].fd = -1;
            }
        }

        // Tunnel data here is the local service's response for the public user.
        if (tunnelSocket != -1 &&
            (fds[2].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            char buffer[4096];
            ssize_t bytesReceived;

            do {
                bytesReceived = recv(tunnelSocket, buffer, sizeof(buffer), 0);
            } while (bytesReceived == -1 && errno == EINTR);

            if (bytesReceived > 0) {
                tunnelDecoder.append(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(buffer),
                    static_cast<std::size_t>(bytesReceived)));

                while (true) {
                    nexus::protocol::Frame frame;
                    const auto status = tunnelDecoder.nextFrame(frame);
                    if (status == nexus::protocol::DecodeStatus::NeedMoreData) {
                        break;
                    }
                    if (status == nexus::protocol::DecodeStatus::ProtocolError ||
                        frame.type != nexus::protocol::MessageType::Data) {
                        std::cerr << "Received an invalid tunnel frame" << std::endl;
                        close(tunnelSocket);
                        tunnelSocket = -1;
                        fds[2].fd = -1;
                        break;
                    }

                    const auto publicStream = publicStreams.find(frame.streamId);
                    if (publicStream == publicStreams.end()) {
                        std::cerr << "Received DATA for an unknown stream"
                                  << std::endl;
                        continue;
                    }

                    if (!sendAll(publicStream->second,
                                 reinterpret_cast<const char*>(frame.payload.data()),
                                 frame.payload.size())) {
                        std::cerr << "Failed to send DATA to public client"
                                  << std::endl;
                        close(publicStream->second);
                        publicStreams.erase(publicStream);
                        publicSocket = -1;
                        publicStreamId = 0;
                        fds[3].fd = -1;
                        break;
                    }
                }
            } else {
                if (bytesReceived == 0) {
                    std::cout << "Tunnel disconnected; waiting for a new client"
                              << std::endl;
                } else {
                    std::cerr << "Failed to receive tunnel data: "
                              << std::strerror(errno) << std::endl;
                }

                close(tunnelSocket);
                tunnelSocket = -1;
                fds[2].fd = -1;

                // The public connection cannot continue without its tunnel.
                if (publicSocket != -1) {
                    close(publicSocket);
                    publicStreams.erase(publicStreamId);
                    publicSocket = -1;
                    publicStreamId = 0;
                    fds[3].fd = -1;
                }
            }
        }
    }

    // These lines run only after the loop ends because poll() reported an error.
    // Release the connected tunnel first, if one was accepted, followed by the
    // two listening descriptors owned by main().
    if (tunnelSocket != -1) {
        close(tunnelSocket);
        std::cout << "Closed tunnel connection" << std::endl;
    }
    if (publicSocket != -1) {
        close(publicSocket);
        std::cout << "Closed public connection" << std::endl;
    }
    close(socket7000);
    std::cout << "Closed listener on port 7000" << std::endl;
    close(socket8000);
    std::cout << "Closed listener on port 8000" << std::endl;
    // The current code returns 1 here, so even this cleanup path reports failure.
    return 1;
}
