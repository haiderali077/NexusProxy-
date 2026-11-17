#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nexus::security {

class SecureToken {
public:
    SecureToken() = default;
    explicit SecureToken(std::string& source);
    ~SecureToken();

    SecureToken(const SecureToken&) = delete;
    SecureToken& operator=(const SecureToken&) = delete;

    SecureToken(SecureToken&& other) noexcept;
    SecureToken& operator=(SecureToken&& other) noexcept;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] bool matches(
        std::span<const std::uint8_t> candidate) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    void clear() noexcept;

    std::vector<std::uint8_t> bytes_;
};

} // namespace nexus::security
