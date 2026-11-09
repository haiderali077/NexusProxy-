#pragma once

#include "secure_token.hpp"
#include "tls_connection.hpp"

#include <chrono>
#include <string>

namespace nexus::security {

enum class AuthenticationResult {
    Succeeded,
    Rejected,
    Closed,
    Error,
};

AuthenticationResult authenticateClient(
    int socketFd, TlsConnection& connection, const SecureToken& token,
    std::chrono::milliseconds timeout, std::string& error);
AuthenticationResult authenticateServer(
    int socketFd, TlsConnection& connection, const SecureToken& expectedToken,
    std::chrono::milliseconds timeout, std::string& error);

} // namespace nexus::security
