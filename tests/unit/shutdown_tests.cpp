#include "nexus/shutdown.hpp"

#include <csignal>
#include <iostream>

int main() {
    if (!nexus::process::installShutdownHandlers()) {
        std::cerr << "Could not install shutdown handlers" << std::endl;
        return 1;
    }
    if (nexus::process::shutdownRequested()) {
        std::cerr << "Shutdown was requested before a signal" << std::endl;
        return 1;
    }

    std::raise(SIGTERM);
    if (!nexus::process::shutdownRequested()) {
        std::cerr << "SIGTERM did not request shutdown" << std::endl;
        return 1;
    }

    std::cout << "Shutdown signal test passed" << std::endl;
    return 0;
}
