#include "nexus/secure_token.hpp"

#include <openssl/crypto.h>

#include <utility>

namespace nexus::security {

SecureToken::SecureToken(std::string& source)
    : bytes_(source.begin(), source.end()) {
    // Once copied into the guarded owner, remove the loader's extra plaintext
    // copy before its string capacity is eventually reused.
    if (!source.empty()) {
        OPENSSL_cleanse(source.data(), source.size());
        source.clear();
    }
}

SecureToken::~SecureToken() {
    clear();
}

SecureToken::SecureToken(SecureToken&& other) noexcept
    : bytes_(std::move(other.bytes_)) {
    other.clear();
}

SecureToken& SecureToken::operator=(SecureToken&& other) noexcept {
    if (this != &other) {
        clear();
        bytes_ = std::move(other.bytes_);
        other.clear();
    }
    return *this;
}

std::span<const std::uint8_t> SecureToken::bytes() const noexcept {
    return bytes_;
}

bool SecureToken::matches(
    std::span<const std::uint8_t> candidate) const noexcept {
    if (candidate.size() != bytes_.size() || bytes_.empty()) {
        return false;
    }
    return CRYPTO_memcmp(candidate.data(), bytes_.data(), bytes_.size()) == 0;
}

std::size_t SecureToken::size() const noexcept {
    return bytes_.size();
}

bool SecureToken::empty() const noexcept {
    return bytes_.empty();
}

void SecureToken::clear() noexcept {
    if (!bytes_.empty()) {
        OPENSSL_cleanse(bytes_.data(), bytes_.size());
        bytes_.clear();
    }
}

} // namespace nexus::security
