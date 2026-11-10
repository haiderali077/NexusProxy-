#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace nexus::security {

std::optional<std::string> loadSecret(
    std::string_view valueEnvironmentName,
    std::string_view fileEnvironmentName, std::size_t minimumBytes,
    std::size_t maximumBytes, std::string& error);

} // namespace nexus::security
