#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string_view>

namespace nexus::logging {

// One shared prefix makes the same logical stream searchable on both programs.
void writeEvent(std::ostream& output, std::string_view component,
                std::uint64_t tunnelId,
                std::optional<std::uint32_t> streamId,
                std::string_view message);

} // namespace nexus::logging
