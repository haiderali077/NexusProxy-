#include "nexus/tls_context.hpp"

#include "nexus/tls_error.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <utility>

namespace nexus::security {
namespace {

bool configureCommonContext(SSL_CTX* context, std::string& error) {
    // TLS 1.3 removes old cipher and handshake choices that this project does
    // not need to support. OpenSSL still selects a secure TLS 1.3 cipher suite.
    if (SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1) {
        error = collectOpenSslErrors();
        return false;
    }
    SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
    return true;
}

} // namespace

TlsContext::TlsContext(SSL_CTX* context) noexcept : context_(context) {}

TlsContext::~TlsContext() {
    reset();
}

TlsContext::TlsContext(TlsContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

TlsContext& TlsContext::operator=(TlsContext&& other) noexcept {
    if (this != &other) {
        reset(std::exchange(other.context_, nullptr));
    }
    return *this;
}

std::optional<TlsContext> TlsContext::createServer(
    const std::string& certificateFile, const std::string& privateKeyFile,
    std::string& error) {
    ERR_clear_error();
    TlsContext context{SSL_CTX_new(TLS_server_method())};
    if (!context.valid()) {
        error = collectOpenSslErrors();
        return std::nullopt;
    }
    if (!configureCommonContext(context.get(), error)) {
        return std::nullopt;
    }
    if (SSL_CTX_use_certificate_chain_file(context.get(),
                                           certificateFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(context.get(), privateKeyFile.c_str(),
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context.get()) != 1) {
        error = collectOpenSslErrors();
        return std::nullopt;
    }
    return context;
}

std::optional<TlsContext> TlsContext::createClient(
    const std::string& trustedCaFile, std::string& error) {
    ERR_clear_error();
    TlsContext context{SSL_CTX_new(TLS_client_method())};
    if (!context.valid()) {
        error = collectOpenSslErrors();
        return std::nullopt;
    }
    if (!configureCommonContext(context.get(), error)) {
        return std::nullopt;
    }

    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_load_verify_locations(context.get(), trustedCaFile.c_str(),
                                      nullptr) != 1) {
        error = collectOpenSslErrors();
        return std::nullopt;
    }
    return context;
}

SSL_CTX* TlsContext::get() const noexcept {
    return context_;
}

bool TlsContext::valid() const noexcept {
    return context_ != nullptr;
}

void TlsContext::reset(SSL_CTX* context) noexcept {
    if (context_ != nullptr) {
        SSL_CTX_free(context_);
    }
    context_ = context;
}

} // namespace nexus::security
