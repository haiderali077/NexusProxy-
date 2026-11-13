#include "nexus/shutdown.hpp"

#include <csignal>

namespace nexus::process {
namespace {

volatile std::sig_atomic_t stopRequested = 0;

void requestShutdown(int) {
    // Assignment to sig_atomic_t is safe inside a signal handler. Logging,
    // allocation, and socket cleanup are deliberately left to the main loop.
    stopRequested = 1;
}

} // namespace

bool installShutdownHandlers() {
    stopRequested = 0;
    return std::signal(SIGINT, requestShutdown) != SIG_ERR &&
           std::signal(SIGTERM, requestShutdown) != SIG_ERR;
}

bool shutdownRequested() {
    return stopRequested != 0;
}

} // namespace nexus::process
