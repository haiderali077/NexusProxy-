#include <iostream>
#include <sys/socket.h> // for creating sockets
#include <netinet/in.h> // provides sockaddr_in structure eg defintion of internet address
#include <unistd.h> // // provies close() function to close the socket eg opreating ssytem functions
#include <poll.h> // for using poll library to handle multiple connections

int createListener(int port) {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket == -1) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }

    int option = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
                   &option, sizeof(option)) == -1) {
        std::cerr << "Failed to set socket options" << std::endl;
        close(serverSocket);
        return -1;
    }

    struct sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(port);

    if (bind(serverSocket, (struct sockaddr*)&serverAddress,
             sizeof(serverAddress)) == -1) {
        std::cerr << "Failed to bind port " << port << std::endl;
        close(serverSocket);
        return -1;
    }

    if (listen(serverSocket, 5) == -1) {
        std::cerr << "Failed to listen on port " << port << std::endl;
        close(serverSocket);
        return -1;
    }

    std::cout << "Listening on port " << port << std::endl;
    return serverSocket;
}

int main() {
    int socket7000 = createListener(7000);
    if (socket7000 == -1) {
        std::cerr << "Failed to create listener on port 7000" << std::endl;
        return 1;
    }

    int socket8000 = createListener(8000);
    if (socket8000 == -1) {
        std::cerr << "Failed to create listener on port 8000" << std::endl;
        close(socket7000);
        return 1;
    }

    struct pollfd fds[2];
    fds[0].fd = socket7000;
    fds[0].events = POLLIN;
    fds[1].fd = socket8000;
    fds[1].events = POLLIN;

    while (true) {
        int result = poll(fds, 2, -1);

        if (result == -1) {
            std::cerr << "Poll failed" << std::endl;
            break;
        }

        if (fds[0].revents & POLLIN) {
            std::cout << "Connection waiting on port 7000" << std::endl;
            int clientSocket1 = accept(socket7000, nullptr, nullptr);

            if (clientSocket1 == -1) {
                std::cerr << "Failed to accept connection on port 7000" << std::endl;
            } else {
                std::cout << "Accepted connection on port 7000" << std::endl;
                close(clientSocket1);
                std::cout << "Closed client connection on port 7000" << std::endl;
            }
        }

        if (fds[1].revents & POLLIN) {
            std::cout << "Connection waiting on port 8000" << std::endl;
            int clientSocket2 = accept(socket8000, nullptr, nullptr);

            if (clientSocket2 == -1) {
                std::cerr << "Failed to accept connection on port 8000" << std::endl;
            } else {
                std::cout << "Accepted connection on port 8000" << std::endl;
                close(clientSocket2);
                std::cout << "Closed client connection on port 8000" << std::endl;
            }
        }
    }

    close(socket7000);
    std::cout << "Closed listener on port 7000" << std::endl;
    close(socket8000);
    std::cout << "Closed listener on port 8000" << std::endl;
    return 1;
}
