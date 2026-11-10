#pragma once

#include "relay.hpp"
#include "tls_connection.hpp"

#include <chrono>
#include <string>

namespace nexus::net {

bool performTlsHandshake(int socketFd, security::TlsConnection& connection,
                         std::chrono::milliseconds timeout,
                         std::string& error);
security::TlsStatus flushTlsOutput(security::TlsConnection& connection,
                                   OutputQueue& output, std::string& error);
bool drainTlsOutput(int socketFd, security::TlsConnection& connection,
                    OutputQueue& output, std::chrono::milliseconds timeout,
                    std::string& error);

} // namespace nexus::net
