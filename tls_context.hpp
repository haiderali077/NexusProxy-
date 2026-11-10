#pragma once

#include <openssl/types.h>

#include <optional>
#include <string>

namespace nexus::security {

class TlsContext {
public:
    TlsContext() noexcept = default;
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    TlsContext(TlsContext&& other) noexcept;
    TlsContext& operator=(TlsContext&& other) noexcept;

    static std::optional<TlsContext> createServer(
        const std::string& certificateFile, const std::string& privateKeyFile,
        std::string& error);
    static std::optional<TlsContext> createClient(
        const std::string& trustedCaFile, std::string& error);

    [[nodiscard]] SSL_CTX* get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    explicit TlsContext(SSL_CTX* context) noexcept;
    void reset(SSL_CTX* context = nullptr) noexcept;

    SSL_CTX* context_{};
};

} // namespace nexus::security
