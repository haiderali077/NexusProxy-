#include "tls_context.hpp"

#include <iostream>
#include <string>
#include <utility>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: tls_context_tests <certificate> <private-key>"
                  << std::endl;
        return 2;
    }

    std::string error;
    auto server = nexus::security::TlsContext::createServer(argv[1], argv[2],
                                                             error);
    if (!server) {
        std::cerr << "Valid server context failed: " << error << std::endl;
        return 1;
    }

    auto client = nexus::security::TlsContext::createClient(argv[1], error);
    if (!client) {
        std::cerr << "Valid client context failed: " << error << std::endl;
        return 1;
    }

    nexus::security::TlsContext movedServer{std::move(*server)};
    if (!movedServer.valid() || server->valid()) {
        std::cerr << "Moving a TLS context did not transfer ownership"
                  << std::endl;
        return 1;
    }

    error.clear();
    const auto missing = nexus::security::TlsContext::createServer(
        "/file/that/does/not/exist", argv[2], error);
    if (missing || error.empty()) {
        std::cerr << "Missing certificate was not reported" << std::endl;
        return 1;
    }

    std::cout << "TLS context ownership and configuration passed" << std::endl;
    return 0;
}
