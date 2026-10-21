// NexusProxy edge server
//
// Right now this program listens on ports 7000 and 8000. It keeps one accepted
// client connection open as the tunnel, but it still closes public connections
// immediately because traffic forwarding is not implemented yet.

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

int main() {
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

    // Each pollfd says which descriptor to watch and which event matters.
    // revents is filled in by poll() and tells me what actually happened.
    struct pollfd fds[2];
    fds[0].fd = socket7000; // Watch for a developer client trying to connect.
    fds[0].events = POLLIN;
    fds[1].fd = socket8000; // Watch for a public user trying to connect.
    fds[1].events = POLLIN;

    while (true) {
        // -1 means wait without a timeout. poll() lets one thread wait on both
        // listeners instead of getting stuck inside accept() on only one port.
        int result = poll(fds, 2, -1);

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
                std::cout << "Accepted and retained tunnel connection on port 7000"
                          << std::endl;
            }
        }

        // This repeats the same accept flow for the public listener. The two
        // accepted descriptors belong to separate TCP connections.
        if (fds[1].revents & POLLIN) {
            std::cout << "Connection waiting on port 8000" << std::endl;
            int clientSocket2 = accept(socket8000, nullptr, nullptr);

            if (clientSocket2 == -1) {
                std::cerr << "Failed to accept connection on port 8000" << std::endl;
            } else {
                std::cout << "Accepted connection on port 8000" << std::endl;
                // Public traffic is not forwarded yet, so this connection is
                // also closed as soon as it is accepted.
                close(clientSocket2);
                std::cout << "Closed client connection on port 8000" << std::endl;
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
    close(socket7000);
    std::cout << "Closed listener on port 7000" << std::endl;
    close(socket8000);
    std::cout << "Closed listener on port 8000" << std::endl;
    // The current code returns 1 here, so even this cleanup path reports failure.
    return 1;
}
