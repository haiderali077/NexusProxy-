#include "tls_connection.hpp"

#include "tls_error.hpp"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace nexus::security {
namespace {

bool configureExpectedPeer(SSL* connection, const std::string& peerName,
                           std::string& error) {
    std::array<unsigned char, sizeof(in6_addr)> addressBytes{};
    const bool isIpAddress =
        inet_pton(AF_INET, peerName.c_str(), addressBytes.data()) == 1 ||
        inet_pton(AF_INET6, peerName.c_str(), addressBytes.data()) == 1;

    X509_VERIFY_PARAM* parameters = SSL_get0_param(connection);
    if (parameters == nullptr) {
        error = "OpenSSL did not provide certificate verification parameters";
        return false;
    }
    if (isIpAddress) {
        if (X509_VERIFY_PARAM_set1_ip_asc(parameters, peerName.c_str()) != 1) {
            error = collectOpenSslErrors();
            return false;
        }
        return true;
    }

    // SNI selects the certificate on multi-domain servers. SSL_set1_host()
    // separately makes that DNS name part of certificate verification.
    if (SSL_set_tlsext_host_name(connection, peerName.c_str()) != 1 ||
        SSL_set1_host(connection, peerName.c_str()) != 1) {
        error = collectOpenSslErrors();
        return false;
    }
    return true;
}

} // namespace

TlsConnection::TlsConnection(SSL* connection) noexcept
    : connection_(connection) {}

TlsConnection::~TlsConnection() {
    reset();
}

TlsConnection::TlsConnection(TlsConnection&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)) {}

TlsConnection& TlsConnection::operator=(TlsConnection&& other) noexcept {
    if (this != &other) {
        reset(std::exchange(other.connection_, nullptr));
    }
    return *this;
}

std::optional<TlsConnection> TlsConnection::createServer(
    TlsContext& context, int socketFd, std::string& error) {
    ERR_clear_error();
    TlsConnection connection{SSL_new(context.get())};
    if (!connection.valid()) {
        error = collectOpenSslErrors();
        return std::nullopt;
    }
    if (SSL_set_fd(connection.connection_, socketFd) != 1) {
        error = collectOpenSslErrors();
        return std::nullopt;
    }
    SSL_set_accept_state(connection.connection_);
    return connection;
}

std::optional<TlsConnection> TlsConnection::createClient(
    TlsContext& context, int socketFd, const std::string& expectedPeerName,
    std::string& error) {
    ERR_clear_error();
    TlsConnection connection{SSL_new(context.get())};
    if (!connection.valid()) {
        error = collectOpenSslErrors();
        return std::nullopt;
    }
    if (SSL_set_fd(connection.connection_, socketFd) != 1 ||
        !configureExpectedPeer(connection.connection_, expectedPeerName,
                               error)) {
        if (error.empty()) {
            error = collectOpenSslErrors();
        }
        return std::nullopt;
    }
    SSL_set_connect_state(connection.connection_);
    return connection;
}

TlsStatus TlsConnection::handshake(std::string& error) {
    ERR_clear_error();
    return translateResult(SSL_do_handshake(connection_), error);
}

TlsIoResult TlsConnection::read(std::span<std::uint8_t> output,
                                std::string& error) {
    if (output.empty()) {
        return {TlsStatus::Complete, 0};
    }
    const int requested = static_cast<int>(std::min<std::size_t>(
        output.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
    std::size_t received = 0;
    ERR_clear_error();
    const int result = SSL_read_ex(connection_, output.data(), requested,
                                   &received);
    if (result == 1) {
        return {TlsStatus::Complete, received};
    }
    return {translateResult(result, error), 0};
}

TlsIoResult TlsConnection::write(std::span<const std::uint8_t> input,
                                 std::string& error) {
    if (input.empty()) {
        return {TlsStatus::Complete, 0};
    }
    const std::size_t requested = std::min<std::size_t>(
        input.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()));
    std::size_t sent = 0;
    ERR_clear_error();
    const int result = SSL_write_ex(connection_, input.data(), requested, &sent);
    if (result == 1) {
        return {TlsStatus::Complete, sent};
    }
    return {translateResult(result, error), 0};
}

void TlsConnection::shutdown() noexcept {
    if (connection_ != nullptr) {
        ERR_clear_error();
        static_cast<void>(SSL_shutdown(connection_));
    }
}

bool TlsConnection::peerCertificateVerified() const noexcept {
    return connection_ != nullptr &&
           SSL_get_verify_result(connection_) == X509_V_OK;
}

std::string TlsConnection::cipherName() const {
    if (connection_ == nullptr) {
        return {};
    }
    const char* name = SSL_get_cipher_name(connection_);
    return name == nullptr ? std::string{} : std::string{name};
}

bool TlsConnection::valid() const noexcept {
    return connection_ != nullptr;
}

TlsStatus TlsConnection::translateResult(int result, std::string& error) const {
    const int sslError = SSL_get_error(connection_, result);
    switch (sslError) {
        case SSL_ERROR_WANT_READ:
            return TlsStatus::WantRead;
        case SSL_ERROR_WANT_WRITE:
            return TlsStatus::WantWrite;
        case SSL_ERROR_ZERO_RETURN:
            return TlsStatus::Closed;
        case SSL_ERROR_SYSCALL:
            if (result == 0) {
                return TlsStatus::Closed;
            }
            [[fallthrough]];
        default:
            error = collectOpenSslErrors();
            return TlsStatus::Error;
    }
}

void TlsConnection::reset(SSL* connection) noexcept {
    if (connection_ != nullptr) {
        SSL_free(connection_);
    }
    connection_ = connection;
}

} // namespace nexus::security
