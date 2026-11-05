#pragma once

#include <chrono>

namespace nexus::net {

// These helpers return an owned descriptor on success and -1 on failure.
int createTcpListener(int port);
int connectTcp(const char* address, int port);
int connectTcp(const char* address, int port,
               std::chrono::milliseconds timeout);
bool setNonBlocking(int socketFd);

} // namespace nexus::net
