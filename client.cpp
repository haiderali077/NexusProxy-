// NexusProxy developer client
// Connects to the edge and a local service, then waits for the edge to announce
// a public connection. Traffic forwarding is not implemented yet.

#include <arpa/inet.h>  // inet_pton() converts an IP string to binary.
#include <iostream>     // Prints progress and error messages.
#include <netinet/in.h> // sockaddr_in describes an IPv4 endpoint.
#include <sys/socket.h> // socket() and connect().
#include <unistd.h>     // close() releases a file descriptor.
#include <string>       // std::stoi() converts a string to an integer.

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

    // Both connections now exist, but bytes are not forwarded between them yet.
    std::cout << "Successfully connected to the local service at " << localAddress << ":" << localPort << std::endl;

    // recv() waits until the edge sends its one-byte notification or disconnects.
    char signal{};
    ssize_t bytesReceived = recv(tunnelSocket, &signal, sizeof(signal), 0);

    if (bytesReceived <= 0) {
        std::cerr << "Failed to receive signal from edge" << std::endl;
        close(localSocket);
        close(tunnelSocket);
        return 1;
    } else if (signal != 'O') {
        std::cerr << "Unexpected signal received: " << signal << std::endl;
        close(localSocket);
        close(tunnelSocket);
        return 1;
    }

    // The control message crossed the tunnel, but request bytes are not forwarded.
    std::cout << "Public connection is ready" << std::endl;

    // main() owns both descriptors returned by connectTcp() and closes each one.
    close(localSocket);
    close(tunnelSocket);
    return 0;
}
