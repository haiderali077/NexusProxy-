#include "config.hpp"

#include <charconv>

namespace nexus::config {

bool parsePort(std::string_view text, int& port) {
    int parsed{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() ||
        parsed < 1 || parsed > 65'535) {
        return false;
    }

    port = parsed;
    return true;
}

} // namespace nexus::config
