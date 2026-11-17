#include "nexus/logging.hpp"

#include <ostream>

namespace nexus::logging {

void writeEvent(std::ostream& output, std::string_view component,
                std::uint64_t tunnelId,
                std::optional<std::uint32_t> streamId,
                std::string_view message) {
    output << '[' << component << " tunnel=" << tunnelId;
    if (streamId) {
        output << " stream=" << *streamId;
    }
    output << "] " << message << std::endl;
}

} // namespace nexus::logging
