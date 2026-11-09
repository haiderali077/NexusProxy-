#include "auth_rate_limiter.hpp"

#include <chrono>
#include <iostream>

int main() {
    using namespace std::chrono_literals;
    using Limiter = nexus::security::AuthenticationRateLimiter;

    Limiter limiter{3, 10s, 2s};
    const auto start = Limiter::Clock::time_point{} + 100s;
    if (!limiter.allowAttempt(start)) {
        std::cerr << "Fresh limiter rejected an attempt" << std::endl;
        return 1;
    }

    limiter.recordFailure(start);
    limiter.recordFailure(start + 1s);
    if (!limiter.allowAttempt(start + 1s)) {
        std::cerr << "Limiter blocked before reaching its threshold"
                  << std::endl;
        return 1;
    }
    limiter.recordFailure(start + 2s);
    if (limiter.allowAttempt(start + 2500ms)) {
        std::cerr << "Limiter did not enforce its cooldown" << std::endl;
        return 1;
    }
    if (!limiter.allowAttempt(start + 4s)) {
        std::cerr << "Limiter did not recover after cooldown" << std::endl;
        return 1;
    }

    limiter.recordFailure(start + 5s);
    limiter.recordSuccess();
    if (!limiter.allowAttempt(start + 5s)) {
        std::cerr << "Successful authentication did not reset failures"
                  << std::endl;
        return 1;
    }

    std::cout << "Authentication rate limiter behavior passed" << std::endl;
    return 0;
}
