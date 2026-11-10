#pragma once

#include <chrono>
#include <cstddef>
#include <deque>

namespace nexus::security {

class AuthenticationRateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    AuthenticationRateLimiter(std::size_t maximumFailures,
                              std::chrono::milliseconds failureWindow,
                              std::chrono::milliseconds cooldown);

    bool allowAttempt(Clock::time_point now);
    void recordFailure(Clock::time_point now);
    void recordSuccess();

private:
    void removeExpired(Clock::time_point now);

    std::size_t maximumFailures_;
    std::chrono::milliseconds failureWindow_;
    std::chrono::milliseconds cooldown_;
    std::deque<Clock::time_point> failures_;
    Clock::time_point blockedUntil_{};
};

} // namespace nexus::security
