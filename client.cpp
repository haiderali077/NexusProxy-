// NexusProxy developer client
// Connects to the edge server's tunnel port. Traffic forwarding is not
// implemented yet. The edge keeps this connection open as the tunnel.

#include <arpa/inet.h>  // inet_pton() converts an IP string to binary.
#include <iostream>     // Prints progress and error messages.
#include <netinet/in.h> // sockaddr_in describes an IPv4 endpoint.
#include <sys/socket.h> // socket() and connect().
#include <unistd.h>     // close() releases a file descriptor.
#include <string>       // std::stoi() converts a string to an integer.

namespace {

[[maybe_unused]] constexpr const char* kEdgeAddress = "127.0.0.1";
[[maybe_unused]] constexpr int kTunnelPort = 7000;

// Returns a connected socket descriptor on success and -1 on failure. Returning
// the descriptor transfers responsibility for closing it to the caller.
[[maybe_unused]] int connectToServer(const char* address, int port) {
    // This creates an IPv4 TCP socket. It is not connected until connect()
    // succeeds below.

    const int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == -1) {
        std::cerr << "Failed to create client socket\n";
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
        std::cerr << "Failed to connect to server: " << address << ":" << port << std::endl;
        close(clientSocket);
        return -1;
    }
    // Do not close a successful socket here because main() still needs it.
    return clientSocket;
}

} // namespace

// The optional arguments choose the local service that will eventually receive
// tunneled traffic. For example: ./client 127.0.0.1 9000
int main(int argc, char*argv[]) {
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

    // This confirms the selected target. Connecting to it is the next step.
    std::cout << "Local service: " << localAddress << ":" << localPort << std::endl;

    // client.cpp and server.cpp are separate programs. This function call only
    // prepares the TCP connection; it does not call code inside server.cpp.
    int tunnelSocket = connectToServer(kEdgeAddress, kTunnelPort);
    if (tunnelSocket == -1) {
        std::cerr << "Failed to connect to server at " << kEdgeAddress << ":" << kTunnelPort << std::endl;
        return 1;
    }

    // Pause so there is time to inspect both programs while the tunnel is open.
    std::cout << "Successfully connected to the server!" << std::endl;
    std::cin.get();

    // main() owns the descriptor returned above, so it releases it when finished.
    close(tunnelSocket);
    return 0;
}
