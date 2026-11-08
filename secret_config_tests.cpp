#include "secret_config.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* kValueName = "NEXUS_TEST_SECRET";
constexpr const char* kFileName = "NEXUS_TEST_SECRET_FILE";

void clearEnvironment() {
    unsetenv(kValueName);
    unsetenv(kFileName);
}

bool writeAll(int descriptor, const std::string& value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t written =
            write(descriptor, value.data() + offset, value.size() - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace

int main() {
    clearEnvironment();
    std::string error;
    const std::string expected(32, 'x');

    setenv(kValueName, expected.c_str(), 1);
    auto direct = nexus::security::loadSecret(kValueName, kFileName, 32, 64,
                                               error);
    if (!direct || *direct != expected) {
        std::cerr << "Direct secret did not load" << std::endl;
        return 1;
    }

    char path[] = "/tmp/nexus-secret-XXXXXX";
    const int descriptor = mkstemp(path);
    if (descriptor == -1 || !writeAll(descriptor, expected + "\n")) {
        std::cerr << "Could not create temporary secret file" << std::endl;
        return 1;
    }
    close(descriptor);

    unsetenv(kValueName);
    setenv(kFileName, path, 1);
    auto fromFile = nexus::security::loadSecret(kValueName, kFileName, 32, 64,
                                                 error);
    unlink(path);
    if (!fromFile || *fromFile != expected) {
        std::cerr << "File secret did not load or trim its newline" << std::endl;
        return 1;
    }

    setenv(kValueName, "too-short", 1);
    unsetenv(kFileName);
    error.clear();
    if (nexus::security::loadSecret(kValueName, kFileName, 32, 64, error) ||
        error.find("too-short") != std::string::npos) {
        std::cerr << "Invalid secret was accepted or exposed in an error"
                  << std::endl;
        return 1;
    }

    setenv(kFileName, "/unused", 1);
    error.clear();
    if (nexus::security::loadSecret(kValueName, kFileName, 1, 64, error)) {
        std::cerr << "Ambiguous secret configuration was accepted" << std::endl;
        return 1;
    }

    clearEnvironment();
    std::cout << "Environment and file secret loading passed" << std::endl;
    return 0;
}
