#include "retry.hpp"

#include <algorithm>

namespace nexus::reliability {
namespace {

std::chrono::milliseconds
atLeastOneMillisecond(std::chrono::milliseconds value) {
  return std::max(value, std::chrono::milliseconds{1});
}

} // namespace

ReconnectBackoff::ReconnectBackoff(std::chrono::milliseconds initialDelay,
                                   std::chrono::milliseconds maximumDelay)
    : ReconnectBackoff(initialDelay, maximumDelay, std::random_device{}()) {}

ReconnectBackoff::ReconnectBackoff(std::chrono::milliseconds initialDelay,
                                   std::chrono::milliseconds maximumDelay,
                                   std::uint32_t randomSeed)
    : initialDelay_(atLeastOneMillisecond(initialDelay)),
      maximumDelay_(std::max(initialDelay_, maximumDelay)),
      currentDelay_(initialDelay_), randomGenerator_(randomSeed) {}

std::chrono::milliseconds ReconnectBackoff::nextDelay() {
  const auto lowerBound = currentDelay_.count() / 2;
  std::uniform_int_distribution<std::int64_t> distribution(
      lowerBound, currentDelay_.count());
  const std::chrono::milliseconds delay{distribution(randomGenerator_)};

  // Check before multiplying so a very large duration cannot overflow.
  if (currentDelay_ > maximumDelay_ / 2) {
    currentDelay_ = maximumDelay_;
  } else {
    currentDelay_ = std::min(currentDelay_ * 2, maximumDelay_);
  }
  return delay;
}

void ReconnectBackoff::reset() { currentDelay_ = initialDelay_; }

std::chrono::milliseconds ReconnectBackoff::currentCeiling() const {
  return currentDelay_;
}

} // namespace nexus::reliability
