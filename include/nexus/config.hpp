#pragma once

#include <string_view>

namespace nexus::config {

bool parsePort(std::string_view text, int& port);

} // namespace nexus::config
