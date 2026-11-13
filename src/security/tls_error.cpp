#include "nexus/tls_error.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>

#include <array>

namespace nexus::security {

std::string_view openSslVersion() {
    return OpenSSL_version(OPENSSL_VERSION);
}

std::string collectOpenSslErrors() {
    std::string message;
    unsigned long errorCode = 0;
    while ((errorCode = ERR_get_error()) != 0) {
        std::array<char, 256> buffer{};
        ERR_error_string_n(errorCode, buffer.data(), buffer.size());
        if (!message.empty()) {
            message += "; ";
        }
        message += buffer.data();
    }

    // Some OpenSSL failures do not add a queue entry. Returning a stable
    // fallback is more useful than printing an empty error message.
    return message.empty() ? "OpenSSL did not provide an error" : message;
}

} // namespace nexus::security
