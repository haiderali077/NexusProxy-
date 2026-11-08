#include "secret_config.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace nexus::security {
namespace {

const char* readEnvironment(std::string_view name) {
    // getenv() requires a null-terminated name, while string_view does not
    // guarantee one. Owning this small copy makes the C API call safe.
    const std::string ownedName{name};
    return std::getenv(ownedName.c_str());
}

bool validLength(std::size_t size, std::size_t minimum,
                 std::size_t maximum) {
    return size >= minimum && size <= maximum;
}

} // namespace

std::optional<std::string> loadSecret(
    std::string_view valueEnvironmentName,
    std::string_view fileEnvironmentName, std::size_t minimumBytes,
    std::size_t maximumBytes, std::string& error) {
    const char* directValue = readEnvironment(valueEnvironmentName);
    const char* filePath = readEnvironment(fileEnvironmentName);
    const bool hasDirectValue = directValue != nullptr && *directValue != '\0';
    const bool hasFile = filePath != nullptr && *filePath != '\0';

    if (hasDirectValue == hasFile) {
        error = hasDirectValue
                    ? "configure only one of " +
                          std::string{valueEnvironmentName} + " and " +
                          std::string{fileEnvironmentName}
                    : "configure one of " + std::string{valueEnvironmentName} +
                          " or " + std::string{fileEnvironmentName};
        return std::nullopt;
    }

    std::string secret;
    if (hasDirectValue) {
        secret = directValue;
    } else {
        std::ifstream input{filePath, std::ios::binary};
        if (!input) {
            error = "could not read secret file configured by " +
                    std::string{fileEnvironmentName};
            return std::nullopt;
        }

        // Leave room for CRLF plus one extra byte. This accepts a maximum-size
        // text secret with its newline while still detecting oversized files.
        std::vector<char> bytes(maximumBytes + 3);
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const std::size_t count = static_cast<std::size_t>(input.gcount());
        secret.assign(bytes.data(), count);
        if (!secret.empty() && secret.back() == '\n') {
            secret.pop_back();
            if (!secret.empty() && secret.back() == '\r') {
                secret.pop_back();
            }
        }
        if (secret.size() > maximumBytes) {
            error = "secret configured by " + std::string{fileEnvironmentName} +
                    " exceeds the maximum length";
            return std::nullopt;
        }
    }

    if (!validLength(secret.size(), minimumBytes, maximumBytes)) {
        error = "configured secret must contain between " +
                std::to_string(minimumBytes) + " and " +
                std::to_string(maximumBytes) + " bytes";
        return std::nullopt;
    }
    return secret;
}

} // namespace nexus::security
