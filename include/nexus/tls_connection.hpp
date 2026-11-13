#pragma once

#include "nexus/tls_context.hpp"

#include <openssl/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace nexus::security {

enum class TlsStatus {
    Complete,
    WantRead,
    WantWrite,
    Closed,
    Error,
};

struct TlsIoResult {
    TlsStatus status{TlsStatus::Error};
    std::size_t bytes{};
};

class TlsConnection {
public:
    TlsConnection() noexcept = default;
    ~TlsConnection();

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    TlsConnection(TlsConnection&& other) noexcept;
    TlsConnection& operator=(TlsConnection&& other) noexcept;

    static std::optional<TlsConnection> createServer(
        TlsContext& context, int socketFd, std::string& error);
    static std::optional<TlsConnection> createClient(
        TlsContext& context, int socketFd, const std::string& expectedPeerName,
        std::string& error);

    TlsStatus handshake(std::string& error);
    TlsIoResult read(std::span<std::uint8_t> output, std::string& error);
    TlsIoResult write(std::span<const std::uint8_t> input,
                      std::string& error);
    void shutdown() noexcept;

    [[nodiscard]] bool peerCertificateVerified() const noexcept;
    [[nodiscard]] std::string cipherName() const;
    [[nodiscard]] bool valid() const noexcept;

private:
    explicit TlsConnection(SSL* connection) noexcept;
    TlsStatus translateResult(int result, std::string& error) const;
    void reset(SSL* connection = nullptr) noexcept;

    SSL* connection_{};
};

} // namespace nexus::security
