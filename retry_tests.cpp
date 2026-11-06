#include "retry.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using nexus::reliability::ReconnectBackoff;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void testGrowthAndMaximum() {
  ReconnectBackoff backoff{100ms, 800ms, 42};

  const auto first = backoff.nextDelay();
  expect(first >= 50ms && first <= 100ms, "first jitter is outside its range");
  expect(backoff.currentCeiling() == 200ms, "first failure did not double");

  backoff.nextDelay();
  expect(backoff.currentCeiling() == 400ms, "second failure did not double");
  backoff.nextDelay();
  expect(backoff.currentCeiling() == 800ms, "maximum was not reached");
  backoff.nextDelay();
  expect(backoff.currentCeiling() == 800ms, "delay exceeded maximum");
}

void testReset() {
  ReconnectBackoff backoff{100ms, 800ms, 7};
  backoff.nextDelay();
  backoff.nextDelay();
  backoff.reset();
  expect(backoff.currentCeiling() == 100ms,
         "reset did not restore initial delay");
}

void testInvalidDurationsAreNormalized() {
  ReconnectBackoff backoff{std::chrono::milliseconds{0},
                           std::chrono::milliseconds{0}, 1};
  expect(backoff.currentCeiling() == 1ms, "zero durations were not normalized");
  expect(backoff.nextDelay() >= 0ms,
         "normalized backoff returned a negative delay");
}

} // namespace

int main() {
  try {
    testGrowthAndMaximum();
    testReset();
    testInvalidDurationsAreNormalized();
  } catch (const std::exception &error) {
    std::cerr << "Retry test failed: " << error.what() << std::endl;
    return 1;
  }

  std::cout << "All retry tests passed" << std::endl;
  return 0;
}
