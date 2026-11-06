#pragma once

#include <chrono>
#include <cstdint>
#include <random>

namespace nexus::reliability {

class ReconnectBackoff {
public:
  ReconnectBackoff(std::chrono::milliseconds initialDelay,
                   std::chrono::milliseconds maximumDelay);
  ReconnectBackoff(std::chrono::milliseconds initialDelay,
                   std::chrono::milliseconds maximumDelay,
                   std::uint32_t randomSeed);

  // Returns a randomized delay, then doubles the ceiling for the next failure.
  std::chrono::milliseconds nextDelay();
  void reset();

  std::chrono::milliseconds currentCeiling() const;

private:
  std::chrono::milliseconds initialDelay_;
  std::chrono::milliseconds maximumDelay_;
  std::chrono::milliseconds currentDelay_;
  std::mt19937 randomGenerator_;
};

} // namespace nexus::reliability
