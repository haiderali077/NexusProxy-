// NexusProxy edge server
//
// This program keeps one developer tunnel and one public connection open. When a
// public connection arrives, it notifies the developer client through the tunnel.
// Forwarding the public connection's bytes comes next.

#include <cerrno>  // errno and EINTR identify an interrupted send().
#include <csignal>
#include <cstddef> // std::size_t stores buffer lengths and byte counts.
#include <iostream>
#include <sys/socket.h> // socket(), setsockopt(), bind(), listen(), and accept().
#include <netinet/in.h> // sockaddr_in, INADDR_ANY, and htons().
#include <unistd.h>     // close() releases a socket descriptor.
#include <poll.h>       // poll() watches both listening sockets at the same time.

// This builds a passive TCP listening socket for one local port. On success the
// caller owns the returned descriptor and must eventually close it.
int createListener(int port) {
    // AF_INET means IPv4 and SOCK_STREAM means TCP. The final 0 asks the OS for
    // the default protocol. socket() creates the endpoint, but it is not bound,
    // listening, or connected yet.
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket == -1) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }

    // SO_REUSEADDR makes restarting the server less likely to fail because the
    // local address is still tied to an earlier connection. It does not make it
    // safe for two active servers to own the same address and port.
    int option = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
                   &option, sizeof(option)) == -1) {
        std::cerr << "Failed to set socket options" << std::endl;
        close(serverSocket);
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
    if (bind(serverSocket, (struct sockaddr*)&serverAddress,
             sizeof(serverAddress)) == -1) {
        std::cerr << "Failed to bind port " << port << std::endl;
        close(serverSocket);
        return -1;
    }

    // listen() changes the bound socket into a passive listener. The value 5 is
    // the requested queue size for connections waiting to be accepted.
    if (listen(serverSocket, 5) == -1) {
        std::cerr << "Failed to listen on port " << port << std::endl;
        close(serverSocket);
        return -1;
    }

    std::cout << "Listening on port " << port << std::endl;
    // Returning transfers ownership to main(); this function must not close a
    // successfully created listener.
    return serverSocket;
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
            return false;
        }

        bytesSent += static_cast<std::size_t>(result);
    }

    // Reaching this point means every requested byte was accepted by the socket.
    return true;
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

    // The first two entries watch the listening sockets. The third watches the
    // connected tunnel after accept() creates it.
    struct pollfd fds[3];
    fds[0].fd = socket7000; // Watch for a developer client trying to connect.
    fds[0].events = POLLIN;
    fds[1].fd = socket8000; // Watch for a public user trying to connect.
    fds[1].events = POLLIN;
    // poll() ignores negative descriptors, so this entry stays inactive until a
    // developer client connects.
    fds[2].fd = -1;
    fds[2].events = POLLIN; // Watch for data or disconnect on the tunnel.

    while (true) {
        // The count is 3 because poll() checks every entry in fds. The final -1
        // means wait indefinitely until one of those descriptors has an event.
        int result = poll(fds, 3, -1);

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

                // 'O' tells the developer client that a public socket is ready.
                // sendAll() also stays correct if this message later grows and
                // one send() writes only part of it.
                const char signal = 'O';

                if (!sendAll(tunnelSocket, &signal, sizeof(signal))) {
                    std::cerr << "Failed to notify developer client" << std::endl;
                    close(publicSocket);
                    publicSocket = -1;
                } else {
                    std::cout << "Retained public connection and notified developer client"
                              << std::endl;
                }
            }
        }

        // An event on the connected socket can mean data arrived, the client
        // closed cleanly, or the socket failed. recv() tells these cases apart.

        if (tunnelSocket != -1 &&
            (fds[2].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            // The client does not send tunnel data yet, so this read only checks
            // whether the connection is still alive.
            char buffer[1];
            ssize_t bytesReceived = recv(tunnelSocket, buffer, sizeof(buffer), 0);

            if (bytesReceived == 0 || bytesReceived == -1) {
                close(tunnelSocket);
                tunnelSocket = -1;
                fds[2].fd = -1;

                // A public socket cannot be served after its tunnel disappears.
                if (publicSocket != -1) {
                    close(publicSocket);
                    publicSocket = -1;
                }

                std::cout << "Tunnel disconnected; waiting for a new client"
                          << std::endl;
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
