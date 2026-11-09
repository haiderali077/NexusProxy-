#include "auth_rate_limiter.hpp"

#include <algorithm>

namespace nexus::security {

AuthenticationRateLimiter::AuthenticationRateLimiter(
    std::size_t maximumFailures, std::chrono::milliseconds failureWindow,
    std::chrono::milliseconds cooldown)
    : maximumFailures_(std::max<std::size_t>(maximumFailures, 1)),
      failureWindow_(std::max(failureWindow, std::chrono::milliseconds{1})),
      cooldown_(std::max(cooldown, std::chrono::milliseconds{1})) {}

bool AuthenticationRateLimiter::allowAttempt(Clock::time_point now) {
    removeExpired(now);
    return now >= blockedUntil_;
}

void AuthenticationRateLimiter::recordFailure(Clock::time_point now) {
    removeExpired(now);
    failures_.push_back(now);
    if (failures_.size() >= maximumFailures_) {
        blockedUntil_ = now + cooldown_;
        // A completed cooldown starts a fresh failure window instead of
        // immediately blocking again because of old attempts.
        failures_.clear();
    }
}

void AuthenticationRateLimiter::recordSuccess() {
    failures_.clear();
    blockedUntil_ = Clock::time_point{};
}

void AuthenticationRateLimiter::removeExpired(Clock::time_point now) {
    while (!failures_.empty() &&
           now - failures_.front() >= failureWindow_) {
        failures_.pop_front();
    }
}

} // namespace nexus::security
