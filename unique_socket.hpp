#pragma once

namespace nexus::net {

// Owns one socket descriptor and closes it exactly once when ownership ends.
class UniqueSocket {
public:
    UniqueSocket() noexcept = default;
    explicit UniqueSocket(int socketFd) noexcept;
    ~UniqueSocket();

    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;

    UniqueSocket(UniqueSocket&& other) noexcept;
    UniqueSocket& operator=(UniqueSocket&& other) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept;

    [[nodiscard]] int release() noexcept;
    void reset(int socketFd = -1) noexcept;

private:
    int socketFd_{-1};
};

} // namespace nexus::net
